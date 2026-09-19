#include "maro_Process.hpp"
#include "maro_Trace.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <memory>
#include <mutex>
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

std::vector<wchar_t> maro_BuildEnvironmentBlock(
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

bool Maro_ConfigureJob(HANDLE job, const Maro_ProcessLimits& limits, bool maro_background)
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

    if (!SetInformationJobObject(
        job,
        JobObjectExtendedLimitInformation,
        &information,
        sizeof(information))) return false;
    if (maro_background)
    {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION maro_cpu{};
        maro_cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        maro_cpu.CpuRate = 2000;
        if (!SetInformationJobObject(job, JobObjectCpuRateControlInformation, &maro_cpu, sizeof(maro_cpu)))
            return false;
    }
    return true;
}

void Maro_PublishProcessOutput(
    bool standardError,
    std::string_view bytes,
    const Maro_ProcessOutputCallback& output,
    std::mutex& outputMutex) noexcept
{
    if (bytes.empty() || !output)
    {
        return;
    }
    try
    {
        std::lock_guard lock(outputMutex);
        output(standardError, bytes);
    }
    catch (...)
    {
    }
}

void Maro_ReadPipe(
    HANDLE pipe,
    std::string& destination,
    std::size_t limit,
    bool maro_rolling,
    std::atomic<bool>& exceeded,
    bool standardError,
    Maro_ProcessOutputCallback output,
    std::mutex& outputMutex,
    std::atomic<ULONGLONG>& maro_lastOutput,
    bool maro_background)
{
    if (maro_background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    char buffer[4096];
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr) || bytesRead == 0)
        {
            break;
        }
        maro_lastOutput.store(GetTickCount64(), std::memory_order_relaxed);

        const std::size_t remaining = destination.size() < limit ? limit - destination.size() : 0;
        const std::size_t toCopy = maro_rolling
            ? static_cast<std::size_t>(bytesRead)
            : (std::min)(remaining, static_cast<std::size_t>(bytesRead));
        try
        {
            destination.append(buffer, toCopy);
            if (maro_rolling && destination.size() > limit &&
                destination.size() - limit > (std::min)(limit, std::size_t{64u << 10}))
            {
                destination.erase(0, destination.size() - limit);
            }
        }
        catch (...)
        {
            exceeded.store(true, std::memory_order_release);
            break;
        }
        Maro_PublishProcessOutput(standardError, std::string_view(buffer, toCopy), output, outputMutex);
        if (toCopy < bytesRead)
        {
            exceeded.store(true, std::memory_order_release);
        }
    }
    if (maro_rolling && destination.size() > limit)
        destination.erase(0, destination.size() - limit);
}

bool maro_WriteInput(HANDLE pipe, std::string_view input)
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
            return false;
        }
        offset += written;
    }
    return true;
}

bool maro_ThreadMayWaitForIo(HANDLE maro_thread)
{
    BOOL maro_pending = FALSE;
    return !maro_thread || !GetThreadIOPendingFlag(maro_thread, &maro_pending) || maro_pending;
}

bool maro_ProcessMayWaitForIo(DWORD maro_processId, DWORD maro_primaryId)
{
    Maro_UniqueHandle maro_snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!maro_snapshot) return true;
    THREADENTRY32 maro_entry{};
    maro_entry.dwSize = sizeof(maro_entry);
    if (!Thread32First(maro_snapshot.get(), &maro_entry)) return true;
    unsigned maro_entries = 0, maro_owned = 0;
    do
    {
        if (++maro_entries > 32768) return true;
        if (maro_entry.th32OwnerProcessID != maro_processId || maro_entry.th32ThreadID == maro_primaryId) continue;
        if (++maro_owned > 1024) return true;
        Maro_UniqueHandle maro_thread(OpenThread(THREAD_QUERY_INFORMATION, FALSE, maro_entry.th32ThreadID));
        if (maro_ThreadMayWaitForIo(maro_thread.get())) return true;
    } while (Thread32Next(maro_snapshot.get(), &maro_entry));
    return GetLastError() != ERROR_NO_MORE_FILES;
}

void Maro_WritePipe(
    HANDLE pipe,
    const std::string& input,
    std::shared_ptr<maro_ProcessInput> maro_interactive)
{
    Maro_UniqueHandle maro_pipe(pipe);
    if (!maro_WriteInput(pipe, input) || !maro_interactive)
    {
        return;
    }
    std::string maro_chunk;
    while (maro_interactive->maro_Read(maro_chunk))
    {
        if (!maro_WriteInput(pipe, maro_chunk))
        {
            maro_interactive->maro_Close();
            break;
        }
    }
}
}

