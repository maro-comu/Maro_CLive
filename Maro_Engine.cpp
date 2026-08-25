#include "Maro_Engine.hpp"

#include "Maro_Analyzer.hpp"
#include "Maro_Process.hpp"
#include "Maro_Text.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <iterator>
#include <utility>

namespace
{
namespace fs = std::filesystem;

class Maro_TemporaryDirectory
{
public:
    explicit Maro_TemporaryDirectory(std::uint64_t requestId)
    {
        std::wstring buffer(32'768, L'\0');
        const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0 || length >= buffer.size())
        {
            return;
        }
        buffer.resize(length);
        root_ = fs::path(buffer);
        path_ = root_ /
            (L"Maro_CLive_Maro_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
             std::to_wstring(requestId) + L"_" + std::to_wstring(GetTickCount64()));
        std::error_code error;
        if (!fs::create_directory(path_, error) || error)
        {
            path_.clear();
        }
    }

    ~Maro_TemporaryDirectory()
    {
        if (path_.empty() || root_.empty())
        {
            return;
        }
        const std::wstring fileName = path_.filename().wstring();
        std::error_code error;
        const fs::path parent = fs::weakly_canonical(path_.parent_path(), error);
        if (error)
        {
            return;
        }
        error.clear();
        const fs::path expectedRoot = fs::weakly_canonical(root_, error);
        if (!error && parent == expectedRoot && fileName.starts_with(L"Maro_CLive_Maro_"))
        {
            fs::remove_all(path_, error);
        }
    }

    const fs::path& path() const noexcept { return path_; }
    explicit operator bool() const noexcept { return !path_.empty(); }

private:
    fs::path root_;
    fs::path path_;
};

Maro_ResultEnvelope Maro_BaseEnvelope(
    std::uint64_t requestId,
    const Maro_SourceRequest& request,
    Maro_Phase phase,
    Maro_Status status,
    std::wstring statusText)
{
    Maro_ResultEnvelope result;
    result.requestId = requestId;
    result.sourceVersion = request.sourceVersion;
    result.sourceHash = Maro_HashSource(request.sourceText);
    result.phase = phase;
    result.status = status;
    result.statusText = std::move(statusText);
    return result;
}

Maro_Diagnostic Maro_MakeIdeFinding(
    const Maro_SourceRequest& request,
    std::wstring code,
    Maro_Severity severity,
    Maro_Evidence evidence,
    std::wstring message)
{
    Maro_Diagnostic diagnostic;
    diagnostic.sourceVersion = request.sourceVersion;
    diagnostic.findingId = L"Maro_IDE_" + std::to_wstring(request.sourceVersion) + L"_" + code;
    diagnostic.code = std::move(code);
    diagnostic.analyzer = L"CLive_Maro";
    diagnostic.analyzerVersion = L"0.1";
    diagnostic.severity = severity;
    diagnostic.evidence = evidence;
    diagnostic.friendlyMessage = std::move(message);
    return diagnostic;
}
} // namespace

Maro_Engine::Maro_Engine(Maro_ResultCallback callback)
    : callback_(std::move(callback)),
      worker_([this](std::stop_token stopToken) { WorkerLoop(stopToken); })
{
}

Maro_Engine::~Maro_Engine()
{
    Shutdown();
}

std::uint64_t Maro_Engine::Submit(Maro_SourceRequest request)
{
    std::uint64_t requestId = 0;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_.load(std::memory_order_acquire))
        {
            return 0;
        }
        requestId = nextRequestId_.fetch_add(1, std::memory_order_acq_rel) + 1;
        const std::uint64_t generation = cancellationGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
        currentRequestId_.store(requestId, std::memory_order_release);
        currentSourceVersion_.store(request.sourceVersion, std::memory_order_release);
        pending_ = Maro_PendingWork{requestId, generation, std::move(request)};
    }
    condition_.notify_all();
    return requestId;
}

void Maro_Engine::Cancel()
{
    {
        std::lock_guard lock(mutex_);
        cancellationGeneration_.fetch_add(1, std::memory_order_acq_rel);
        pending_.reset();
    }
    condition_.notify_all();
}

void Maro_Engine::Shutdown()
{
    bool expected = false;
    if (!shuttingDown_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        cancellationGeneration_.fetch_add(1, std::memory_order_acq_rel);
        pending_.reset();
    }
    worker_.request_stop();
    condition_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
    {
        worker_.join();
    }
}

