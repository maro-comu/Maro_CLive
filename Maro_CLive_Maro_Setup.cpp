#include "Maro_CLive_Maro_Setup.hpp"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class Maro_Handle final {
public:
    Maro_Handle() noexcept = default;
    explicit Maro_Handle(HANDLE value) noexcept : value_(value) {}

    ~Maro_Handle() noexcept {
        Reset();
    }

    Maro_Handle(const Maro_Handle&) = delete;
    Maro_Handle& operator=(const Maro_Handle&) = delete;

    Maro_Handle(Maro_Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    Maro_Handle& operator=(Maro_Handle&& other) noexcept {
        if (this != &other) {
            Reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept {
        return value_;
    }

    [[nodiscard]] HANDLE* Put() noexcept {
        Reset();
        return &value_;
    }

    void Reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

class Maro_TemporaryVsix final {
public:
    Maro_TemporaryVsix() = default;
    ~Maro_TemporaryVsix() noexcept {
        Cleanup();
    }

    Maro_TemporaryVsix(const Maro_TemporaryVsix&) = delete;
    Maro_TemporaryVsix& operator=(const Maro_TemporaryVsix&) = delete;

    [[nodiscard]] const std::wstring& Path() const noexcept {
        return path_;
    }

    void Set(std::wstring directory, std::wstring path) {
        Cleanup();
        directory_ = std::move(directory);
        path_ = std::move(path);
    }

private:
    void Cleanup() noexcept {
        if (!path_.empty()) {
            DeleteFileW(path_.c_str());
            path_.clear();
        }
        if (!directory_.empty()) {
            RemoveDirectoryW(directory_.c_str());
            directory_.clear();
        }
    }

    std::wstring directory_;
    std::wstring path_;
};

[[nodiscard]] bool Maro_IsFile(const std::wstring& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

[[nodiscard]] std::wstring Maro_GetEnvironmentVariable(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }

    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return {};
    }
    value.resize(written);
    return value;
}

[[nodiscard]] std::wstring Maro_JoinPath(std::wstring left, std::wstring_view right) {
    if (!left.empty() && left.back() != L'\\' && left.back() != L'/') {
        left.push_back(L'\\');
    }
    left.append(right);
    return left;
}

[[nodiscard]] std::wstring Maro_QuoteArgument(std::wstring_view argument) {
    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');

    std::size_t slashCount = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashCount;
            continue;
        }

        if (character == L'"') {
            quoted.append(slashCount * 2 + 1, L'\\');
            quoted.push_back(L'"');
            slashCount = 0;
            continue;
        }

        quoted.append(slashCount, L'\\');
        slashCount = 0;
        quoted.push_back(character);
    }

    quoted.append(slashCount * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] std::wstring Maro_Utf8ToWide(const std::vector<char>& text) {
    if (text.empty()) {
        return {};
    }

    const int sourceSize = static_cast<int>(text.size());
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), sourceSize, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (required == 0) {
        codePage = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(codePage, flags, text.data(), sourceSize, nullptr, 0);
    }
    if (required <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(codePage, flags, text.data(), sourceSize, result.data(), required) == 0) {
        return {};
    }
    return result;
}

void Maro_Trim(std::wstring& text) noexcept {
    constexpr std::wstring_view whitespace = L" \t\r\n";
    const std::size_t begin = text.find_first_not_of(whitespace);
    if (begin == std::wstring::npos) {
        text.clear();
        return;
    }
    const std::size_t end = text.find_last_not_of(whitespace);
    text = text.substr(begin, end - begin + 1);
    if (!text.empty() && text.front() == static_cast<wchar_t>(0xFEFF)) {
        text.erase(text.begin());
    }
}