maro_ProcessInput::maro_ProcessInput(std::size_t maro_capacity)
    : maro_capacity_(maro_capacity)
{
}

bool maro_ProcessInput::maro_Submit(std::string_view maro_text) noexcept
{
    try
    {
        std::unique_lock maro_lock(maro_mutex_, std::try_to_lock);
        if (!maro_lock.owns_lock() || !maro_IsOpen() ||
            maro_text.size() > maro_capacity_ - maro_bytes_)
        {
            return false;
        }
        if (!maro_text.empty())
        {
            maro_pending_.emplace_back(maro_text);
            maro_bytes_ += maro_text.size();
            maro_changed_.notify_one();
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void maro_ProcessInput::maro_End() noexcept
{
    maro_ended_.store(true, std::memory_order_release);
    maro_changed_.notify_all();
}

void maro_ProcessInput::maro_Close() noexcept
{
    maro_closed_.store(true, std::memory_order_release);
    maro_changed_.notify_all();
}

bool maro_ProcessInput::maro_IsOpen() const noexcept
{
    return !maro_ended_.load(std::memory_order_acquire) &&
        !maro_closed_.load(std::memory_order_acquire);
}

bool maro_ProcessInput::maro_Claim() noexcept
{
    return !maro_closed_.load(std::memory_order_acquire) &&
        !maro_claimed_.exchange(true, std::memory_order_acq_rel);
}

bool maro_ProcessInput::maro_Read(std::string& maro_text)
{
    std::unique_lock maro_lock(maro_mutex_);
    for (;;)
    {
        if (maro_closed_.load(std::memory_order_acquire))
        {
            return false;
        }
        if (!maro_pending_.empty())
        {
            maro_text = std::move(maro_pending_.front());
            maro_pending_.pop_front();
            maro_bytes_ -= maro_text.size();
            return true;
        }
        if (maro_ended_.load(std::memory_order_acquire))
        {
            return false;
        }
        maro_changed_.wait_for(maro_lock, std::chrono::milliseconds(20));
    }
}

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

std::wstring maro_BuildWindowsCommandLine(
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
    const Maro_CancelCheck& cancelled,
    const Maro_ProcessOutputCallback& output)
{
    Maro_ProcessResult result;
    result.termination = Maro_ProcessTermination::StartFailed;

    if (request.maro_interactiveInput && !request.maro_interactiveInput->maro_Claim())
    {
        result.win32Error = ERROR_INVALID_PARAMETER;
        return result;
    }
    struct maro_InputGuard
    {
        std::shared_ptr<maro_ProcessInput> maro_input;
        ~maro_InputGuard()
        {
            if (maro_input)
            {
                maro_input->maro_Close();
            }
        }
    } maro_inputGuard{request.maro_interactiveInput};

    if (request.executable.empty() || (request.maro_trace && request.maro_traceSource.empty()))
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
    auto maro_jobLimits = request.limits;
    if (request.maro_trace) maro_jobLimits.cpuMilliseconds = 0;
    const bool maro_background = request.maro_background && !request.maro_trace;
    if (!job || !Maro_ConfigureJob(job.get(), maro_jobLimits, maro_background))
    {
        result.win32Error = GetLastError();
        return result;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | (request.maro_allowGuiWindows ? 0 : STARTF_USESHOWWINDOW);
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

    std::wstring commandLine = maro_BuildWindowsCommandLine(request.executable, request.arguments);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    std::vector<wchar_t> environment;
    void* environmentPointer = nullptr;
    if (!request.inheritEnvironment || !request.environmentOverrides.empty())
    {
        environment = maro_BuildEnvironmentBlock(
            request.environmentOverrides, request.inheritEnvironment);
        environmentPointer = environment.data();
    }

    DWORD creationFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    if (request.createNoWindow)
    {
        creationFlags |= CREATE_NO_WINDOW;
    }
    if (request.maro_trace) creationFlags |= DEBUG_ONLY_THIS_PROCESS;
    if (maro_background) creationFlags |= BELOW_NORMAL_PRIORITY_CLASS;

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
    struct maro_DebugGuard
    {
        DWORD maro_pid;
        bool maro_attached;
        ~maro_DebugGuard()
        {
            if (maro_attached) DebugActiveProcessStop(maro_pid);
        }
    } maro_debugGuard{processInformation.dwProcessId, request.maro_trace != nullptr};
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
    if (request.maro_trace || !request.maro_interactiveInput || !request.limits.maro_idleMilliseconds)
        thread.reset();

    std::atomic<bool> outputExceeded{false};
    std::atomic<ULONGLONG> maro_lastOutput{GetTickCount64()};
    std::mutex outputMutex;
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
            request.maro_rollingOutput,
            std::ref(outputExceeded),
            false,
            output,
            std::ref(outputMutex), std::ref(maro_lastOutput), maro_background);
        stderrReader = std::thread(
            Maro_ReadPipe,
            stderrRead.get(),
            std::ref(result.standardErrorUtf8),
            request.limits.stderrBytes,
            request.maro_rollingOutput,
            std::ref(outputExceeded),
            true,
            output,
            std::ref(outputMutex), std::ref(maro_lastOutput), maro_background);
        rawStdinWrite = stdinWrite.release();
        stdinWriter = std::thread(
            Maro_WritePipe,
            rawStdinWrite,
            std::cref(request.standardInputUtf8),
            request.maro_interactiveInput);
        rawStdinWrite = nullptr;
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
        if (request.maro_interactiveInput)
        {
            request.maro_interactiveInput->maro_Close();
        }
        if (maro_debugGuard.maro_attached)
        {
            DebugActiveProcessStop(maro_debugGuard.maro_pid);
            maro_debugGuard.maro_attached = false;
        }
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
    ULONGLONG maro_idleSince = startedAt, maro_sampleAt = startedAt;
    LONGLONG maro_previousCpu = 0;
    if (request.maro_trace)
    {
        const auto maro_traceResult = maro_RunDebugLoop(process.get(), processInformation.dwProcessId,
            request.executable, request.maro_traceSource, request.maro_trace, [&] {
                bool maro_cancel = false;
                try { maro_cancel = cancelled && cancelled(); }
                catch (...) { maro_cancel = true; }
                return maro_cancel || outputExceeded.load(std::memory_order_acquire);
            });
        maro_debugGuard.maro_attached = false;
        if (maro_traceResult.maro_cancelled)
            result.termination = outputExceeded.load(std::memory_order_acquire)
                ? Maro_ProcessTermination::OutputLimit : Maro_ProcessTermination::Cancelled;
        else if (maro_traceResult.maro_error)
        {
            result.termination = Maro_ProcessTermination::InternalError;
            result.win32Error = maro_traceResult.maro_error;
        }
        if (maro_traceResult.maro_exited)
        {
            result.hasExitCode = true;
            result.exitCode = maro_traceResult.maro_exitCode;
        }
    }
    else for (;;)
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
        const ULONGLONG maro_now = GetTickCount64();
        if (request.limits.maro_idleMilliseconds && maro_now - maro_sampleAt >= 250)
        {
            const auto maro_outputAt = maro_lastOutput.load(std::memory_order_relaxed);
            maro_idleSince = (std::max)(maro_idleSince, (std::min)(maro_now, maro_outputAt));
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION maro_accounting{};
            const bool maro_measured = QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation,
                &maro_accounting, sizeof(maro_accounting), nullptr) != FALSE;
            const LONGLONG maro_cpu = maro_accounting.TotalUserTime.QuadPart + maro_accounting.TotalKernelTime.QuadPart;
            const bool maro_waiting = request.maro_interactiveInput && request.maro_interactiveInput->maro_IsOpen() &&
                (!maro_measured || maro_cpu - maro_previousCpu < static_cast<LONGLONG>(maro_now - maro_sampleAt) * 100 ||
                    maro_ThreadMayWaitForIo(thread.get()));
            const bool maro_graphics = request.maro_allowGuiWindows && GetGuiResources(process.get(), GR_GDIOBJECTS) != 0;
            if (maro_waiting || maro_graphics) maro_idleSince = maro_now;
            maro_previousCpu = maro_cpu;
            maro_sampleAt = maro_now;
            if (maro_now - maro_idleSince >= request.limits.maro_idleMilliseconds)
            {
                if (request.maro_interactiveInput && request.maro_interactiveInput->maro_IsOpen() &&
                    maro_ProcessMayWaitForIo(processInformation.dwProcessId, processInformation.dwThreadId))
                {
                    maro_idleSince = maro_now;
                    continue;
                }
                result.termination = Maro_ProcessTermination::WallTimedOut;
                TerminateJobObject(job.get(), WAIT_TIMEOUT);
                break;
            }
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
        request.limits.activeProcessLimit == 1 && request.limits.cpuMilliseconds > 0 && !request.maro_trace)
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

    process.reset();
    job.reset();
    if (request.maro_interactiveInput)
    {
        request.maro_interactiveInput->maro_Close();
    }
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
    if (result.termination == Maro_ProcessTermination::Exited &&
        outputExceeded.load(std::memory_order_acquire))
    {
        result.termination = Maro_ProcessTermination::OutputLimit;
    }
    return result;
}