std::uint64_t Maro_Engine::CurrentRequestId() const noexcept
{
    return currentRequestId_.load(std::memory_order_acquire);
}

bool Maro_Engine::IsCurrent(std::uint64_t requestId, std::uint64_t sourceVersion) const noexcept
{
    return requestId == currentRequestId_.load(std::memory_order_acquire) &&
        sourceVersion == currentSourceVersion_.load(std::memory_order_acquire);
}

void Maro_Engine::SetLimits(Maro_ExecutionLimits limits)
{
    std::lock_guard lock(mutex_);
    limits_ = limits;
}

void Maro_Engine::WorkerLoop(std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        std::optional<Maro_PendingWork> work;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, stopToken, [this] {
                return pending_.has_value() || shuttingDown_.load(std::memory_order_acquire);
            });
            if (stopToken.stop_requested() || shuttingDown_.load(std::memory_order_acquire))
            {
                break;
            }
            work = std::move(pending_);
            pending_.reset();
        }
        if (work)
        {
            try
            {
                ProcessOne(*work, stopToken);
            }
            catch (...)
            {
                Maro_ResultEnvelope result = Maro_BaseEnvelope(
                    work->requestId,
                    work->request,
                    Maro_Phase::Completed,
                    Maro_Status::InternalError,
                    L"내부 처리 중 예기치 않은 오류가 발생했습니다.");
                Publish(*work, std::move(result));
            }
        }
    }
}

void Maro_Engine::Publish(const Maro_PendingWork& work, Maro_ResultEnvelope result)
{
    if (!IsCurrent(work.requestId, work.request.sourceVersion) || !callback_)
    {
        return;
    }
    result.requestId = work.requestId;
    result.sourceVersion = work.request.sourceVersion;
    result.sourceHash = Maro_HashSource(work.request.sourceText);
    try
    {
        callback_(std::move(result));
    }
    catch (...)
    {
        // A UI callback must not terminate the engine worker.
    }
}