[[nodiscard]] bool Maro_RunAndCapture(
    const std::wstring& executable,
    std::wstring_view arguments,
    std::vector<char>& output) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    Maro_Handle readPipe;
    Maro_Handle writePipe;
    if (!CreatePipe(readPipe.Put(), writePipe.Put(), &security, 0)) {
        return false;
    }
    if (!SetHandleInformation(readPipe.Get(), HANDLE_FLAG_INHERIT, 0)) {
        return false;
    }

    Maro_Handle nullInput(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (nullInput.Get() == INVALID_HANDLE_VALUE) {
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput.Get();
    startup.hStdOutput = writePipe.Get();
    startup.hStdError = writePipe.Get();

    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = Maro_QuoteArgument(executable);
    if (!arguments.empty()) {
        commandLine.push_back(L' ');
        commandLine.append(arguments);
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    if (!CreateProcessW(
            executable.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            nullptr,
            &startup,
            &processInfo)) {
        return false;
    }

    Maro_Handle process(processInfo.hProcess);
    Maro_Handle thread(processInfo.hThread);
    writePipe.Reset();
    nullInput.Reset();

    output.clear();
    std::array<char, 1024> buffer{};
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(readPipe.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                break;
            }
            return false;
        }
        output.insert(output.end(), buffer.data(), buffer.data() + bytesRead);
    }

    if (WaitForSingleObject(process.Get(), INFINITE) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD exitCode = 1;
    return GetExitCodeProcess(process.Get(), &exitCode) && exitCode == 0;
}

[[nodiscard]] std::wstring Maro_FindInstallerWithVswhere(const std::wstring& vswhere) {
    if (!Maro_IsFile(vswhere)) {
        return {};
    }

    constexpr std::array<std::wstring_view, 2> queries{
        L"-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -utf8",
        L"-latest -products * -property installationPath -utf8",
    };

    for (const std::wstring_view query : queries) {
        std::vector<char> output;
        if (!Maro_RunAndCapture(vswhere, query, output)) {
            continue;
        }
        std::wstring installationPath = Maro_Utf8ToWide(output);
        Maro_Trim(installationPath);
        if (installationPath.empty()) {
            continue;
        }

        const std::size_t lineEnd = installationPath.find_first_of(L"\r\n");
        if (lineEnd != std::wstring::npos) {
            installationPath.resize(lineEnd);
            Maro_Trim(installationPath);
        }
        const std::wstring installer = Maro_JoinPath(installationPath, L"Common7\\IDE\\VSIXInstaller.exe");
        if (Maro_IsFile(installer)) {
            return installer;
        }
    }
    return {};
}

[[nodiscard]] std::wstring Maro_FindVsixInstaller() {
    const std::array<std::wstring, 2> programDirectories{
        Maro_GetEnvironmentVariable(L"ProgramFiles(x86)"),
        Maro_GetEnvironmentVariable(L"ProgramFiles"),
    };

    for (const std::wstring& directory : programDirectories) {
        if (directory.empty()) {
            continue;
        }
        const std::wstring vswhere = Maro_JoinPath(directory, L"Microsoft Visual Studio\\Installer\\vswhere.exe");
        const std::wstring installer = Maro_FindInstallerWithVswhere(vswhere);
        if (!installer.empty()) {
            return installer;
        }
    }

    constexpr std::array<std::wstring_view, 3> versions{L"18", L"2022", L"17"};
    constexpr std::array<std::wstring_view, 4> editions{L"Community", L"Professional", L"Enterprise", L"BuildTools"};
    for (const std::wstring& directory : programDirectories) {
        if (directory.empty()) {
            continue;
        }
        for (const std::wstring_view version : versions) {
            for (const std::wstring_view edition : editions) {
                std::wstring path = Maro_JoinPath(directory, L"Microsoft Visual Studio");
                path = Maro_JoinPath(std::move(path), version);
                path = Maro_JoinPath(std::move(path), edition);
                path = Maro_JoinPath(std::move(path), L"Common7\\IDE\\VSIXInstaller.exe");
                if (Maro_IsFile(path)) {
                    return path;
                }
            }
        }
    }
    return {};
}

[[nodiscard]] bool Maro_ExtractVsix(HINSTANCE instance, Maro_TemporaryVsix& temporaryVsix) {
    const HRSRC resource = FindResourceW(
        instance,
        MAKEINTRESOURCEW(MARO_CLIVE_MARO_VSIX_RESOURCE),
        RT_RCDATA);
    if (resource == nullptr) {
        return false;
    }
    const HGLOBAL loadedResource = LoadResource(instance, resource);
    const void* resourceData = loadedResource == nullptr ? nullptr : LockResource(loadedResource);
    const DWORD resourceSize = SizeofResource(instance, resource);
    if (resourceData == nullptr || resourceSize == 0) {
        return false;
    }

    std::array<wchar_t, MAX_PATH + 1> tempPathBuffer{};
    const DWORD tempPathLength = GetTempPathW(static_cast<DWORD>(tempPathBuffer.size()), tempPathBuffer.data());
    if (tempPathLength == 0 || tempPathLength >= tempPathBuffer.size()) {
        return false;
    }

    GUID uniqueId{};
    if (FAILED(CoCreateGuid(&uniqueId))) {
        return false;
    }
    std::array<wchar_t, 40> uniqueText{};
    if (StringFromGUID2(uniqueId, uniqueText.data(), static_cast<int>(uniqueText.size())) == 0) {
        return false;
    }

    std::wstring token(uniqueText.data());
    if (token.size() >= 2 && token.front() == L'{' && token.back() == L'}') {
        token = token.substr(1, token.size() - 2);
    }
    const std::wstring directory = Maro_JoinPath(tempPathBuffer.data(), L"Maro_CLive_Maro_" + token);
    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        return false;
    }

    const std::wstring path = Maro_JoinPath(directory, L"Maro_CLive_Maro.vsix");
    Maro_Handle file(CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(directory.c_str());
        return false;
    }

    const auto* bytes = static_cast<const std::byte*>(resourceData);
    DWORD remaining = resourceSize;
    while (remaining != 0) {
        DWORD written = 0;
        if (!WriteFile(file.Get(), bytes + (resourceSize - remaining), remaining, &written, nullptr) || written == 0) {
            file.Reset();
            DeleteFileW(path.c_str());
            RemoveDirectoryW(directory.c_str());
            return false;
        }
        remaining -= written;
    }
    file.Reset();
    temporaryVsix.Set(directory, path);
    return true;
}

