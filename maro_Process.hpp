#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class maro_TraceSession;

enum class Maro_ProcessTermination
{
    Exited,
    Cancelled,
    WallTimedOut,
    CpuTimedOut,
    MemoryLimit,
    OutputLimit,
    ProcessLimit,
    StartFailed,
    InternalError
};

struct Maro_ProcessLimits
{
    std::uint32_t wallMilliseconds = 3'000;
    std::uint32_t cpuMilliseconds = 2'000;
    std::uint64_t memoryBytes = 256ull << 20;
    std::uint32_t activeProcessLimit = 1;
    std::size_t stdoutBytes = 1u << 20;
    std::size_t stderrBytes = 1u << 20;
    std::uint32_t maro_idleMilliseconds = 0;
};

class maro_ProcessInput
{
public:
    explicit maro_ProcessInput(std::size_t maro_capacity = 64u << 10);
    bool maro_Submit(std::string_view maro_text) noexcept;
    void maro_End() noexcept;
    void maro_Close() noexcept;
    bool maro_IsOpen() const noexcept;
    bool maro_Claim() noexcept;
    bool maro_Read(std::string& maro_text);

private:
    const std::size_t maro_capacity_;
    std::mutex maro_mutex_;
    std::condition_variable maro_changed_;
    std::deque<std::string> maro_pending_;
    std::size_t maro_bytes_ = 0;
    std::atomic<bool> maro_ended_{false};
    std::atomic<bool> maro_closed_{false};
    std::atomic<bool> maro_claimed_{false};
};

struct Maro_ProcessRequest
{
    std::wstring executable;
    std::vector<std::wstring> arguments;
    std::wstring workingDirectory;
    std::string standardInputUtf8;
    std::shared_ptr<maro_ProcessInput> maro_interactiveInput;
    std::shared_ptr<maro_TraceSession> maro_trace;
    std::wstring maro_traceSource;
    std::map<std::wstring, std::wstring, std::less<>> environmentOverrides;
    Maro_ProcessLimits limits;
    bool createNoWindow = true;
    bool inheritEnvironment = true;
    bool maro_rollingOutput = false;
    bool maro_allowGuiWindows = false;
    bool maro_background = false;
};

struct Maro_ProcessResult
{
    Maro_ProcessTermination termination = Maro_ProcessTermination::InternalError;
    std::string standardOutputUtf8;
    std::string standardErrorUtf8;
    std::uint32_t exitCode = 0;
    std::uint32_t win32Error = 0;
    bool hasExitCode = false;
    bool jobObjectApplied = false;
};

using Maro_CancelCheck = std::function<bool()>;
using Maro_ProcessOutputCallback = std::function<void(bool, std::string_view)>;

std::wstring Maro_QuoteWindowsArgument(std::wstring_view argument);
std::wstring maro_BuildWindowsCommandLine(
    std::wstring_view executable,
    const std::vector<std::wstring>& arguments);

Maro_ProcessResult Maro_RunProcess(
    const Maro_ProcessRequest& request,
    const Maro_CancelCheck& cancelled = {},
    const Maro_ProcessOutputCallback& output = {});
