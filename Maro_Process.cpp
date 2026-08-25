#include "Maro_Process.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <memory>
#include <thread>

namespace
{
class Maro_UniqueHandle
{
public:
    Maro_UniqueHandle() = default;
    explicit Maro_UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~Maro_UniqueHandle()
    {
        reset();
    }

    Maro_UniqueHandle(const Maro_UniqueHandle&) = delete;
    Maro_UniqueHandle& operator=(const Maro_UniqueHandle&) = delete;

    Maro_UniqueHandle(Maro_UniqueHandle&& other) noexcept : handle_(other.release()) {}
    Maro_UniqueHandle& operator=(Maro_UniqueHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept
    {
        const HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }
    void reset(HANDLE handle = nullptr) noexcept
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

bool Maro_CreatePipe(Maro_UniqueHandle& readHandle, Maro_UniqueHandle& writeHandle)
{
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE rawRead = nullptr;
    HANDLE rawWrite = nullptr;
    if (!CreatePipe(&rawRead, &rawWrite, &attributes, 0))
    {
        return false;
    }
    readHandle.reset(rawRead);
    writeHandle.reset(rawWrite);
    return true;
}

std::wstring Maro_EnvironmentKey(std::wstring_view entry)
{
    std::size_t equals = entry.find(L'=');
    if (!entry.empty() && entry.front() == L'=')
    {
        equals = entry.find(L'=', 1);
    }
    return std::wstring(entry.substr(0, equals));
}

std::vector<wchar_t> Maro_BuildEnvironmentBlock(
    const std::map<std::wstring, std::wstring, std::less<>>& overrides,
    bool inheritEnvironment)
{
    std::vector<std::wstring> entries;
    if (inheritEnvironment)
    {
        if (LPWCH environment = GetEnvironmentStringsW())
        {
            for (const wchar_t* cursor = environment; *cursor != L'\0';)
            {
                std::wstring value(cursor);
                entries.push_back(value);
                cursor += value.size() + 1;
            }
            FreeEnvironmentStringsW(environment);
        }
    }

    for (const auto& [key, value] : overrides)
    {
        if (key.empty() || key.find(L'=') != std::wstring::npos)
        {
            continue;
        }
        std::erase_if(entries, [&key](const std::wstring& entry) {
            return _wcsicmp(Maro_EnvironmentKey(entry).c_str(), key.c_str()) == 0;
        });
        entries.push_back(key + L"=" + value);
    }

    std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    std::size_t size = 1;
    for (const std::wstring& entry : entries)
    {
        size += entry.size() + 1;
    }
    block.reserve(size);
    for (const std::wstring& entry : entries)
    {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.empty())
    {
        block.push_back(L'\0');
    }
    return block;
}

bool Maro_ConfigureJob(HANDLE job, const Maro_ProcessLimits& limits)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
    information.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;

    if (limits.activeProcessLimit > 0)
    {
        information.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        information.BasicLimitInformation.ActiveProcessLimit = limits.activeProcessLimit;
    }
    if (limits.memoryBytes > 0)
    {
        information.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        information.JobMemoryLimit = static_cast<SIZE_T>(limits.memoryBytes);
    }
    if (limits.cpuMilliseconds > 0)
    {
        information.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
        information.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
            static_cast<LONGLONG>(limits.cpuMilliseconds) * 10'000ll;
    }

    return SetInformationJobObject(
        job,
        JobObjectExtendedLimitInformation,
        &information,
        sizeof(information)) != FALSE;
}

void Maro_ReadPipe(
    HANDLE pipe,
    std::string& destination,
    std::size_t limit,
    std::atomic<bool>& exceeded)
{
    char buffer[4096];
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr) || bytesRead == 0)
        {
            break;
        }

        const std::size_t remaining = destination.size() < limit ? limit - destination.size() : 0;
        const std::size_t toCopy = (std::min)(remaining, static_cast<std::size_t>(bytesRead));
        try
        {
            destination.append(buffer, toCopy);
        }
        catch (...)
        {
            exceeded.store(true, std::memory_order_release);
            break;
        }
        if (toCopy < bytesRead)
        {
            exceeded.store(true, std::memory_order_release);
        }
    }
}

void Maro_WritePipe(HANDLE pipe, const std::string& input)
{
    std::size_t offset = 0;
    while (offset < input.size())
    {
        const std::size_t remaining = input.size() - offset;
        const DWORD requested = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>(64 * 1024)));
        DWORD written = 0;
        if (!WriteFile(pipe, input.data() + offset, requested, &written, nullptr) || written == 0)
        {
            break;
        }
        offset += written;
    }
    CloseHandle(pipe);
}
} // namespace