void Maro_Engine::ProcessOne(const Maro_PendingWork& work, std::stop_token stopToken)
{
    Maro_ExecutionLimits limits;
    {
        std::lock_guard lock(mutex_);
        limits = limits_;
    }
    const auto cancelled = [this, &work, stopToken] {
        return stopToken.stop_requested() || shuttingDown_.load(std::memory_order_acquire) ||
            cancellationGeneration_.load(std::memory_order_acquire) != work.cancellationGeneration ||
            !IsCurrent(work.requestId, work.request.sourceVersion);
    };

    auto finishCancelled = [this, &work] {
        Maro_ResultEnvelope result = Maro_BaseEnvelope(
            work.requestId, work.request, Maro_Phase::Completed, Maro_Status::Cancelled, L"요청이 취소되었습니다.");
        Publish(work, std::move(result));
    };

    Publish(work, Maro_BaseEnvelope(
        work.requestId, work.request, Maro_Phase::Generating, Maro_Status::Pending,
        work.request.mode == Maro_SourceMode::Snippet ? L"학습용 코드를 생성하는 중…" : L"소스를 준비하는 중…"));
    const Maro_GeneratedSource generated = Maro_BuildGeneratedSource(work.request);
    if (cancelled())
    {
        finishCancelled();
        return;
    }

    static const Maro_ToolchainInfo toolchain = Maro_DetectToolchain();
    if (toolchain.kind == Maro_ToolchainKind::None)
    {
        Maro_ResultEnvelope result = Maro_BaseEnvelope(
            work.requestId, work.request, Maro_Phase::Completed, Maro_Status::ToolchainMissing,
            L"Clang 또는 MSVC C/C++ 컴파일러를 찾지 못했습니다.");
        result.generatedSource = generated.text;
        result.snippetWrapped = generated.wrapped;
        result.diagnostics.push_back(Maro_MakeIdeFinding(
            work.request, L"SAFE-1001", Maro_Severity::Error, Maro_Evidence::Unknown,
            L"LLVM Clang 또는 Visual Studio C++ 도구를 설치한 뒤 다시 실행해 주세요."));
        Publish(work, std::move(result));
        return;
    }

    Maro_TemporaryDirectory temporary(work.requestId);
    if (!temporary)
    {
        Maro_ResultEnvelope result = Maro_BaseEnvelope(
            work.requestId, work.request, Maro_Phase::Completed, Maro_Status::SandboxUnavailable,
            L"요청별 임시 작업 폴더를 만들지 못했습니다.");
        Publish(work, std::move(result));
        return;
    }

    Maro_ResultEnvelope analyzing = Maro_BaseEnvelope(
        work.requestId, work.request, Maro_Phase::Analyzing, Maro_Status::Pending, L"컴파일러 진단을 확인하는 중…");
    analyzing.generatedSource = generated.text;
    analyzing.snippetWrapped = generated.wrapped;
    analyzing.compilerName = toolchain.name;
    analyzing.compilerVersion = toolchain.version;
    analyzing.usedFallbackCompiler = toolchain.fallback;
    Publish(work, std::move(analyzing));

    Maro_AnalysisResult analysis = Maro_AnalyzeSource(
        toolchain, work.request, generated, temporary.path(), limits, cancelled);
    if (analysis.cancelled || cancelled())
    {
        finishCancelled();
        return;
    }
    if (!analysis.succeeded)
    {
        const Maro_Status failureStatus = analysis.processStartFailed
            ? Maro_Status::SandboxUnavailable
            : (analysis.timedOut
                ? Maro_Status::TimedOut
                : (analysis.limitExceeded ? Maro_Status::LimitExceeded : Maro_Status::CompileFailed));
        const std::wstring failureText = analysis.processStartFailed
            ? L"제한된 컴파일러 프로세스를 시작하지 못했습니다."
            : (analysis.timedOut
                ? L"정적 분석 시간이 제한을 초과했습니다."
                : (analysis.limitExceeded
                    ? L"정적 분석 자원 제한을 초과했습니다."
                    : L"컴파일러 진단을 먼저 해결해 주세요."));
        Maro_ResultEnvelope result = Maro_BaseEnvelope(
            work.requestId,
            work.request,
            Maro_Phase::Completed,
            failureStatus,
            failureText);
        result.generatedSource = generated.text;
        result.snippetWrapped = generated.wrapped;
        result.compilerOutput = analysis.compilerOutput;
        result.diagnostics = std::move(analysis.diagnostics);
        result.compilerName = toolchain.name;
        result.compilerVersion = toolchain.version;
        result.usedFallbackCompiler = toolchain.fallback;
        result.resourceLimitsApplied = analysis.resourceLimitsApplied;
        Publish(work, std::move(result));
        return;
    }

    if (!work.request.execute)
    {
        Maro_ResultEnvelope result = Maro_BaseEnvelope(
            work.requestId, work.request, Maro_Phase::Completed, Maro_Status::Success, L"정적 분석을 완료했습니다.");
        result.generatedSource = generated.text;
        result.snippetWrapped = generated.wrapped;
        result.compilerOutput = analysis.compilerOutput;
        result.diagnostics = std::move(analysis.diagnostics);
        result.compilerName = toolchain.name;
        result.compilerVersion = toolchain.version;
        result.usedFallbackCompiler = toolchain.fallback;
        result.resourceLimitsApplied = analysis.resourceLimitsApplied;
        Publish(work, std::move(result));
        return;
    }

    if (work.request.mode == Maro_SourceMode::Program && !Maro_HasMain(work.request.sourceText))
    {
        Maro_ResultEnvelope result = Maro_BaseEnvelope(
            work.requestId, work.request, Maro_Phase::Completed, Maro_Status::Success,
            L"main()이 없어 프로그램을 실행하지 않았습니다.");
        result.generatedSource = generated.text;
        result.compilerOutput = analysis.compilerOutput;
        result.diagnostics = std::move(analysis.diagnostics);
        result.diagnostics.push_back(Maro_MakeIdeFinding(
            work.request,
            work.request.language == Maro_Language::C17 ? L"C-NAME-1002" : L"CPP-NAME-1002",
            Maro_Severity::Info,
            Maro_Evidence::StaticAnalysis,
            L"Program 모드에서 실행하려면 main() 함수를 정의하세요."));
        result.compilerName = toolchain.name;
        result.compilerVersion = toolchain.version;
        result.usedFallbackCompiler = toolchain.fallback;
        result.resourceLimitsApplied = analysis.resourceLimitsApplied;
        Publish(work, std::move(result));
        return;
    }

    const std::string standardInputUtf8 = Maro_WideToUtf8(work.request.standardInput);
    if (standardInputUtf8.size() > limits.standardInputBytes)
    {
        Maro_ResultEnvelope result = Maro_BaseEnvelope(
            work.requestId,
            work.request,
            Maro_Phase::Completed,
            Maro_Status::LimitExceeded,
            L"입력 데이터가 설정된 실행 제한을 초과했습니다.");
        result.generatedSource = generated.text;
        result.snippetWrapped = generated.wrapped;
        result.compilerOutput = analysis.compilerOutput;
        result.diagnostics = std::move(analysis.diagnostics);
        result.diagnostics.push_back(Maro_MakeIdeFinding(
            work.request,
            L"SAFE-1001",
            Maro_Severity::Error,
            Maro_Evidence::StaticAnalysis,
            L"입력 크기를 줄인 뒤 다시 실행하세요."));
        result.compilerName = toolchain.name;
        result.compilerVersion = toolchain.version;
        result.usedFallbackCompiler = toolchain.fallback;
        result.resourceLimitsApplied = analysis.resourceLimitsApplied;
        Publish(work, std::move(result));
        return;
    }

    Maro_ResultEnvelope compiling = Maro_BaseEnvelope(
        work.requestId, work.request, Maro_Phase::Compiling, Maro_Status::Pending, L"실행 파일을 만드는 중…");
    compiling.generatedSource = generated.text;
    compiling.snippetWrapped = generated.wrapped;
    compiling.compilerOutput = analysis.compilerOutput;
    compiling.diagnostics = analysis.diagnostics;
    compiling.compilerName = toolchain.name;
    compiling.compilerVersion = toolchain.version;
    compiling.usedFallbackCompiler = toolchain.fallback;
    Publish(work, std::move(compiling));

    Maro_CompilationResult compilation = Maro_CompileSource(
        toolchain, work.request, generated, temporary.path(), limits, cancelled);
    if (compilation.cancelled || cancelled())
    {
        finishCancelled();
        return;
    }
    if (!compilation.succeeded)
    {
        const Maro_Status failureStatus = compilation.processStartFailed
            ? Maro_Status::SandboxUnavailable
            : (compilation.timedOut
                ? Maro_Status::TimedOut
                : (compilation.limitExceeded ? Maro_Status::LimitExceeded : Maro_Status::CompileFailed));
        const std::wstring failureText = compilation.processStartFailed
            ? L"제한된 컴파일러 프로세스를 시작하지 못했습니다."
            : (compilation.timedOut
                ? L"컴파일 시간이 제한을 초과했습니다."
                : (compilation.limitExceeded
                    ? L"컴파일 자원 제한을 초과했습니다."
                    : L"실행 파일을 만들지 못했습니다."));
        Maro_ResultEnvelope result = Maro_BaseEnvelope(
            work.requestId,
            work.request,
            Maro_Phase::Completed,
            failureStatus,
            failureText);
        result.generatedSource = generated.text;
        result.snippetWrapped = generated.wrapped;
        result.compilerOutput = compilation.compilerOutput;
        result.diagnostics = std::move(compilation.diagnostics);
        result.compilerName = toolchain.name;
        result.compilerVersion = toolchain.version;
        result.usedFallbackCompiler = toolchain.fallback;
        result.resourceLimitsApplied = compilation.resourceLimitsApplied;
        Publish(work, std::move(result));
        return;
    }

    Maro_ResultEnvelope running = Maro_BaseEnvelope(
        work.requestId, work.request, Maro_Phase::Running, Maro_Status::Pending, L"Job Object 자원 제한으로 실행 중…");
    running.executionId = work.requestId;
    running.generatedSource = generated.text;
    running.snippetWrapped = generated.wrapped;
    running.compilerOutput = compilation.compilerOutput;
    running.diagnostics = analysis.diagnostics;
    running.compilerName = toolchain.name;
    running.compilerVersion = toolchain.version;
    running.usedFallbackCompiler = toolchain.fallback;
    Publish(work, std::move(running));

    Maro_ProcessRequest processRequest;
    processRequest.executable = compilation.executablePath.wstring();
    processRequest.workingDirectory = temporary.path().wstring();
    processRequest.standardInputUtf8 = standardInputUtf8;
    processRequest.inheritEnvironment = false;
    wchar_t windowsDirectory[MAX_PATH]{};
    wchar_t systemDirectory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windowsDirectory, static_cast<UINT>(std::size(windowsDirectory))) != 0)
    {
        processRequest.environmentOverrides[L"SystemRoot"] = windowsDirectory;
        processRequest.environmentOverrides[L"WINDIR"] = windowsDirectory;
    }
    if (GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory))) != 0)
    {
        processRequest.environmentOverrides[L"PATH"] = systemDirectory;
    }
    processRequest.environmentOverrides[L"TEMP"] = temporary.path().wstring();
    processRequest.environmentOverrides[L"TMP"] = temporary.path().wstring();
    processRequest.limits.wallMilliseconds = limits.runWallMilliseconds;
    processRequest.limits.cpuMilliseconds = limits.runWallMilliseconds;
    processRequest.limits.memoryBytes = limits.runMemoryBytes;
    processRequest.limits.activeProcessLimit = 1;
    processRequest.limits.stdoutBytes = limits.standardOutputBytes;
    processRequest.limits.stderrBytes = limits.standardErrorBytes;
    const Maro_ProcessResult process = Maro_RunProcess(processRequest, cancelled);
    if (process.termination == Maro_ProcessTermination::Cancelled || cancelled())
    {
        finishCancelled();
        return;
    }

    Maro_Status finalStatus = Maro_Status::RuntimeFailed;
    std::wstring statusText = L"프로그램 실행이 비정상적으로 끝났습니다.";
    if (process.termination == Maro_ProcessTermination::WallTimedOut ||
        process.termination == Maro_ProcessTermination::CpuTimedOut)
    {
        finalStatus = Maro_Status::TimedOut;
        statusText = L"실행 시간이 제한을 초과해 프로세스 트리를 종료했습니다.";
    }
    else if (process.termination == Maro_ProcessTermination::OutputLimit ||
             process.termination == Maro_ProcessTermination::MemoryLimit ||
             process.termination == Maro_ProcessTermination::ProcessLimit)
    {
        finalStatus = Maro_Status::LimitExceeded;
        statusText = L"실행 자원 제한을 초과해 프로세스 트리를 종료했습니다.";
    }
    else if (process.termination == Maro_ProcessTermination::StartFailed ||
             process.termination == Maro_ProcessTermination::InternalError)
    {
        finalStatus = Maro_Status::SandboxUnavailable;
        statusText = L"제한된 실행 프로세스를 시작하지 못했습니다.";
    }
    else if (process.hasExitCode && process.exitCode == 0)
    {
        finalStatus = Maro_Status::Success;
        statusText = process.standardOutputUtf8.empty()
            ? L"정상 종료했습니다. 실제 stdout 출력은 없습니다."
            : L"정상 종료했습니다.";
    }
    else if (process.hasExitCode)
    {
        statusText = L"프로그램이 종료 코드 " + std::to_wstring(process.exitCode) + L"(으)로 끝났습니다.";
    }

    Maro_ResultEnvelope result = Maro_BaseEnvelope(
        work.requestId, work.request, Maro_Phase::Completed, finalStatus, std::move(statusText));
    result.executionId = work.requestId;
    result.generatedSource = generated.text;
    result.snippetWrapped = generated.wrapped;
    result.compilerOutput = compilation.compilerOutput;
    result.standardOutput = Maro_SanitizeOutput(Maro_Utf8ToWide(process.standardOutputUtf8));
    result.standardError = Maro_SanitizeOutput(Maro_Utf8ToWide(process.standardErrorUtf8));
    result.diagnostics = std::move(analysis.diagnostics);
    result.compilerName = toolchain.name;
    result.compilerVersion = toolchain.version;
    result.usedFallbackCompiler = toolchain.fallback;
    result.resourceLimitsApplied = process.jobObjectApplied;
    result.exitCode = process.exitCode;
    result.hasExitCode = process.hasExitCode;
    if (finalStatus == Maro_Status::Success && process.standardOutputUtf8.empty())
    {
        result.diagnostics.push_back(Maro_MakeIdeFinding(
            work.request, L"RUN-0000", Maro_Severity::Info, Maro_Evidence::RuntimeObservation,
            L"실행은 정상적으로 끝났으며 stdout에서 관찰된 출력은 없습니다."));
    }
    else if (finalStatus == Maro_Status::RuntimeFailed)
    {
        result.diagnostics.push_back(Maro_MakeIdeFinding(
            work.request, L"RUN-1002", Maro_Severity::Error, Maro_Evidence::RuntimeObservation,
            L"프로그램이 0이 아닌 종료 코드 또는 운영체제 예외로 종료되었습니다."));
    }
    Publish(work, std::move(result));
}
