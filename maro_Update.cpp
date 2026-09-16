#include "maro_Update.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr wchar_t Maro_ApiHost[] = L"api.github.com";
constexpr wchar_t Maro_ApiPath[] = L"/repos/maro-comu/Maro_CLive/releases/latest";
constexpr wchar_t Maro_ReleasePrefix[] = L"https://github.com/maro-comu/Maro_CLive/releases/download/";
constexpr std::size_t Maro_MaxApiBytes = 4u << 20;
constexpr std::uint64_t Maro_MaxInstallerBytes = 256ull << 20;

bool Maro_IsCancelled(const std::atomic_bool* cancelled) noexcept
{
    return cancelled != nullptr && cancelled->load(std::memory_order_relaxed);
}

std::wstring Maro_ErrorText(std::wstring_view operation, DWORD error)
{
    std::wstring result(operation);
    result.append(L" (");
    result.append(std::to_wstring(error));
    result.push_back(L')');
    return result;
}

std::wstring Maro_Utf8ToWide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }
    const int size = static_cast<int>(text.size());
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        size,
        nullptr,
        0);
    if (required <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            size,
            result.data(),
            required) != required)
    {
        return {};
    }
    return result;
}

void Maro_AppendUtf8(std::string& output, std::uint32_t codePoint)
{
    if (codePoint <= 0x7fu)
    {
        output.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint <= 0x7ffu)
    {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
    else if (codePoint <= 0xffffu)
    {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
    else
    {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

class Maro_JsonReader
{
public:
    explicit Maro_JsonReader(std::string_view source) noexcept : source_(source) {}

    bool End() noexcept
    {
        Space();
        return position_ == source_.size();
    }

    bool Take(char value) noexcept
    {
        Space();
        if (position_ >= source_.size() || source_[position_] != value)
        {
            return false;
        }
        ++position_;
        return true;
    }

    bool String(std::string& value)
    {
        Space();
        if (position_ >= source_.size() || source_[position_] != '"')
        {
            return false;
        }
        ++position_;
        value.clear();
        while (position_ < source_.size())
        {
            const unsigned char character = static_cast<unsigned char>(source_[position_++]);
            if (character == '"')
            {
                return true;
            }
            if (character < 0x20u)
            {
                return false;
            }
            if (character != '\\')
            {
                value.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= source_.size())
            {
                return false;
            }
            const char escape = source_[position_++];
            if (escape == '"' || escape == '\\' || escape == '/')
            {
                value.push_back(escape);
            }
            else if (escape == 'b')
            {
                value.push_back('\b');
            }
            else if (escape == 'f')
            {
                value.push_back('\f');
            }
            else if (escape == 'n')
            {
                value.push_back('\n');
            }
            else if (escape == 'r')
            {
                value.push_back('\r');
            }
            else if (escape == 't')
            {
                value.push_back('\t');
            }
            else if (escape == 'u')
            {
                std::uint32_t first = 0;
                if (!Hex4(first))
                {
                    return false;
                }
                std::uint32_t codePoint = first;
                if (first >= 0xd800u && first <= 0xdbffu)
                {
                    if (position_ + 2 > source_.size() ||
                        source_[position_] != '\\' || source_[position_ + 1] != 'u')
                    {
                        return false;
                    }
                    position_ += 2;
                    std::uint32_t second = 0;
                    if (!Hex4(second) || second < 0xdc00u || second > 0xdfffu)
                    {
                        return false;
                    }
                    codePoint = 0x10000u + ((first - 0xd800u) << 10) + (second - 0xdc00u);
                }
                else if (first >= 0xdc00u && first <= 0xdfffu)
                {
                    return false;
                }
                Maro_AppendUtf8(value, codePoint);
            }
            else
            {
                return false;
            }
        }
        return false;
    }

    bool Boolean(bool& value) noexcept
    {
        Space();
        if (source_.substr(position_, 4) == "true")
        {
            position_ += 4;
            value = true;
            return true;
        }
        if (source_.substr(position_, 5) == "false")
        {
            position_ += 5;
            value = false;
            return true;
        }
        return false;
    }

    bool Unsigned(std::uint64_t& value) noexcept
    {
        Space();
        if (position_ >= source_.size() || source_[position_] < '0' || source_[position_] > '9')
        {
            return false;
        }
        const char* begin = source_.data() + position_;
        const char* end = source_.data() + source_.size();
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc() || (parsed.ptr - begin > 1 && *begin == '0'))
        {
            return false;
        }
        position_ = static_cast<std::size_t>(parsed.ptr - source_.data());
        return position_ == source_.size() ||
            (source_[position_] != '.' && source_[position_] != 'e' && source_[position_] != 'E');
    }

    bool Null() noexcept
    {
        Space();
        if (source_.substr(position_, 4) != "null")
        {
            return false;
        }
        position_ += 4;
        return true;
    }

    bool Skip(std::size_t depth = 0)
    {
        if (depth > 64)
        {
            return false;
        }
        Space();
        if (position_ >= source_.size())
        {
            return false;
        }
        if (source_[position_] == '"')
        {
            std::string unused;
            return String(unused);
        }
        if (source_[position_] == '{')
        {
            ++position_;
            Space();
            if (position_ < source_.size() && source_[position_] == '}')
            {
                ++position_;
                return true;
            }
            for (;;)
            {
                std::string key;
                if (!String(key) || !Take(':') || !Skip(depth + 1))
                {
                    return false;
                }
                if (Take('}'))
                {
                    return true;
                }
                if (!Take(','))
                {
                    return false;
                }
            }
        }
        if (source_[position_] == '[')
        {
            ++position_;
            Space();
            if (position_ < source_.size() && source_[position_] == ']')
            {
                ++position_;
                return true;
            }
            for (;;)
            {
                if (!Skip(depth + 1))
                {
                    return false;
                }
                if (Take(']'))
                {
                    return true;
                }
                if (!Take(','))
                {
                    return false;
                }
            }
        }
        bool booleanValue = false;
        if (Boolean(booleanValue) || Null())
        {
            return true;
        }
        const std::size_t begin = position_;
        if (source_[position_] == '-')
        {
            ++position_;
        }
        if (position_ >= source_.size())
        {
            return false;
        }
        if (source_[position_] == '0')
        {
            ++position_;
        }
        else if (source_[position_] >= '1' && source_[position_] <= '9')
        {
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9')
            {
                ++position_;
            }
        }
        else
        {
            position_ = begin;
            return false;
        }
        if (position_ < source_.size() && source_[position_] == '.')
        {
            ++position_;
            const std::size_t digits = position_;
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9')
            {
                ++position_;
            }
            if (digits == position_)
            {
                return false;
            }
        }
        if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E'))
        {
            ++position_;
            if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-'))
            {
                ++position_;
            }
            const std::size_t digits = position_;
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9')
            {
                ++position_;
            }
            if (digits == position_)
            {
                return false;
            }
        }
        return position_ != begin;
    }

private:
    void Space() noexcept
    {
        while (position_ < source_.size())
        {
            const char value = source_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
            {
                break;
            }
            ++position_;
        }
    }

    bool Hex4(std::uint32_t& value) noexcept
    {
        if (position_ + 4 > source_.size())
        {
            return false;
        }
        value = 0;
        for (std::size_t index = 0; index < 4; ++index)
        {
            const unsigned char character = static_cast<unsigned char>(source_[position_++]);
            std::uint32_t digit = 0;
            if (character >= '0' && character <= '9')
            {
                digit = character - '0';
            }
            else if (character >= 'a' && character <= 'f')
            {
                digit = character - 'a' + 10u;
            }
            else if (character >= 'A' && character <= 'F')
            {
                digit = character - 'A' + 10u;
            }
            else
            {
                return false;
            }
            value = (value << 4) | digit;
        }
        return true;
    }

    std::string_view source_;
    std::size_t position_ = 0;
};

struct Maro_RawAsset
{
    std::string name;
    std::string url;
    std::string digest;
    std::uint64_t size = 0;
    bool hasName = false;
    bool hasUrl = false;
    bool hasDigest = false;
    bool hasSize = false;
};

bool Maro_ReadRawAsset(Maro_JsonReader& reader, Maro_RawAsset& asset)
{
    if (!reader.Take('{'))
    {
        return false;
    }
    if (reader.Take('}'))
    {
        return true;
    }
    for (;;)
    {
        std::string key;
        if (!reader.String(key) || !reader.Take(':'))
        {
            return false;
        }
        if (key == "name")
        {
            asset.hasName = reader.String(asset.name);
            if (!asset.hasName)
            {
                return false;
            }
        }
        else if (key == "browser_download_url")
        {
            asset.hasUrl = reader.String(asset.url);
            if (!asset.hasUrl)
            {
                return false;
            }
        }
        else if (key == "digest")
        {
            asset.hasDigest = reader.String(asset.digest);
            if (!asset.hasDigest && !reader.Null())
            {
                return false;
            }
        }
        else if (key == "size")
        {
            asset.hasSize = reader.Unsigned(asset.size);
            if (!asset.hasSize)
            {
                return false;
            }
        }
        else if (!reader.Skip())
        {
            return false;
        }
        if (reader.Take('}'))
        {
            return true;
        }
        if (!reader.Take(','))
        {
            return false;
        }
    }
}

bool Maro_ReadAssets(Maro_JsonReader& reader, std::vector<Maro_RawAsset>& assets)
{
    if (!reader.Take('['))
    {
        return false;
    }
    if (reader.Take(']'))
    {
        return true;
    }
    for (;;)
    {
        Maro_RawAsset asset;
        if (!Maro_ReadRawAsset(reader, asset))
        {
            return false;
        }
        assets.push_back(std::move(asset));
        if (reader.Take(']'))
        {
            return true;
        }
        if (!reader.Take(','))
        {
            return false;
        }
    }
}

int Maro_HexDigit(char character) noexcept
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}

bool Maro_ParseDigest(std::string_view digest, std::array<std::uint8_t, 32>& result) noexcept
{
    constexpr std::string_view prefix = "sha256:";
    if (!digest.starts_with(prefix) || digest.size() != prefix.size() + result.size() * 2)
    {
        return false;
    }
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const int high = Maro_HexDigit(digest[prefix.size() + index * 2]);
        const int low = Maro_HexDigit(digest[prefix.size() + index * 2 + 1]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        result[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool Maro_ValidDownloadUrl(
    std::wstring_view url,
    std::wstring_view tag,
    std::wstring_view fileName)
{
    std::wstring expected(Maro_ReleasePrefix);
    expected.append(tag);
    expected.push_back(L'/');
    expected.append(fileName);
    return url == expected;
}

class Maro_DownloadCleanup
{
public:
    Maro_DownloadCleanup(std::wstring& directory, std::wstring& path) noexcept
        : directory_(directory), path_(path) {}
    ~Maro_DownloadCleanup() noexcept
    {
        if (!keep)
        {
            DeleteFileW(path_.c_str());
            RemoveDirectoryW(directory_.c_str());
            path_.clear();
            directory_.clear();
        }
    }
    Maro_DownloadCleanup(const Maro_DownloadCleanup&) = delete;
    Maro_DownloadCleanup& operator=(const Maro_DownloadCleanup&) = delete;
    bool keep = false;

private:
    std::wstring& directory_;
    std::wstring& path_;
};

class Maro_InternetHandle
{
public:
    Maro_InternetHandle() noexcept = default;
    explicit Maro_InternetHandle(HINTERNET value) noexcept : value_(value) {}
    ~Maro_InternetHandle() noexcept
    {
        Reset();
    }
    Maro_InternetHandle(const Maro_InternetHandle&) = delete;
    Maro_InternetHandle& operator=(const Maro_InternetHandle&) = delete;
    Maro_InternetHandle(Maro_InternetHandle&& other) noexcept : value_(other.Release()) {}
    Maro_InternetHandle& operator=(Maro_InternetHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset(other.Release());
        }
        return *this;
    }
    HINTERNET Get() const noexcept
    {
        return value_;
    }
    explicit operator bool() const noexcept
    {
        return value_ != nullptr;
    }
    HINTERNET Release() noexcept
    {
        const HINTERNET value = value_;
        value_ = nullptr;
        return value;
    }
    void Reset(HINTERNET value = nullptr) noexcept
    {
        if (value_ != nullptr)
        {
            WinHttpCloseHandle(value_);
        }
        value_ = value;
    }

private:
    HINTERNET value_ = nullptr;
};

class Maro_FileHandle
{
public:
    explicit Maro_FileHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~Maro_FileHandle() noexcept
    {
        Reset();
    }
    void Reset() noexcept
    {
        if (value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
            value_ = INVALID_HANDLE_VALUE;
        }
    }
    Maro_FileHandle(const Maro_FileHandle&) = delete;
    Maro_FileHandle& operator=(const Maro_FileHandle&) = delete;
    HANDLE Get() const noexcept
    {
        return value_;
    }
    explicit operator bool() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class Maro_AlgorithmHandle
{
public:
    ~Maro_AlgorithmHandle() noexcept
    {
        if (value_ != nullptr)
        {
            BCryptCloseAlgorithmProvider(value_, 0);
        }
    }
    BCRYPT_ALG_HANDLE* Put() noexcept
    {
        return &value_;
    }
    BCRYPT_ALG_HANDLE Get() const noexcept
    {
        return value_;
    }

private:
    BCRYPT_ALG_HANDLE value_ = nullptr;
};

class Maro_HashHandle
{
public:
    ~Maro_HashHandle() noexcept
    {
        if (value_ != nullptr)
        {
            BCryptDestroyHash(value_);
        }
    }
    BCRYPT_HASH_HANDLE* Put() noexcept
    {
        return &value_;
    }
    BCRYPT_HASH_HANDLE Get() const noexcept
    {
        return value_;
    }

private:
    BCRYPT_HASH_HANDLE value_ = nullptr;
};

struct Maro_HttpRequest
{
    Maro_HttpRequest() noexcept = default;
    Maro_HttpRequest(const Maro_HttpRequest&) = delete;
    Maro_HttpRequest& operator=(const Maro_HttpRequest&) = delete;
    ~Maro_HttpRequest() noexcept
    {
        Close();
        if (completed != nullptr)
        {
            CloseHandle(completed);
        }
        if (closed != nullptr)
        {
            CloseHandle(closed);
        }
    }

    static void CALLBACK Callback(
        HINTERNET,
        DWORD_PTR context,
        DWORD status,
        void* information,
        DWORD size) noexcept
    {
        auto* state = reinterpret_cast<Maro_HttpRequest*>(context);
        if (state == nullptr)
        {
            return;
        }
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
        {
            SetEvent(state->closed);
        }
        else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
        {
            const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(information);
            state->failure.store(result->dwError, std::memory_order_release);
            SetEvent(state->completed);
        }
        else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
            status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
            status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
        {
            state->bytes.store(
                status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ? size : 0,
                std::memory_order_release);
            SetEvent(state->completed);
        }
    }

    bool Initialize(std::wstring& error)
    {
        if ((completed = CreateEventW(nullptr, TRUE, FALSE, nullptr)) == nullptr ||
            (closed = CreateEventW(nullptr, TRUE, FALSE, nullptr)) == nullptr)
        {
            error = Maro_ErrorText(L"업데이트 대기를 준비할 수 없습니다.", GetLastError());
            return false;
        }
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(request.Get(), WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
            WinHttpSetStatusCallback(
                request.Get(),
                Callback,
                WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
                    WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
                    WINHTTP_CALLBACK_FLAG_HANDLES,
                0) == WINHTTP_INVALID_STATUS_CALLBACK)
        {
            error = Maro_ErrorText(L"업데이트 콜백을 설정할 수 없습니다.", GetLastError());
            return false;
        }
        callbackAttached = true;
        return true;
    }

    void Close() noexcept
    {
        if (request)
        {
            WinHttpCloseHandle(request.Release());
            if (callbackAttached)
            {
                WaitForSingleObject(closed, INFINITE);
            }
        }
    }

    void Prepare() noexcept
    {
        ResetEvent(completed);
        failure.store(ERROR_SUCCESS, std::memory_order_relaxed);
        bytes.store(0, std::memory_order_relaxed);
    }

    bool Await(
        BOOL started,
        const std::atomic_bool* cancelled,
        std::wstring_view operation,
        std::wstring& error)
    {
        const DWORD startError = started ? ERROR_SUCCESS : GetLastError();
        if (startError != ERROR_SUCCESS && startError != ERROR_IO_PENDING)
        {
            error = Maro_ErrorText(operation, startError);
            return false;
        }
        for (;;)
        {
            if (Maro_IsCancelled(cancelled))
            {
                Close();
                error.clear();
                return false;
            }
            const DWORD waited = WaitForSingleObject(completed, 50);
            if (waited == WAIT_OBJECT_0)
            {
                const DWORD result = failure.load(std::memory_order_acquire);
                if (result != ERROR_SUCCESS)
                {
                    error = Maro_ErrorText(operation, result);
                    return false;
                }
                return true;
            }
            if (waited != WAIT_TIMEOUT)
            {
                error = Maro_ErrorText(operation, GetLastError());
                return false;
            }
        }
    }

    bool Read(const std::atomic_bool* cancelled, DWORD& read, std::wstring& error)
    {
        Prepare();
        if (!Await(
                WinHttpReadData(request.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), nullptr),
                cancelled,
                L"업데이트 응답을 읽을 수 없습니다.",
                error))
        {
            return false;
        }
        read = bytes.load(std::memory_order_acquire);
        return true;
    }

    Maro_InternetHandle session;
    Maro_InternetHandle connection;
    Maro_InternetHandle request;
    std::array<std::uint8_t, 64u << 10> buffer{};
    HANDLE completed = nullptr;
    HANDLE closed = nullptr;
    std::atomic<DWORD> failure{ERROR_SUCCESS};
    std::atomic<DWORD> bytes{0};
    bool callbackAttached = false;
};

bool Maro_OpenRequest(
    std::wstring_view url,
    bool githubApi,
    Maro_HttpRequest& handles,
    const std::atomic_bool* cancelled,
    std::wstring& error)
{
    if (Maro_IsCancelled(cancelled))
    {
        error.clear();
        return false;
    }
    std::wstring ownedUrl(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(ownedUrl.c_str(), 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.lpszHostName == nullptr || components.dwHostNameLength == 0)
    {
        error = L"HTTPS URL이 올바르지 않습니다.";
        return false;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path;
    if (components.lpszUrlPath != nullptr)
    {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo != nullptr)
    {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty())
    {
        path = L"/";
    }

    handles.session.Reset(WinHttpOpen(
        L"CLive_Maro-Updater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        WINHTTP_FLAG_ASYNC));
    if (!handles.session)
    {
        error = Maro_ErrorText(L"WinHTTP 세션을 열 수 없습니다.", GetLastError());
        return false;
    }
    if (!WinHttpSetTimeouts(handles.session.Get(), 10'000, 10'000, 15'000, 15'000))
    {
        error = Maro_ErrorText(L"연결 제한 시간을 설정할 수 없습니다.", GetLastError());
        return false;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetOption(
        handles.session.Get(),
        WINHTTP_OPTION_SECURE_PROTOCOLS,
        &protocols,
        sizeof(protocols)))
    {
        error = Maro_ErrorText(L"HTTPS 보안을 설정할 수 없습니다.", GetLastError());
        return false;
    }

    handles.connection.Reset(WinHttpConnect(
        handles.session.Get(),
        host.c_str(),
        components.nPort,
        0));
    if (!handles.connection)
    {
        error = Maro_ErrorText(L"서버에 연결할 수 없습니다.", GetLastError());
        return false;
    }

    handles.request.Reset(WinHttpOpenRequest(
        handles.connection.Get(),
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!handles.request)
    {
        error = Maro_ErrorText(L"요청을 만들 수 없습니다.", GetLastError());
        return false;
    }
    if (!handles.Initialize(error))
    {
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(
        handles.request.Get(),
        WINHTTP_OPTION_REDIRECT_POLICY,
        &redirectPolicy,
        sizeof(redirectPolicy)))
    {
        error = Maro_ErrorText(L"HTTPS 이동 정책을 설정할 수 없습니다.", GetLastError());
        return false;
    }
    DWORD redirects = 5;
    if (!WinHttpSetOption(
        handles.request.Get(),
        WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
        &redirects,
        sizeof(redirects)))
    {
        error = Maro_ErrorText(L"HTTPS 이동 횟수를 설정할 수 없습니다.", GetLastError());
        return false;
    }

    const wchar_t* headers = githubApi
        ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
        : L"Accept: application/octet-stream\r\n";
    handles.Prepare();
    if (!handles.Await(WinHttpSendRequest(
            handles.request.Get(),
            headers,
            static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            reinterpret_cast<DWORD_PTR>(&handles)),
            cancelled,
            L"HTTPS 요청에 실패했습니다.",
            error))
    {
        return false;
    }
    handles.Prepare();
    if (!handles.Await(
            WinHttpReceiveResponse(handles.request.Get(), nullptr),
            cancelled,
            L"HTTPS 응답을 받을 수 없습니다.",
            error))
    {
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(
            handles.request.Get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) || status != HTTP_STATUS_OK)
    {
        error = L"서버 응답 코드가 올바르지 않습니다.";
        if (status != 0)
        {
            error.append(L" (");
            error.append(std::to_wstring(status));
            error.push_back(L')');
        }
        return false;
    }
    return true;
}

bool Maro_ReadApi(
    const std::atomic_bool* cancelled,
    std::string& response,
    std::wstring& error)
{
    Maro_HttpRequest handles;
    std::wstring url = L"https://";
    url.append(Maro_ApiHost);
    url.append(Maro_ApiPath);
    if (!Maro_OpenRequest(url, true, handles, cancelled, error))
    {
        return false;
    }
    response.clear();
    for (;;)
    {
        if (Maro_IsCancelled(cancelled))
        {
            error.clear();
            return false;
        }
        DWORD read = 0;
        if (!handles.Read(cancelled, read, error))
        {
            return false;
        }
        if (read == 0)
        {
            return true;
        }
        if (response.size() > Maro_MaxApiBytes - read)
        {
            error = L"업데이트 응답이 너무 큽니다.";
            return false;
        }
        response.append(reinterpret_cast<const char*>(handles.buffer.data()), read);
    }
}

bool Maro_CreateTemporaryTarget(
    std::wstring_view fileName,
    std::wstring& directory,
    std::wstring& path,
    std::wstring& error)
{
    std::vector<wchar_t> temporary(MAX_PATH + 1, L'\0');
    DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
    if (length == 0)
    {
        error = Maro_ErrorText(L"임시 경로를 찾을 수 없습니다.", GetLastError());
        return false;
    }
    if (length >= temporary.size())
    {
        temporary.assign(static_cast<std::size_t>(length) + 1, L'\0');
        length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0 || length >= temporary.size())
        {
            error = Maro_ErrorText(L"임시 경로를 찾을 수 없습니다.", GetLastError());
            return false;
        }
    }
    const std::wstring root(temporary.data(), length);
    for (std::uint32_t attempt = 0; attempt < 64; ++attempt)
    {
        std::array<std::uint8_t, 8> random{};
        if (BCryptGenRandom(
                nullptr,
                random.data(),
                static_cast<ULONG>(random.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        {
            error = L"임시 경로 식별자를 만들 수 없습니다.";
            return false;
        }
        constexpr wchar_t digits[] = L"0123456789abcdef";
        std::wstring suffix;
        suffix.reserve(random.size() * 2);
        for (const std::uint8_t value : random)
        {
            suffix.push_back(digits[value >> 4]);
            suffix.push_back(digits[value & 0x0fu]);
        }
        directory = root + L"maro_Update_" + suffix;
        if (CreateDirectoryW(directory.c_str(), nullptr))
        {
            path = directory + L"\\" + std::wstring(fileName);
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            error = Maro_ErrorText(L"임시 폴더를 만들 수 없습니다.", GetLastError());
            return false;
        }
    }
    error = L"고유한 임시 폴더를 만들 수 없습니다.";
    return false;
}

bool Maro_WriteAll(HANDLE file, const std::uint8_t* data, DWORD size, std::wstring& error)
{
    DWORD offset = 0;
    while (offset < size)
    {
        DWORD written = 0;
        if (!WriteFile(file, data + offset, size - offset, &written, nullptr) || written == 0)
        {
            error = Maro_ErrorText(L"업데이트 파일을 저장할 수 없습니다.", GetLastError());
            return false;
        }
        offset += written;
    }
    return true;
}

bool Maro_DownloadVerified(
    const Maro_UpdateRelease& release,
    const std::atomic_bool* cancelled,
    const Maro_UpdateProgress& progress,
    std::wstring& directory,
    std::wstring& path,
    std::wstring& error)
{
    const std::wstring tag = Maro_Utf8ToWide(release.tag);
    const auto version = maro_ParseSemanticVersion(release.tag);
    const std::wstring expectedName = L"maro_CLive_Maro_" + tag + L".exe";
    if (!version || maro_CompareSemanticVersions(*version, release.version) != 0 ||
        release.asset.fileName != expectedName || !Maro_ValidDownloadUrl(
            release.asset.downloadUrl,
            tag,
            release.asset.fileName) || release.asset.size == 0 ||
        release.asset.size > Maro_MaxInstallerBytes)
    {
        error = L"업데이트 파일 정보가 올바르지 않습니다.";
        return false;
    }
    if (!Maro_CreateTemporaryTarget(release.asset.fileName, directory, path, error))
    {
        return false;
    }

    Maro_DownloadCleanup cleanup(directory, path);

    Maro_HttpRequest handles;
    if (!Maro_OpenRequest(release.asset.downloadUrl, false, handles, cancelled, error))
    {
        return false;
    }
    if (Maro_IsCancelled(cancelled))
    {
        error.clear();
        return false;
    }

    Maro_FileHandle file(CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file)
    {
        error = Maro_ErrorText(L"업데이트 파일을 만들 수 없습니다.", GetLastError());
        return false;
    }

    Maro_AlgorithmHandle algorithm;
    if (BCryptOpenAlgorithmProvider(algorithm.Put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    {
        error = L"SHA-256 검증을 시작할 수 없습니다.";
        return false;
    }
    DWORD objectSize = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(
            algorithm.Get(),
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize),
            &copied,
            0) < 0 || copied != sizeof(objectSize))
    {
        error = L"SHA-256 정보를 읽을 수 없습니다.";
        return false;
    }
    std::vector<std::uint8_t> hashObject(objectSize);
    Maro_HashHandle hash;
    if (BCryptCreateHash(
            algorithm.Get(),
            hash.Put(),
            hashObject.data(),
            static_cast<ULONG>(hashObject.size()),
            nullptr,
            0,
            0) < 0)
    {
        error = L"SHA-256 검증을 시작할 수 없습니다.";
        return false;
    }

    std::uint64_t total = 0;
    if (progress)
    {
        progress(0, release.asset.size);
    }
    for (;;)
    {
        if (Maro_IsCancelled(cancelled))
        {
            error.clear();
            return false;
        }
        DWORD read = 0;
        if (!handles.Read(cancelled, read, error))
        {
            return false;
        }
        if (read == 0)
        {
            break;
        }
        if (total > release.asset.size || read > release.asset.size - total)
        {
            error = L"업데이트 파일 크기가 릴리스 정보와 다릅니다.";
            return false;
        }
        if (!Maro_WriteAll(file.Get(), handles.buffer.data(), read, error))
        {
            return false;
        }
        if (BCryptHashData(hash.Get(), handles.buffer.data(), read, 0) < 0)
        {
            error = L"업데이트 파일 해시를 계산할 수 없습니다.";
            return false;
        }
        total += read;
        if (progress)
        {
            progress(total, release.asset.size);
        }
    }
    if (total != release.asset.size)
    {
        error = L"업데이트 파일 크기가 릴리스 정보와 다릅니다.";
        return false;
    }
    std::array<std::uint8_t, 32> actual{};
    if (BCryptFinishHash(hash.Get(), actual.data(), static_cast<ULONG>(actual.size()), 0) < 0)
    {
        error = L"업데이트 파일 해시를 완료할 수 없습니다.";
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < actual.size(); ++index)
    {
        difference |= static_cast<std::uint8_t>(actual[index] ^ release.asset.sha256[index]);
    }
    if (difference != 0)
    {
        error = L"업데이트 파일 SHA-256 검증에 실패했습니다.";
        return false;
    }
    if (!FlushFileBuffers(file.Get()))
    {
        error = Maro_ErrorText(L"업데이트 파일 저장을 완료할 수 없습니다.", GetLastError());
        return false;
    }
    file.Reset();
    cleanup.keep = true;
    return true;
}

bool Maro_LaunchInstaller(
    const std::wstring& path,
    const std::wstring& directory,
    std::wstring& error)
{
    SHELLEXECUTEINFOW information{};
    information.cbSize = sizeof(information);
    information.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    information.lpVerb = L"open";
    information.lpFile = path.c_str();
    information.lpDirectory = directory.c_str();
    information.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&information))
    {
        error = Maro_ErrorText(L"업데이트 설치 파일을 실행할 수 없습니다.", GetLastError());
        return false;
    }
    if (information.hProcess != nullptr)
    {
        CloseHandle(information.hProcess);
    }
    return true;
}
}

std::optional<Maro_SemanticVersion> maro_ParseSemanticVersion(std::string_view text) noexcept
{
    if (text.size() < 6 || text.front() != 'v')
    {
        return std::nullopt;
    }
    Maro_SemanticVersion result;
    std::array<std::uint32_t*, 3> components{&result.major, &result.minor, &result.patch};
    std::size_t position = 1;
    for (std::size_t index = 0; index < components.size(); ++index)
    {
        const std::size_t begin = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9')
        {
            ++position;
        }
        if (position == begin || (position - begin > 1 && text[begin] == '0'))
        {
            return std::nullopt;
        }
        const auto parsed = std::from_chars(
            text.data() + begin,
            text.data() + position,
            *components[index]);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + position)
        {
            return std::nullopt;
        }
        if (index + 1 < components.size())
        {
            if (position >= text.size() || text[position] != '.')
            {
                return std::nullopt;
            }
            ++position;
        }
    }
    if (position != text.size())
    {
        return std::nullopt;
    }
    return result;
}

int maro_CompareSemanticVersions(
    const Maro_SemanticVersion& left,
    const Maro_SemanticVersion& right) noexcept
{
    if (left.major != right.major)
    {
        return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor)
    {
        return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch)
    {
        return left.patch < right.patch ? -1 : 1;
    }
    return 0;
}

std::string maro_FormatSemanticVersion(const Maro_SemanticVersion& version)
{
    return "v" + std::to_string(version.major) + "." +
        std::to_string(version.minor) + "." + std::to_string(version.patch);
}

bool maro_ParseUpdateReleaseJson(
    std::string_view json,
    Maro_UpdateRelease& release,
    std::string& error)
{
    Maro_JsonReader reader(json);
    std::string tag;
    bool draft = false;
    bool prerelease = false;
    bool hasTag = false;
    bool hasDraft = false;
    bool hasPrerelease = false;
    bool hasAssets = false;
    std::vector<Maro_RawAsset> assets;
    if (!reader.Take('{'))
    {
        error = "invalid release object";
        return false;
    }
    if (!reader.Take('}'))
    {
        for (;;)
        {
            std::string key;
            if (!reader.String(key) || !reader.Take(':'))
            {
                error = "invalid release field";
                return false;
            }
            if (key == "tag_name")
            {
                hasTag = reader.String(tag);
                if (!hasTag)
                {
                    error = "invalid tag_name";
                    return false;
                }
            }
            else if (key == "draft")
            {
                hasDraft = reader.Boolean(draft);
                if (!hasDraft)
                {
                    error = "invalid draft flag";
                    return false;
                }
            }
            else if (key == "prerelease")
            {
                hasPrerelease = reader.Boolean(prerelease);
                if (!hasPrerelease)
                {
                    error = "invalid prerelease flag";
                    return false;
                }
            }
            else if (key == "assets")
            {
                hasAssets = Maro_ReadAssets(reader, assets);
                if (!hasAssets)
                {
                    error = "invalid assets";
                    return false;
                }
            }
            else if (!reader.Skip())
            {
                error = "invalid release value";
                return false;
            }
            if (reader.Take('}'))
            {
                break;
            }
            if (!reader.Take(','))
            {
                error = "invalid release separator";
                return false;
            }
        }
    }
    if (!reader.End())
    {
        error = "trailing release data";
        return false;
    }
    if (!hasTag || !hasDraft || !hasPrerelease || !hasAssets)
    {
        error = "missing release fields";
        return false;
    }
    if (draft || prerelease)
    {
        error = "draft or prerelease rejected";
        return false;
    }
    const auto version = maro_ParseSemanticVersion(tag);
    if (!version)
    {
        error = "invalid release version";
        return false;
    }
    const std::string expectedName = "maro_CLive_Maro_" + tag + ".exe";
    const Maro_RawAsset* selected = nullptr;
    for (const Maro_RawAsset& asset : assets)
    {
        if (asset.hasName && asset.name == expectedName)
        {
            if (selected != nullptr)
            {
                error = "duplicate update asset";
                return false;
            }
            selected = &asset;
        }
    }
    if (selected == nullptr || !selected->hasUrl || !selected->hasSize ||
        !selected->hasDigest || selected->size == 0 || selected->size > Maro_MaxInstallerBytes)
    {
        error = "missing update asset metadata";
        return false;
    }

    Maro_UpdateRelease parsed;
    parsed.version = *version;
    parsed.tag = tag;
    parsed.asset.fileName = Maro_Utf8ToWide(selected->name);
    parsed.asset.downloadUrl = Maro_Utf8ToWide(selected->url);
    if (parsed.asset.fileName.empty() || parsed.asset.downloadUrl.empty() ||
        !Maro_ParseDigest(selected->digest, parsed.asset.sha256))
    {
        error = "invalid update asset metadata";
        return false;
    }
    const std::wstring wideTag = Maro_Utf8ToWide(tag);
    if (!Maro_ValidDownloadUrl(parsed.asset.downloadUrl, wideTag, parsed.asset.fileName))
    {
        error = "untrusted update asset URL";
        return false;
    }
    parsed.asset.size = selected->size;
    release = std::move(parsed);
    error.clear();
    return true;
}

Maro_UpdateCheckResult maro_CheckForUpdate(
    const Maro_SemanticVersion& currentVersion,
    const std::atomic_bool* cancelled)
{
    Maro_UpdateCheckResult result;
    if (Maro_IsCancelled(cancelled))
    {
        result.status = Maro_UpdateCheckStatus::Cancelled;
        return result;
    }
    std::string json;
    if (!Maro_ReadApi(cancelled, json, result.error))
    {
        result.status = Maro_IsCancelled(cancelled)
            ? Maro_UpdateCheckStatus::Cancelled
            : Maro_UpdateCheckStatus::Failed;
        return result;
    }
    std::string parseError;
    if (!maro_ParseUpdateReleaseJson(json, result.release, parseError))
    {
        result.error = L"업데이트 정보를 확인할 수 없습니다: " + Maro_Utf8ToWide(parseError);
        result.status = Maro_UpdateCheckStatus::Failed;
        return result;
    }
    result.status = maro_CompareSemanticVersions(currentVersion, result.release.version) < 0
        ? Maro_UpdateCheckStatus::Available
        : Maro_UpdateCheckStatus::Current;
    return result;
}

Maro_UpdateInstallResult maro_DownloadUpdate(
    const Maro_UpdateRelease& release,
    const std::atomic_bool* cancelled,
    const Maro_UpdateProgress& progress)
{
    Maro_UpdateInstallResult result;
    if (Maro_IsCancelled(cancelled))
    {
        result.status = Maro_UpdateInstallStatus::Cancelled;
        return result;
    }
    std::wstring directory;
    if (!Maro_DownloadVerified(
            release,
            cancelled,
            progress,
            directory,
            result.installerPath,
            result.error))
    {
        result.status = Maro_IsCancelled(cancelled)
            ? Maro_UpdateInstallStatus::Cancelled
            : Maro_UpdateInstallStatus::Failed;
        return result;
    }
    if (Maro_IsCancelled(cancelled))
    {
        DeleteFileW(result.installerPath.c_str());
        RemoveDirectoryW(directory.c_str());
        result.installerPath.clear();
        result.status = Maro_UpdateInstallStatus::Cancelled;
        return result;
    }
    result.status = Maro_UpdateInstallStatus::Downloaded;
    return result;
}

Maro_UpdateInstallResult maro_DownloadAndLaunchUpdate(
    const Maro_UpdateRelease& release,
    const std::atomic_bool* cancelled,
    const Maro_UpdateProgress& progress)
{
    Maro_UpdateInstallResult result = maro_DownloadUpdate(release, cancelled, progress);
    if (result.status != Maro_UpdateInstallStatus::Downloaded)
    {
        return result;
    }
    const std::wstring directory = result.installerPath.substr(0, result.installerPath.find_last_of(L'\\'));
    if (Maro_IsCancelled(cancelled))
    {
        DeleteFileW(result.installerPath.c_str());
        RemoveDirectoryW(directory.c_str());
        result.installerPath.clear();
        result.status = Maro_UpdateInstallStatus::Cancelled;
        return result;
    }
    if (!Maro_LaunchInstaller(result.installerPath, directory, result.error))
    {
        DeleteFileW(result.installerPath.c_str());
        RemoveDirectoryW(directory.c_str());
        result.installerPath.clear();
        result.status = Maro_UpdateInstallStatus::Failed;
        return result;
    }
    result.status = Maro_UpdateInstallStatus::Launched;
    return result;
}