std::wstring Maro_QuoteWindowsArgument(std::wstring_view argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
    {
        return std::wstring(argument);
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring Maro_BuildWindowsCommandLine(
    std::wstring_view executable,
    const std::vector<std::wstring>& arguments)
{
    std::wstring commandLine = Maro_QuoteWindowsArgument(executable);
    for (const std::wstring& argument : arguments)
    {
        commandLine.push_back(L' ');
        commandLine += Maro_QuoteWindowsArgument(argument);
    }
    return commandLine;
}

Maro_ProcessResult Maro_RunProcess(
    const Maro_ProcessRequest& request,
    const Maro_CancelCheck& cancelled)
{
    Maro_ProcessResult result;
    result.termination = Maro_ProcessTermination::StartFailed;

    if (request.executable.empty())
    {
        result.win32Error = ERROR_INVALID_PARAMETER;
        return result;
    }

    Maro_UniqueHandle stdoutRead;
    Maro_UniqueHandle stdoutWrite;
    Maro_UniqueHandle stderrRead;
    Maro_UniqueHandle stderrWrite;
    Maro_UniqueHandle stdinRead;
    Maro_UniqueHandle stdinWrite;
    if (!Maro_CreatePipe(stdoutRead, stdoutWrite) ||
        !Maro_CreatePipe(stderrRead, stderrWrite) ||
        !Maro_CreatePipe(stdinRead, stdinWrite))
    {
        result.win32Error = GetLastError();
        return result;
    }

    if (!SetHandleInformation(stdoutRead.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderrRead.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stdinWrite.get(), HANDLE_FLAG_INHERIT, 0))
    {
        result.win32Error = GetLastError();
        return result;
    }

    Maro_UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job || !Maro_ConfigureJob(job.get(), request.limits))
    {
        result.win32Error = GetLastError();
        return result;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = stdinRead.get();
    startup.StartupInfo.hStdOutput = stdoutWrite.get();
    startup.StartupInfo.hStdError = stderrWrite.get();

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::unique_ptr<std::byte[]> attributeStorage(new (std::nothrow) std::byte[attributeBytes]);
    if (!attributeStorage)
    {
        result.win32Error = ERROR_OUTOFMEMORY;
        return result;
    }
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.get());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes))
    {
        result.win32Error = GetLastError();
        return result;
    }
    struct Maro_AttributeListGuard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Maro_AttributeListGuard() { DeleteProcThreadAttributeList(value); }
    } attributeGuard{startup.lpAttributeList};

    HANDLE inheritedHandles[] = {stdinRead.get(), stdoutWrite.get(), stderrWrite.get()};
    if (!UpdateProcThreadAttribute(
            startup.lpAttributeList,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles,
            sizeof(inheritedHandles),
            nullptr,
            nullptr))
    {
        result.win32Error = GetLastError();
        return result;
    }

    std::wstring commandLine = Maro_BuildWindowsCommandLine(request.executable, request.arguments);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    std::vector<wchar_t> environment;
    void* environmentPointer = nullptr;
    if (!request.inheritEnvironment || !request.environmentOverrides.empty())
    {
        environment = Maro_BuildEnvironmentBlock(
            request.environmentOverrides, request.inheritEnvironment);
        environmentPointer = environment.data();
    }

    DWORD creationFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    if (request.createNoWindow)
    {
        creationFlags |= CREATE_NO_WINDOW;
    }

    PROCESS_INFORMATION processInformation{};
    const BOOL created = CreateProcessW(
        request.executable.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        creationFlags,
        environmentPointer,
        request.workingDirectory.empty() ? nullptr : request.workingDirectory.c_str(),
        &startup.StartupInfo,
        &processInformation);
    if (!created)
    {
        result.win32Error = GetLastError();
        return result;
    }

    Maro_UniqueHandle process(processInformation.hProcess);
    Maro_UniqueHandle thread(processInformation.hThread);
    stdinRead.reset();
    stdoutWrite.reset();
    stderrWrite.reset();

    if (!AssignProcessToJobObject(job.get(), process.get()))
    {
        result.win32Error = GetLastError();
        TerminateProcess(process.get(), ERROR_ACCESS_DENIED);
        return result;
    }
    result.jobObjectApplied = true;

    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
    {
        result.win32Error = GetLastError();
        TerminateJobObject(job.get(), result.win32Error);
        return result;
    }
    thread.reset();

    std::atomic<bool> outputExceeded{false};
    std::thread stdoutReader;
    std::thread stderrReader;
    std::thread stdinWriter;
    HANDLE rawStdinWrite = nullptr;
    try
    {
        stdoutReader = std::thread(
            Maro_ReadPipe,
            stdoutRead.get(),
            std::ref(result.standardOutputUtf8),
            request.limits.stdoutBytes,
            std::ref(outputExceeded));
        stderrReader = std::thread(
            Maro_ReadPipe,
            stderrRead.get(),
            std::ref(result.standardErrorUtf8),
            request.limits.stderrBytes,
            std::ref(outputExceeded));
        rawStdinWrite = stdinWrite.release();
        stdinWriter = std::thread(
            Maro_WritePipe,
            rawStdinWrite,
            std::cref(request.standardInputUtf8));
        rawStdinWrite = nullptr; // The writer thread owns and closes the handle.
    }
    catch (...)
    {
        if (rawStdinWrite != nullptr)
        {
            CloseHandle(rawStdinWrite);
        }
        result.win32Error = ERROR_NOT_ENOUGH_MEMORY;
        result.termination = Maro_ProcessTermination::InternalError;
        TerminateJobObject(job.get(), result.win32Error);
        process.reset();
        job.reset();
        if (stdinWriter.joinable())
        {
            stdinWriter.join();
        }
        if (stdoutReader.joinable())
        {
            stdoutReader.join();
        }
        if (stderrReader.joinable())
        {
            stderrReader.join();
        }
        return result;
    }

    result.termination = Maro_ProcessTermination::Exited;
    const ULONGLONG startedAt = GetTickCount64();
    for (;;)
    {
        const DWORD waitResult = WaitForSingleObject(process.get(), 20);
        if (waitResult == WAIT_OBJECT_0)
        {
            break;
        }
        if (waitResult == WAIT_FAILED)
        {
            result.win32Error = GetLastError();
            result.termination = Maro_ProcessTermination::InternalError;
            TerminateJobObject(job.get(), result.win32Error);
            break;
        }

        bool shouldCancel = false;
        if (cancelled)
        {
            try
            {
                shouldCancel = cancelled();
            }
            catch (...)
            {
                shouldCancel = true;
            }
        }
        if (shouldCancel)
        {
            result.termination = Maro_ProcessTermination::Cancelled;
            TerminateJobObject(job.get(), ERROR_CANCELLED);
            break;
        }
        if (outputExceeded.load(std::memory_order_acquire))
        {
            result.termination = Maro_ProcessTermination::OutputLimit;
            TerminateJobObject(job.get(), ERROR_BUFFER_OVERFLOW);
            break;
        }
        if (request.limits.wallMilliseconds > 0 &&
            GetTickCount64() - startedAt >= request.limits.wallMilliseconds)
        {
            result.termination = Maro_ProcessTermination::WallTimedOut;
            TerminateJobObject(job.get(), WAIT_TIMEOUT);
            break;
        }
    }

    WaitForSingleObject(process.get(), 2'000);
    DWORD exitCode = 0;
    if (GetExitCodeProcess(process.get(), &exitCode) && exitCode != STILL_ACTIVE)
    {
        result.exitCode = exitCode;
        result.hasExitCode = true;
    }
    if (result.termination == Maro_ProcessTermination::Exited &&
        result.hasExitCode && result.exitCode != 0 &&
        request.limits.activeProcessLimit == 1 && request.limits.cpuMilliseconds > 0)
    {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (QueryInformationJobObject(
                job.get(),
                JobObjectBasicAccountingInformation,
                &accounting,
                sizeof(accounting),
                nullptr))
        {
            const LONGLONG cpuLimit = static_cast<LONGLONG>(request.limits.cpuMilliseconds) * 10'000ll;
            if (accounting.TotalUserTime.QuadPart >= cpuLimit)
            {
                result.termination = Maro_ProcessTermination::CpuTimedOut;
            }
        }
    }

    // Closing the remaining child-side handles makes all blocked pipe operations finish.
    process.reset();
    job.reset();
    if (stdinWriter.joinable())
    {
        stdinWriter.join();
    }
    if (stdoutReader.joinable())
    {
        stdoutReader.join();
    }
    if (stderrReader.joinable())
    {
        stderrReader.join();
    }
    stdoutRead.reset();
    stderrRead.reset();
    return result;
}
