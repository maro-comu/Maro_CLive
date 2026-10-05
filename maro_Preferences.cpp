#include "maro_Preferences.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <array>
#include <atomic>
#include <string>
#include <string_view>
#include <utility>

namespace
{
constexpr wchar_t maro_fileName[] = L"maro_Preferences.cfg";
constexpr std::string_view maro_prefix = "CLive_Maro preferences 1\nautomatic=";
std::atomic<unsigned long long> maro_sequence{0};

struct maro_File
{
    HANDLE maro_handle = INVALID_HANDLE_VALUE;
    ~maro_File() { if (maro_handle != INVALID_HANDLE_VALUE) CloseHandle(maro_handle); }
    bool maro_Close() noexcept
    {
        if (maro_handle == INVALID_HANDLE_VALUE) return false;
        const bool maro_closed = CloseHandle(maro_handle) != FALSE;
        maro_handle = INVALID_HANDLE_VALUE;
        return maro_closed;
    }
};

bool maro_ValidPath(const std::filesystem::path& maro_path)
{
    const auto& maro_native = maro_path.native();
    return !maro_native.empty() && maro_native.size() < 4096 &&
        maro_native.find(L'\0') == std::wstring::npos && maro_path.is_absolute() &&
        maro_path.filename() == maro_fileName;
}

bool maro_RegularDirectory(const std::filesystem::path& maro_path)
{
    const DWORD maro_attributes = GetFileAttributesW(maro_path.c_str());
    return maro_attributes != INVALID_FILE_ATTRIBUTES &&
        (maro_attributes & FILE_ATTRIBUTE_DIRECTORY) && !(maro_attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool maro_RegularFileOrMissing(const std::filesystem::path& maro_path)
{
    const DWORD maro_attributes = GetFileAttributesW(maro_path.c_str());
    if (maro_attributes == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    return !(maro_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

std::string_view maro_Encoding(unsigned maro_page) noexcept
{
    switch (maro_page)
    {
    case 0: return "auto";
    case 65001: return "utf8";
    case 949: return "cp949";
    case 1252: return "cp1252";
    default: return {};
    }
}

bool maro_ParsePreferences(std::string_view maro_text, maro_Preferences& maro_result) noexcept
{
    if (!maro_text.starts_with(maro_prefix)) return false;
    maro_text.remove_prefix(maro_prefix.size());
    if (maro_text.starts_with("1\nencoding=")) maro_result.maro_automatic = true;
    else if (maro_text.starts_with("0\nencoding=")) maro_result.maro_automatic = false;
    else return false;
    maro_text.remove_prefix(11);
    if (maro_text == "auto\n") maro_result.maro_codePage = 0;
    else if (maro_text == "utf8\n") maro_result.maro_codePage = 65001;
    else if (maro_text == "cp949\n") maro_result.maro_codePage = 949;
    else if (maro_text == "cp1252\n") maro_result.maro_codePage = 1252;
    else return false;
    return true;
}
}

std::filesystem::path maro_PreferencesPath() noexcept
{
    try
    {
        PWSTR maro_folder = nullptr;
        const HRESULT maro_result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY,
            nullptr, &maro_folder);
        if (FAILED(maro_result) || !maro_folder)
        {
            CoTaskMemFree(maro_folder);
            return {};
        }
        std::filesystem::path maro_path;
        try { maro_path = std::filesystem::path(maro_folder) / L"CLive_Maro" / maro_fileName; }
        catch (...) { CoTaskMemFree(maro_folder); return {}; }
        CoTaskMemFree(maro_folder);
        return maro_ValidPath(maro_path) ? maro_path : std::filesystem::path{};
    }
    catch (...) { return {}; }
}

maro_Preferences maro_LoadPreferences(const std::filesystem::path& maro_path) noexcept
{
    try
    {
        const auto maro_target = maro_path.empty() ? maro_PreferencesPath() : maro_path;
        if (!maro_ValidPath(maro_target) || !maro_RegularDirectory(maro_target.parent_path())) return {};
        maro_File maro_file;
        maro_file.maro_handle = CreateFileW(maro_target.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (maro_file.maro_handle == INVALID_HANDLE_VALUE) return {};
        BY_HANDLE_FILE_INFORMATION maro_information{};
        LARGE_INTEGER maro_size{};
        if (!GetFileInformationByHandle(maro_file.maro_handle, &maro_information) ||
            (maro_information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(maro_file.maro_handle, &maro_size) || maro_size.QuadPart <= 0 ||
            maro_size.QuadPart >= 128) return {};
        std::array<char, 128> maro_data{};
        DWORD maro_read = 0;
        if (!ReadFile(maro_file.maro_handle, maro_data.data(), static_cast<DWORD>(maro_size.QuadPart),
            &maro_read, nullptr) || maro_read != static_cast<DWORD>(maro_size.QuadPart)) return {};
        maro_Preferences maro_preferences;
        if (!maro_ParsePreferences(std::string_view(maro_data.data(), maro_read), maro_preferences)) return {};
        return maro_preferences;
    }
    catch (...) { return {}; }
}

bool maro_SavePreferences(const maro_Preferences& maro_preferences,
    const std::filesystem::path& maro_path) noexcept
{
    try
    {
        const auto maro_encoding = maro_Encoding(maro_preferences.maro_codePage);
        if (maro_encoding.empty()) return false;
        const auto maro_target = maro_path.empty() ? maro_PreferencesPath() : maro_path;
        if (!maro_ValidPath(maro_target)) return false;
        const auto maro_parent = maro_target.parent_path();
        if (!maro_RegularDirectory(maro_parent))
        {
            if (!maro_path.empty() || (!CreateDirectoryW(maro_parent.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) || !maro_RegularDirectory(maro_parent)) return false;
        }
        if (!maro_RegularFileOrMissing(maro_target)) return false;
        std::string maro_text(maro_prefix);
        maro_text += maro_preferences.maro_automatic ? "1\nencoding=" : "0\nencoding=";
        maro_text += maro_encoding;
        maro_text += '\n';
        const auto maro_temp = maro_parent / (std::wstring(L"maro_Preferences_") +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()) + L"_" +
            std::to_wstring(maro_sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp");
        maro_File maro_file;
        maro_file.maro_handle = CreateFileW(maro_temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (maro_file.maro_handle == INVALID_HANDLE_VALUE) return false;
        DWORD maro_written = 0;
        const bool maro_complete = WriteFile(maro_file.maro_handle, maro_text.data(),
            static_cast<DWORD>(maro_text.size()), &maro_written, nullptr) && maro_written == maro_text.size() &&
            FlushFileBuffers(maro_file.maro_handle);
        const bool maro_closed = maro_file.maro_Close();
        if (maro_complete && maro_closed && maro_RegularDirectory(maro_parent) &&
            maro_RegularFileOrMissing(maro_target) && MoveFileExW(maro_temp.c_str(), maro_target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileW(maro_temp.c_str());
        return false;
    }
    catch (...) { return false; }
}

maro_PreferencesWorker::maro_PreferencesWorker(std::filesystem::path maro_path)
    : maro_path_(std::move(maro_path))
{
    try { maro_thread_ = std::thread([this] { maro_Run(); }); }
    catch (...) { maro_loaded_ = maro_Preferences{}; }
}

maro_PreferencesWorker::~maro_PreferencesWorker()
{
    {
        std::lock_guard maro_lock(maro_mutex_);
        maro_stopped_ = true;
    }
    maro_ready_.notify_one();
    if (maro_thread_.joinable()) maro_thread_.join();
}

bool maro_PreferencesWorker::maro_TakeLoaded(maro_Preferences& maro_preferences) noexcept
{
    try
    {
        std::lock_guard maro_lock(maro_mutex_);
        if (!maro_loaded_) return false;
        maro_preferences = *maro_loaded_;
        maro_loaded_.reset();
        return true;
    }
    catch (...) { return false; }
}

void maro_PreferencesWorker::maro_QueueSave(maro_Preferences maro_preferences) noexcept
{
    try
    {
        if (maro_Encoding(maro_preferences.maro_codePage).empty()) return;
        {
            std::lock_guard maro_lock(maro_mutex_);
            if (maro_stopped_ || !maro_thread_.joinable()) return;
            maro_pending_ = maro_preferences;
        }
        maro_ready_.notify_one();
    }
    catch (...) {}
}

void maro_PreferencesWorker::maro_Run() noexcept
{
    try
    {
        const auto maro_loaded = maro_LoadPreferences(maro_path_);
        {
            std::lock_guard maro_lock(maro_mutex_);
            maro_loaded_ = maro_loaded;
        }
        for (;;)
        {
            maro_Preferences maro_snapshot;
            {
                std::unique_lock maro_lock(maro_mutex_);
                maro_ready_.wait(maro_lock, [this] { return maro_stopped_ || maro_pending_.has_value(); });
                if (!maro_pending_) return;
                maro_snapshot = *maro_pending_;
                maro_pending_.reset();
            }
            maro_SavePreferences(maro_snapshot, maro_path_);
        }
    }
    catch (...) {}
}
