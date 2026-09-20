#include "maro_Engine.hpp"
#include "maro_CodeDiagnostics.hpp"

#include "maro_Analyzer.hpp"
#include "maro_Process.hpp"
#include "maro_Project.hpp"
#include "maro_Text.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <utility>

namespace
{
namespace fs = std::filesystem;

struct maro_DecodedStream
{
    maro_DecodedStream(unsigned maro_page, std::size_t maro_limit)
        : maro_decoder(maro_page), maro_capacity(maro_limit)
    {
    }

    std::wstring maro_Decode(std::string_view maro_bytes, bool maro_final = false)
    {
        auto maro_chunk = Maro_SanitizeOutput(maro_decoder.maro_Decode(maro_bytes, maro_final));
        maro_text += maro_chunk;
        if (maro_text.size() > maro_capacity &&
            maro_text.size() - maro_capacity > (std::min)(maro_capacity, std::size_t{32u << 10}))
        {
            maro_Trim();
        }
        return maro_chunk;
    }

    void maro_Trim()
    {
        if (maro_text.size() <= maro_capacity) return;
        std::size_t maro_remove = maro_text.size() - maro_capacity;
        if (maro_remove < maro_text.size() && maro_text[maro_remove] >= 0xdc00 && maro_text[maro_remove] <= 0xdfff)
            ++maro_remove;
        maro_text.erase(0, maro_remove);
    }

    maro_OutputDecoder maro_decoder;
    std::size_t maro_capacity;
    std::wstring maro_text;
};

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
            (L"maro_CLive_Maro_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
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
        if (!error && parent == expectedRoot && fileName.starts_with(L"maro_CLive_Maro_"))
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
    diagnostic.analyzerVersion = L"2.3.4";
    diagnostic.severity = severity;
    diagnostic.evidence = evidence;
    diagnostic.friendlyMessage = std::move(message);
    return diagnostic;
}
}

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
    const auto maro_sourceHash = Maro_HashSource(request.sourceText);
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
        if (pending_ && pending_->request.maro_input) pending_->request.maro_input->maro_Close();
        pending_ = Maro_PendingWork{requestId, generation, std::move(request), maro_sourceHash};
    }
    condition_.notify_all();
    return requestId;
}