[[nodiscard]] bool Maro_HasQuietArgument() noexcept {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return false;
    }

    bool quiet = false;
    for (int index = 1; index < argumentCount; ++index) {
        if (_wcsicmp(arguments[index], L"/quiet") == 0 ||
            _wcsicmp(arguments[index], L"-quiet") == 0 ||
            _wcsicmp(arguments[index], L"/q") == 0) {
            quiet = true;
            break;
        }
    }
    LocalFree(arguments);
    return quiet;
}

[[nodiscard]] bool Maro_RunVsixInstaller(
    const std::wstring& installer,
    const std::wstring& vsix,
    bool quiet,
    DWORD& exitCode) {
    std::wstring commandLine = Maro_QuoteArgument(installer);
    if (quiet) {
        commandLine.append(L" /quiet");
    }
    commandLine.push_back(L' ');
    commandLine.append(Maro_QuoteArgument(vsix));

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    const DWORD creationFlags = quiet ? CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT : CREATE_UNICODE_ENVIRONMENT;
    if (!CreateProcessW(
            installer.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            creationFlags,
            nullptr,
            nullptr,
            &startup,
            &processInfo)) {
        return false;
    }

    Maro_Handle process(processInfo.hProcess);
    Maro_Handle thread(processInfo.hThread);
    if (WaitForSingleObject(process.Get(), INFINITE) != WAIT_OBJECT_0) {
        return false;
    }
    return GetExitCodeProcess(process.Get(), &exitCode) != FALSE;
}

void Maro_ShowError(const wchar_t* message) noexcept {
    MessageBoxW(nullptr, message, L"CLive_Maro", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        const std::wstring installer = Maro_FindVsixInstaller();
        if (installer.empty()) {
            Maro_ShowError(L"Visual Studio 설치 관리자를 찾을 수 없습니다.");
            return 2;
        }

        Maro_TemporaryVsix temporaryVsix;
        if (!Maro_ExtractVsix(instance, temporaryVsix)) {
            Maro_ShowError(L"설치 파일을 준비하지 못했습니다.");
            return 3;
        }

        DWORD exitCode = 1;
        if (!Maro_RunVsixInstaller(installer, temporaryVsix.Path(), Maro_HasQuietArgument(), exitCode)) {
            Maro_ShowError(L"Visual Studio 확장 설치를 시작하지 못했습니다.");
            return 4;
        }
        return static_cast<int>(exitCode);
    } catch (...) {
        Maro_ShowError(L"설치 중 오류가 발생했습니다.");
        return 5;
    }
}
