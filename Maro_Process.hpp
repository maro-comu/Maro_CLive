#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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
};

struct Maro_ProcessRequest
{
    std::wstring executable;
    std::vector<std::wstring> arguments;
    std::wstring workingDirectory;
    std::string standardInputUtf8;
    std::map<std::wstring, std::wstring, std::less<>> environmentOverrides;
    Maro_ProcessLimits limits;
    bool createNoWindow = true;
    bool inheritEnvironment = true;
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

std::wstring Maro_QuoteWindowsArgument(std::wstring_view argument);
std::wstring Maro_BuildWindowsCommandLine(
    std::wstring_view executable,
    const std::vector<std::wstring>& arguments);

Maro_ProcessResult Maro_RunProcess(
    const Maro_ProcessRequest& request,
    const Maro_CancelCheck& cancelled = {});