void Maro_Engine::Cancel()
{
    {
        std::lock_guard lock(mutex_);
        cancellationGeneration_.fetch_add(1, std::memory_order_acq_rel);
        currentRequestId_.store(0, std::memory_order_release);
        currentSourceVersion_.store(0, std::memory_order_release);
        if (pending_ && pending_->request.maro_input) pending_->request.maro_input->maro_Close();
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
        currentRequestId_.store(0, std::memory_order_release);
        currentSourceVersion_.store(0, std::memory_order_release);
        if (pending_ && pending_->request.maro_input) pending_->request.maro_input->maro_Close();
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
    result.sourceHash = work.maro_sourceHash;
    try
    {
        callback_(std::move(result));
    }
    catch (...)
    {
    }
}

void Maro_Engine::ProcessOne(const Maro_PendingWork& work, std::stop_token stopToken)
{
    struct maro_InputLifetime
    {
        std::shared_ptr<maro_ProcessInput> maro_input;
        ~maro_InputLifetime() { if (maro_input) maro_input->maro_Close(); }
    } maro_inputLifetime{work.request.maro_input};
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

    if (!work.request.maro_projectPath.empty())
    {
        maro_ProcessProject(work, cancelled);
        return;
    }
    if (work.request.maro_input)
    {
        limits.analysisWallMilliseconds = 60'000;
        limits.compileWallMilliseconds = 120'000;
        limits.compileMemoryBytes = 4ull << 30;
        limits.sourceBytes = 16u << 20;
    }

    Publish(work, Maro_BaseEnvelope(
        work.requestId, work.request, Maro_Phase::Generating, Maro_Status::Pending,
        work.request.mode == Maro_SourceMode::Snippet ? L"학습용 코드를 생성하는 중…" : L"소스를 준비하는 중…"));
    const Maro_GeneratedSource generated = maro_BuildGeneratedSource(work.request);
    if (cancelled())
    {
        finishCancelled();
        return;
    }

    static const Maro_ToolchainInfo maro_defaultToolchain = Maro_DetectToolchain();
    const auto& toolchain = [&]() -> const Maro_ToolchainInfo& {
        if (!work.request.maro_trace) return maro_defaultToolchain;
        static const Maro_ToolchainInfo maro_traceToolchain = Maro_DetectToolchain(true);
        return maro_traceToolchain;
    }();
    if (toolchain.kind == Maro_ToolchainKind::None)
    {
        Maro_ResultEnvelope result = Maro_BaseEnvelope(
            work.requestId, work.request, Maro_Phase::Completed, Maro_Status::ToolchainMissing,
            work.request.maro_trace ? L"실제 한 줄 실행에는 Visual Studio MSVC C/C++ 도구가 필요합니다."
                : L"Clang 또는 MSVC C/C++ 컴파일러를 찾지 못했습니다.");
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

    const bool maro_canRun = work.request.execute &&
        (work.request.mode == Maro_SourceMode::Snippet || Maro_HasMain(work.request.sourceText));
    Maro_AnalysisResult analysis;
    analysis.succeeded = true;
    const auto maro_analysisStarted = GetTickCount64();
    if (!maro_canRun)
    {
        analysis = Maro_AnalyzeSource(toolchain, work.request, generated, temporary.path(), limits, cancelled);
    }
    const auto maro_analysisMs = GetTickCount64() - maro_analysisStarted;
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
        result.maro_compileMilliseconds = maro_analysisMs;
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
        result.maro_compileMilliseconds = maro_analysisMs;
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
        result.maro_compileMilliseconds = maro_analysisMs;
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

    const auto maro_compileStarted = GetTickCount64();
    Maro_CompilationResult compilation = Maro_CompileSource(
        toolchain, work.request, generated, temporary.path(), limits, cancelled);
    const auto maro_compileMs = GetTickCount64() - maro_compileStarted;
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
        result.maro_compileMilliseconds = maro_compileMs;
        Publish(work, std::move(result));
        return;
    }

    Maro_ResultEnvelope running = Maro_BaseEnvelope(
        work.requestId, work.request, Maro_Phase::Running, Maro_Status::Pending, L"Job Object 자원 제한으로 실행 중…");
    running.executionId = work.requestId;
    running.generatedSource = generated.text;
    running.snippetWrapped = generated.wrapped;
    running.compilerOutput = compilation.compilerOutput;
    running.diagnostics = compilation.diagnostics;
    running.compilerName = toolchain.name;
    running.compilerVersion = toolchain.version;
    running.usedFallbackCompiler = toolchain.fallback;
    running.maro_compileMilliseconds = maro_compileMs;
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
    processRequest.maro_trace = work.request.maro_trace;
    processRequest.maro_background = work.request.maro_background;
    processRequest.maro_traceSource = (temporary.path() /
        (work.request.language == Maro_Language::C17 ? L"maro_UserSource.c" : L"maro_UserSource.cpp")).wstring();
    if (work.request.maro_input)
    {
        processRequest.maro_interactiveInput = work.request.maro_input;
        processRequest.maro_rollingOutput = true;
        processRequest.maro_allowGuiWindows = true;
        processRequest.inheritEnvironment = true;
        processRequest.environmentOverrides.erase(L"PATH");
        processRequest.limits.wallMilliseconds = 0;
        processRequest.limits.cpuMilliseconds = 0;
        processRequest.limits.memoryBytes = 0;
        processRequest.limits.activeProcessLimit = 0;
        processRequest.limits.maro_idleMilliseconds = 30'000;
        const auto maro_parent = fs::path(work.request.sourcePath).parent_path();
        std::error_code maro_error;
        if (!maro_parent.empty() && fs::is_directory(maro_parent, maro_error))
        {
            processRequest.workingDirectory = maro_parent.wstring();
        }
    }
    maro_DecodedStream maro_stdout(work.request.maro_outputCodePage, limits.standardOutputBytes);
    maro_DecodedStream maro_stderr(work.request.maro_outputCodePage, limits.standardErrorBytes);
    const auto maro_publishOutput = [this, &work, &maro_stdout, &maro_stderr](
        bool standardError, std::string_view bytes, bool maro_final = false) {
            auto text = (standardError ? maro_stderr : maro_stdout).maro_Decode(bytes, maro_final);
            if (text.empty()) return;
            Maro_ResultEnvelope update = Maro_BaseEnvelope(
                work.requestId,
                work.request,
                Maro_Phase::Running,
                Maro_Status::Pending,
                {});
            update.executionId = work.requestId;
            if (standardError)
            {
                update.standardError = std::move(text);
            }
            else
            {
                update.standardOutput = std::move(text);
            }
            Publish(work, std::move(update));
        };
    const auto maro_runStarted = GetTickCount64();
    const Maro_ProcessResult process = Maro_RunProcess(processRequest, cancelled,
        [&](bool maro_error, std::string_view maro_bytes) { maro_publishOutput(maro_error, maro_bytes); });
    maro_publishOutput(false, {}, true);
    maro_publishOutput(true, {}, true);
    maro_stdout.maro_Trim();
    maro_stderr.maro_Trim();
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
        statusText = work.request.maro_input ? L"출력 없이 30초 동안 계산이 계속되어 중지했습니다. 입력 대기는 제한하지 않습니다."
            : L"실행 시간이 제한을 초과해 프로세스 트리를 종료했습니다.";
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
    result.standardOutput = std::move(maro_stdout.maro_text);
    result.standardError = std::move(maro_stderr.maro_text);
    result.diagnostics = std::move(compilation.diagnostics);
    result.compilerName = toolchain.name;
    result.compilerVersion = toolchain.version;
    result.usedFallbackCompiler = toolchain.fallback;
    result.resourceLimitsApplied = process.jobObjectApplied;
    result.exitCode = process.exitCode;
    result.hasExitCode = process.hasExitCode;
    result.maro_compileMilliseconds = maro_compileMs;
    result.maro_runMilliseconds = GetTickCount64() - maro_runStarted;
    if (finalStatus == Maro_Status::TimedOut)
        result.diagnostics.push_back(maro_MakeTimeoutDiagnostic(work.request));
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

void Maro_Engine::maro_ProcessProject(const Maro_PendingWork& maro_work,
    const std::function<bool()>& maro_cancelled)
{
    maro_ProjectRequest maro_request;
    maro_request.maro_projectPath = maro_work.request.maro_projectPath;
    maro_request.maro_configuration = maro_work.request.maro_configuration;
    maro_request.maro_platform = maro_work.request.maro_platform;
    maro_request.maro_msbuildPath = maro_work.request.maro_msbuildPath;
    maro_request.maro_solutionPath = maro_work.request.maro_solutionPath;
    maro_request.maro_background = maro_work.request.maro_background && !maro_work.request.maro_trace;
    Publish(maro_work, Maro_BaseEnvelope(maro_work.requestId, maro_work.request,
        Maro_Phase::Analyzing, Maro_Status::Pending, L"프로젝트 빌드 중..."));
    maro_OutputDecoder maro_buildStdout;
    maro_OutputDecoder maro_buildStderr;
    const auto maro_buildStarted = GetTickCount64();
    const auto maro_project = maro_BuildProject(maro_request, maro_cancelled,
        [this, &maro_work, &maro_buildStdout, &maro_buildStderr](bool maro_error, std::string_view maro_bytes) {
            auto maro_text = Maro_SanitizeOutput((maro_error ? maro_buildStderr : maro_buildStdout).maro_Decode(maro_bytes));
            if (maro_text.empty()) return;
            auto maro_update = Maro_BaseEnvelope(maro_work.requestId, maro_work.request,
                Maro_Phase::Analyzing, Maro_Status::Pending, {});
            maro_update.compilerOutput = std::move(maro_text);
            Publish(maro_work, std::move(maro_update));
        });
    const auto maro_buildMs = GetTickCount64() - maro_buildStarted;
    if (maro_cancelled()) return;
    auto maro_result = Maro_BaseEnvelope(maro_work.requestId, maro_work.request,
        Maro_Phase::Completed, maro_project.maro_success ? Maro_Status::Success : Maro_Status::CompileFailed,
        maro_project.maro_message);
    maro_result.compilerOutput = maro_project.maro_buildOutput;
    maro_result.maro_compileMilliseconds = maro_buildMs;
    const auto maro_generated = maro_BuildGeneratedSource(maro_work.request);
    maro_result.diagnostics = Maro_ParseCompilerDiagnostics(maro_project.maro_buildOutput,
        maro_work.request, maro_generated, L"MSBuild", {}, maro_work.request.sourcePath);
    maro_ImproveDiagnostics(maro_work.request, maro_result.diagnostics);
    if (!maro_project.maro_success || !maro_work.request.execute || maro_project.maro_executablePath.empty())
    {
        Publish(maro_work, std::move(maro_result));
        return;
    }
    auto maro_checked = maro_result;
    maro_checked.phase = Maro_Phase::Compiling;
    Publish(maro_work, std::move(maro_checked));
    auto maro_running = maro_result;
    maro_running.phase = Maro_Phase::Running;
    maro_running.status = Maro_Status::Pending;
    maro_running.statusText = L"프로젝트 실행 중...";
    maro_running.executionId = maro_work.requestId;
    maro_running.maro_compileMilliseconds = maro_buildMs;
    Publish(maro_work, std::move(maro_running));
    Maro_ProcessRequest maro_process;
    maro_process.executable = maro_project.maro_executablePath;
    maro_process.workingDirectory = maro_project.maro_workingDirectory;
    maro_process.arguments = maro_project.maro_arguments;
    maro_process.environmentOverrides = maro_project.maro_environment;
    maro_process.inheritEnvironment = maro_project.maro_inheritEnvironment;
    maro_process.maro_interactiveInput = maro_work.request.maro_input;
    maro_process.maro_rollingOutput = true;
    maro_process.maro_allowGuiWindows = true;
    maro_process.maro_trace = maro_work.request.maro_trace;
    maro_process.maro_background = maro_work.request.maro_background;
    maro_process.maro_traceSource = maro_work.request.sourcePath;
    maro_process.limits = {0, 0, 0, 0, 1u << 20, 1u << 20};
    maro_process.limits.maro_idleMilliseconds = 30'000;
    maro_DecodedStream maro_stdout(maro_work.request.maro_outputCodePage, maro_process.limits.stdoutBytes);
    maro_DecodedStream maro_stderr(maro_work.request.maro_outputCodePage, maro_process.limits.stderrBytes);
    const auto maro_publishOutput = [this, &maro_work, &maro_stdout, &maro_stderr](
        bool maro_error, std::string_view maro_bytes, bool maro_final = false) {
            auto maro_text = (maro_error ? maro_stderr : maro_stdout).maro_Decode(maro_bytes, maro_final);
            if (maro_text.empty()) return;
            auto maro_update = Maro_BaseEnvelope(maro_work.requestId, maro_work.request,
                Maro_Phase::Running, Maro_Status::Pending, {});
            maro_update.executionId = maro_work.requestId;
            (maro_error ? maro_update.standardError : maro_update.standardOutput) =
                std::move(maro_text);
            Publish(maro_work, std::move(maro_update));
        };
    const auto maro_runStarted = GetTickCount64();
    const auto maro_run = Maro_RunProcess(maro_process, maro_cancelled,
        [&](bool maro_error, std::string_view maro_bytes) { maro_publishOutput(maro_error, maro_bytes); });
    maro_publishOutput(false, {}, true);
    maro_publishOutput(true, {}, true);
    maro_stdout.maro_Trim();
    maro_stderr.maro_Trim();
    if (maro_cancelled()) return;
    maro_result.status = maro_run.hasExitCode && maro_run.exitCode == 0
        ? Maro_Status::Success : Maro_Status::RuntimeFailed;
    maro_result.statusText = maro_result.status == Maro_Status::Success ? L"정상 종료했습니다."
        : L"프로그램 실행 실패: " + std::to_wstring(maro_run.hasExitCode ? maro_run.exitCode : maro_run.win32Error);
    if (maro_run.termination == Maro_ProcessTermination::WallTimedOut)
    {
        maro_result.status = Maro_Status::TimedOut;
        maro_result.statusText = L"출력 없이 30초 동안 계산이 계속되어 중지했습니다. 입력 대기는 제한하지 않습니다.";
        maro_result.diagnostics.push_back(maro_MakeTimeoutDiagnostic(maro_work.request, true));
    }
    maro_result.executionId = maro_work.requestId;
    maro_result.hasExitCode = maro_run.hasExitCode;
    maro_result.exitCode = maro_run.exitCode;
    maro_result.maro_runMilliseconds = GetTickCount64() - maro_runStarted;
    maro_result.resourceLimitsApplied = maro_run.jobObjectApplied;
    maro_result.standardOutput = std::move(maro_stdout.maro_text);
    maro_result.standardError = std::move(maro_stderr.maro_text);
    Publish(maro_work, std::move(maro_result));
}
