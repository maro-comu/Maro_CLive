#include "maro_CLive_Maro_Package.hpp"
#include "maro_Text.hpp"
#include "maro_VisualStudio.hpp"
#include "maro_Update.hpp"
#include "maro_Process.hpp"
#include "maro_DiagnosticFilter.hpp"

#include <filesystem>
#include <new>
#include <sstream>
#include <utility>

const CLSID Maro_CLive_Maro_PackageClsid =
    {0xc766c863, 0x83a1, 0x4fcf, {0xbb, 0xf5, 0xf2, 0x6f, 0xfc, 0x83, 0x79, 0x6b}};
const GUID Maro_CLive_Maro_CommandSet =
    {0x24748981, 0x75eb, 0x4e44, {0xbc, 0xbe, 0x9f, 0x92, 0x0d, 0x16, 0xd3, 0xc3}};
const GUID Maro_CLive_Maro_DiagnosticPane =
    {0x315cbbd5, 0xcc25, 0x4567, {0xbc, 0x11, 0x70, 0x21, 0xb3, 0xf8, 0xd8, 0x40}};
const GUID Maro_CLive_Maro_RunPane =
    {0xe3b0e987, 0xb59e, 0x4be9, {0x92, 0xa2, 0x8f, 0x82, 0x66, 0x1d, 0x51, 0x78}};

OBJECT_ENTRY_AUTO(Maro_CLive_Maro_PackageClsid, Maro_CLive_Maro_Package)

namespace
{
const GUID maro_LiveOutputGuid = {0x3c3f69d0,0x7aa4,0x4ba2,{0x85,0xd8,0x36,0xfa,0x6d,0xa9,0x65,0xc2}};
bool maro_ReadSourcePath(IVsTextLines* lines, std::wstring& path)
{
    path.clear();
    if (lines == nullptr)
    {
        return false;
    }
    ATL::CComQIPtr<IPersistFileFormat> file(lines);
    if (file != nullptr)
    {
        LPOLESTR rawPath = nullptr;
        DWORD format = 0;
        const HRESULT result = file->GetCurFile(&rawPath, &format);
        if (SUCCEEDED(result) && rawPath != nullptr)
        {
            path = rawPath;
        }
        CoTaskMemFree(rawPath);
    }
    if (!path.empty())
    {
        return Maro_IsVisualStudioCppPath(path);
    }
    constexpr GUID maro_CppLanguage =
        {0xb2f072b0, 0xabc1, 0x11d0, {0x9d, 0x62, 0x00, 0xc0, 0x4f, 0xd9, 0xdf, 0xd9}};
    GUID language = GUID_NULL;
    return SUCCEEDED(lines->GetLanguageServiceID(&language)) && language == maro_CppLanguage;
}

const wchar_t* Maro_SeverityText(Maro_Severity severity) noexcept
{
    switch (severity)
    {
    case Maro_Severity::Warning: return L"warning";
    case Maro_Severity::Error: return L"error";
    case Maro_Severity::Fatal: return L"fatal";
    default: return L"info";
    }
}

bool Maro_IsCompleted(const Maro_ResultEnvelope& result) noexcept
{
    return result.phase == Maro_Phase::Completed;
}

void maro_WritePane(IVsOutputWindowPane* pane, const std::wstring& text)
{
    if (pane == nullptr || text.empty())
    {
        return;
    }
    ATL::CComPtr<IVsOutputWindowPane> owned = pane;
    ATL::CComQIPtr<IVsOutputWindowPaneNoPump> noPump(owned);
    if (noPump != nullptr)
    {
        noPump->OutputStringNoPump(text.c_str());
    }
    else
    {
        owned->OutputStringThreadSafe(text.c_str());
    }
}

}

STDMETHODIMP Maro_CLive_Maro_Package::Initialize(
    IAsyncServiceProvider*,
    IProfferAsyncService*,
    IAsyncProgressCallback*,
    IVsTask** task)
{
    if (task == nullptr)
    {
        return E_POINTER;
    }
    *task = nullptr;
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::SetSite(IServiceProvider* serviceProvider)
{
    try
    {
        if (serviceProvider == nullptr)
        {
            Shutdown();
            return S_OK;
        }

        if (serviceProvider_ != nullptr)
        {
            return S_OK;
        }
        shuttingDown_ = false;
        initializationResult_ = E_PENDING;
        serviceProvider_ = serviceProvider;
        liveInstance_ = this;
        uiTimer_ = SetTimer(nullptr, 0, 100, UiTimerProc);
        if (uiTimer_ == 0)
        {
            Shutdown();
            return E_FAIL;
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        Shutdown();
        return E_OUTOFMEMORY;
    }
    catch (...)
    {
        Shutdown();
        return E_FAIL;
    }
}

HRESULT Maro_CLive_Maro_Package::InitializeUi()
{
    if (initializing_ || shuttingDown_ || serviceProvider_ == nullptr)
    {
        return E_PENDING;
    }
    if (initialized_)
    {
        return S_OK;
    }
    initializing_ = true;
    HRESULT result = E_FAIL;
    try
    {
        result = EnsureOutputPanes();
        if (SUCCEEDED(result) && !shuttingDown_)
        {
            diagnosticEngine_ = std::make_unique<Maro_Engine>([this](Maro_ResultEnvelope result) {
                PublishDiagnosticResult(result);
            });
            runEngine_ = std::make_unique<Maro_Engine>([this](Maro_ResultEnvelope result) {
                PublishRunResult(result);
            });
            maro_insightWorker_ = std::make_unique<maro_InsightWorker>();
            initialized_ = true;
            serviceProvider_->QueryService(SID_SVsRunningDocumentTable, IID_IVsRunningDocumentTable,
                reinterpret_cast<void**>(&maro_documents_));
            if (maro_documents_ != nullptr)
            {
                maro_documents_->AdviseRunningDocTableEvents(this, &maro_documentCookie_);
            }
            StartLiveTracking();
            EnsureDiagnosticWindow(false);
            maro_EnsureLiveOutput();
            maro_EnsureSourceWindow();
        }
        if (shuttingDown_)
        {
            result = E_ABORT;
            initialized_ = false;
        }
    }
    catch (const std::bad_alloc&)
    {
        result = E_OUTOFMEMORY;
    }
    catch (...)
    {
        result = E_FAIL;
    }
    initializationResult_ = result;
    initializing_ = false;
    return result;
}

void CALLBACK Maro_CLive_Maro_Package::UiTimerProc(HWND, UINT, UINT_PTR timer, DWORD) noexcept
{
    auto* instance = liveInstance_;
    if (instance != nullptr && instance->uiTimer_ == timer)
    {
        ATL::CComPtr<IVsPackage> lifetime = instance;
        instance->ProcessUi();
    }
}

void Maro_CLive_Maro_Package::ProcessUi() noexcept
{
    if (uiBusy_ || initializing_ || shuttingDown_)
    {
        return;
    }
    uiBusy_ = true;
    try
    {
        if (!initialized_ && initializationResult_ == E_PENDING)
        {
            InitializeUi();
        }
        if (initialized_ && !shuttingDown_)
        {
            if (std::exchange(maro_clearOutputPending_, false))
            {
                if (diagnosticWindow_ != nullptr)
                {
                    diagnosticWindow_->maro_ClearOutput();
                }
                if (maro_liveOutput_) maro_liveOutput_->maro_ClearOutput();
                runPane_->Clear();
            }
            const DWORD maro_commands = maro_pendingCommands_.exchange(0);
            HRESULT maro_commandResult = S_OK;
            if ((maro_commands & 16) != 0)
            {
                maro_CancelLiveWork();
                maro_clearOutputPending_ = false;
                liveDeadline_ = 0;
                maro_stopped_ = true;
                updateNotice_.Push(L"중지했습니다.");
            }
            if ((maro_commands & 32) != 0)
            {
                maro_CancelLiveWork();
                maro_projectMode_ = !maro_projectMode_;
                maro_stopped_ = false;
                if (diagnosticWindow_) diagnosticWindow_->maro_SetProjectMode(maro_projectMode_);
                if (maro_liveOutput_) maro_liveOutput_->maro_SetProjectMode(maro_projectMode_);
                updateNotice_.Push(maro_projectMode_ ? L"프로젝트 모드 · 수정 파일을 모두 저장하세요." : L"단일 파일 모드");
                ScheduleLiveAnalysis();
            }
            if ((maro_commands & 128) != 0)
            {
                maro_automatic_ = !maro_automatic_;
                maro_CancelLiveWork();
                maro_stopped_ = false;
                if (diagnosticWindow_) diagnosticWindow_->maro_SetAutomatic(maro_automatic_);
                if (maro_liveOutput_) maro_liveOutput_->maro_SetAutomatic(maro_automatic_);
                updateNotice_.Push(maro_automatic_ ? L"자동 실행 켜짐" : L"자동 실행 꺼짐 · 외부 컴파일과 실행을 중지했습니다.");
                ScheduleLiveAnalysis();
            }
            if ((maro_commands & 64) != 0) maro_commandResult = maro_StartTrace();
            if (maro_navigation_)
            {
                auto maro_item = std::move(*maro_navigation_);
                maro_navigation_.reset();
                if (maro_insightHash_ != 0 && maro_navigationVersion_ == maro_insightVersion_)
                    maro_Navigate(maro_item);
            }
            if ((maro_commands & 3) != 0 && !shuttingDown_)
            {
                maro_commandResult = StartAnalysis((maro_commands & 2) != 0);
            }
            if ((maro_commands & 8) != 0 && !shuttingDown_)
            {
                maro_commandResult = EnsureDiagnosticWindow(true);
                maro_EnsureSourceWindow();
                maro_EnsureLiveOutput();
            }
            if ((maro_commands & 256) != 0 && SUCCEEDED(maro_EnsureLiveOutput()))
                maro_liveOutput_->maro_ToggleExpanded();
            if ((maro_commands & 4) != 0 && !shuttingDown_)
            {
                maro_commandResult = StartUpdate();
            }
            if (shuttingDown_)
            {
                uiBusy_ = false;
                return;
            }
            if (FAILED(maro_commandResult))
            {
                updateNotice_.Push(L"요청을 처리하지 못했습니다. 다시 시도해 주세요.");
            }
            if (liveDeadline_ != 0 && GetTickCount64() >= liveDeadline_)
            {
                liveDeadline_ = 0;
                RunLiveAnalysis();
            }
            if (maro_insightWorker_)
            {
                auto maro_snapshot = maro_insightWorker_->maro_Take();
                if (maro_snapshot && maro_snapshot->maro_version == maro_insightVersion_)
                {
                    if (diagnosticWindow_) diagnosticWindow_->maro_SetSource(maro_snapshot->maro_insight);
                    if (maro_liveOutput_) maro_liveOutput_->maro_SetSource(maro_snapshot->maro_insight);
                    if (maro_sourceWindow_) maro_sourceWindow_->maro_SetSource(std::move(maro_snapshot->maro_insight));
                    maro_displayedInsightVersion_ = maro_insightVersion_;
                }
            }
            {
                std::optional<maro_TraceSnapshot> maro_snapshot;
                { std::lock_guard maro_lock(maro_traceMutex_); maro_snapshot.swap(maro_traceSnapshot_); }
                if (maro_snapshot && diagnosticWindow_) diagnosticWindow_->maro_SetTrace(*maro_snapshot);
                if (maro_snapshot && maro_liveOutput_) maro_liveOutput_->maro_SetTrace(*maro_snapshot);
            }
            std::optional<Maro_ResultEnvelope> snapshot;
            {
                std::lock_guard lock(diagnosticMutex_);
                snapshot.swap(pendingDiagnostic_);
            }
            if (diagnosticWindow_ != nullptr && snapshot &&
                snapshot->sourceVersion == diagnosticSourceVersion_.load(std::memory_order_acquire))
            {
                diagnosticWindow_->maro_SetResult(*snapshot);
                if (snapshot->maro_compileMilliseconds != 0)
                    maro_debounceMilliseconds_ = (std::max)(std::uint64_t{1000},
                        (std::min)(std::uint64_t{3000}, snapshot->maro_compileMilliseconds / 2));
            }
            auto notice = updateNotice_.Take(16 * 1024);
            if (!notice.empty())
            {
                if (diagnosticWindow_) diagnosticWindow_->maro_SetNotice(notice);
                if (maro_liveOutput_) maro_liveOutput_->maro_SetNotice(std::move(notice));
            }
            const auto maro_diagnosticVersion = diagnosticSourceVersion_.load(std::memory_order_acquire);
            const auto maro_outputVersion = runDiagnosticVersion_.load(std::memory_order_acquire);
            const auto diagnostic = diagnosticOutput_.Take(8 * 1024);
            const auto output = runOutput_.Take(8 * 1024);
            if (!shuttingDown_ &&
                maro_diagnosticVersion == diagnosticSourceVersion_.load(std::memory_order_acquire))
            {
                if (maro_diagnosticPaneSize_ + diagnostic.size() > 1u << 20)
                {
                    diagnosticPane_->Clear();
                    maro_diagnosticPaneSize_ = 0;
                }
                maro_diagnosticPaneSize_ += diagnostic.size();
                maro_WritePane(diagnosticPane_, diagnostic);
            }
            if (!shuttingDown_ && maro_outputVersion == runDiagnosticVersion_.load(std::memory_order_acquire))
            {
                if (maro_liveOutput_ != nullptr)
                {
                    maro_liveOutput_->maro_AppendOutput(output);
                }
                if (maro_outputPaneSize_ + output.size() > 1u << 20)
                {
                    runPane_->Clear();
                    maro_outputPaneSize_ = 0;
                }
                maro_outputPaneSize_ += output.size();
                maro_WritePane(runPane_, output);
            }
            for (auto* maro_pane : {diagnosticWindow_.p, maro_liveOutput_.p})
            {
                if (!maro_pane) continue;
                const auto maro_current = runDiagnosticVersion_.load(std::memory_order_acquire);
                maro_pane->maro_SetSessionState(maro_current != 0 &&
                    maro_finishedVersion_.load(std::memory_order_acquire) != maro_current,
                    maro_current != 0 && maro_runningVersion_.load(std::memory_order_acquire) == maro_current &&
                    maro_input_ && maro_input_->maro_IsOpen());
            }
        }
    }
    catch (...)
    {
    }
    if (!shuttingDown_ && uiTimer_ != 0)
    {
        const auto maro_current = runDiagnosticVersion_.load(std::memory_order_acquire);
        const UINT maro_interval = liveDeadline_ != 0 || updateRunning_.load() ||
            (maro_current != 0 && maro_finishedVersion_.load() != maro_current) ? 100 : 500;
        if (maro_interval != maro_idleInterval_)
        {
            const auto maro_timer = SetTimer(nullptr, uiTimer_, maro_interval, UiTimerProc);
            if (maro_timer) { uiTimer_ = maro_timer; maro_idleInterval_ = maro_interval; }
        }
    }
    uiBusy_ = false;
}

HRESULT Maro_CLive_Maro_Package::EnsureDiagnosticWindow(bool activate, bool show)
{
    if (shuttingDown_ || serviceProvider_ == nullptr || creatingWindow_)
    {
        return E_PENDING;
    }
    if (diagnosticFrame_ == nullptr)
    {
        creatingWindow_ = true;
        ATL::CComPtr<IVsUIShell> shell;
        HRESULT result = serviceProvider_->QueryService(SID_SVsUIShell, IID_IVsUIShell,
            reinterpret_cast<void**>(&shell));
        if (SUCCEEDED(result) && shell != nullptr && !shuttingDown_)
        {
            ATL::CComObject<maro_DiagnosticWindow>* pane = nullptr;
            result = ATL::CComObject<maro_DiagnosticWindow>::CreateInstance(&pane);
            if (SUCCEEDED(result))
            {
                diagnosticWindow_ = pane;
                maro_ConfigurePane(pane, false);
                BOOL defaultPosition = FALSE;
                result = shell->CreateToolWindow(CTW_fInitNew, 0,
                    static_cast<IVsWindowPane*>(pane), GUID_NULL, maro_DiagnosticWindowGuid,
                    GUID_NULL, serviceProvider_, L"CLive_Maro 디버그", &defaultPosition, &diagnosticFrame_);
                if (FAILED(result))
                {
                    diagnosticWindow_.Release();
                }
            }
        }
        creatingWindow_ = false;
        if (FAILED(result))
        {
            return result;
        }
    }
    if (diagnosticFrame_ == nullptr || shuttingDown_)
    {
        return E_FAIL;
    }
    return !show ? S_OK : activate ? diagnosticFrame_->Show() : diagnosticFrame_->ShowNoActivate();
}

void Maro_CLive_Maro_Package::maro_ConfigurePane(maro_DiagnosticWindow* maro_pane, bool maro_output)
{
    maro_pane->maro_SetOutputMode(maro_output);
    if (!maro_output) maro_pane->maro_SetExpandCallback([this] { maro_pendingCommands_.fetch_or(256); });
    maro_pane->maro_SetRunCallback([this] { maro_pendingCommands_.fetch_or(2); });
    maro_pane->maro_SetSessionCallbacks([this](std::wstring maro_text) {
        if (shuttingDown_ || !maro_input_ || runDiagnosticVersion_.load() == 0 ||
            maro_runningVersion_.load() != runDiagnosticVersion_.load()) return false;
        std::string maro_bytes;
        const auto maro_line = maro_text + L"\n";
        if (maro_codePage_ == 0 || maro_codePage_ == CP_UTF8) maro_bytes = Maro_WideToUtf8(maro_line);
        else
        {
            BOOL maro_lossy = FALSE;
            const int maro_size = WideCharToMultiByte(maro_codePage_, WC_NO_BEST_FIT_CHARS,
                maro_line.data(), static_cast<int>(maro_line.size()), nullptr, 0, nullptr, &maro_lossy);
            if (!maro_size || maro_lossy) return false;
            maro_bytes.resize(maro_size);
            WideCharToMultiByte(maro_codePage_, WC_NO_BEST_FIT_CHARS, maro_line.data(),
                static_cast<int>(maro_line.size()), maro_bytes.data(), maro_size, nullptr, &maro_lossy);
            if (maro_lossy) return false;
        }
        if (!maro_input_->maro_Submit(maro_bytes)) return false;
        runOutput_.maro_PushVersion(runDiagnosticVersion_.load(), maro_text + L"\r\n");
        return true;
    }, [this] { if (maro_input_) maro_input_->maro_End(); },
        [this] { maro_pendingCommands_.fetch_or(16); });
    maro_pane->maro_SetProjectCallback([this] { maro_pendingCommands_.fetch_or(32); });
    maro_pane->maro_SetProjectMode(maro_projectMode_);
    maro_pane->maro_SetAutomaticCallback([this] { maro_pendingCommands_.fetch_or(128); });
    maro_pane->maro_SetAutomatic(maro_automatic_);
    maro_pane->maro_SetEncodingCallback([this](unsigned maro_page) {
        maro_codePage_ = maro_page;
        if (diagnosticWindow_) diagnosticWindow_->maro_SetEncoding(maro_page);
        if (maro_liveOutput_) maro_liveOutput_->maro_SetEncoding(maro_page);
        maro_pendingCommands_.fetch_or(16);
        updateNotice_.Push(L"인코딩을 변경했습니다. F5로 다시 실행하세요.");
    });
    maro_pane->maro_SetEncoding(maro_codePage_);
    maro_pane->maro_SetTraceCallbacks([this] { maro_pendingCommands_.fetch_or(64); },
        [this] { if (maro_trace_) maro_trace_->maro_Step(); },
        [this] { if (maro_trace_) maro_trace_->maro_Continue(); });
}

HRESULT Maro_CLive_Maro_Package::maro_EnsureLiveOutput(bool maro_show)
{
    if (shuttingDown_ || !serviceProvider_ || maro_creatingOutput_) return E_PENDING;
    if (!maro_liveOutputFrame_)
    {
        maro_creatingOutput_ = true;
        ATL::CComPtr<IVsUIShell> maro_shell;
        HRESULT maro_result = serviceProvider_->QueryService(SID_SVsUIShell, IID_IVsUIShell,
            reinterpret_cast<void**>(&maro_shell));
        if (SUCCEEDED(maro_result) && maro_shell)
        {
            ATL::CComObject<maro_DiagnosticWindow>* maro_pane = nullptr;
            maro_result = ATL::CComObject<maro_DiagnosticWindow>::CreateInstance(&maro_pane);
            if (SUCCEEDED(maro_result))
            {
                maro_liveOutput_ = maro_pane;
                maro_ConfigurePane(maro_pane, true);
                BOOL maro_default = FALSE;
                maro_result = maro_shell->CreateToolWindow(CTW_fInitNew, 0, static_cast<IVsWindowPane*>(maro_pane),
                    GUID_NULL, maro_LiveOutputGuid, GUID_NULL, serviceProvider_, L"CLive_Maro 실시간 출력",
                    &maro_default, &maro_liveOutputFrame_);
                if (FAILED(maro_result)) maro_liveOutput_.Release();
            }
        }
        maro_creatingOutput_ = false;
        if (FAILED(maro_result)) return maro_result;
    }
    return maro_liveOutputFrame_ ? (maro_show ? maro_liveOutputFrame_->ShowNoActivate() : S_OK) : E_FAIL;
}

void Maro_CLive_Maro_Package::SetDiagnosticPending(const std::wstring& path, const wchar_t* status)
{
    diagnosticSourceVersion_.store(path.empty() ? 0 : sourceVersion_, std::memory_order_release);
    if (diagnosticWindow_ != nullptr)
    {
        diagnosticWindow_->maro_SetPending(path, status);
    }
}

HRESULT Maro_CLive_Maro_Package::maro_EnsureSourceWindow(bool maro_show)
{
    if (shuttingDown_ || !serviceProvider_ || maro_creatingSource_) return E_PENDING;
    if (!maro_sourceFrame_)
    {
        maro_creatingSource_ = true;
        ATL::CComPtr<IVsUIShell> maro_shell;
        HRESULT maro_result = serviceProvider_->QueryService(SID_SVsUIShell, IID_IVsUIShell,
            reinterpret_cast<void**>(&maro_shell));
        if (SUCCEEDED(maro_result) && maro_shell)
        {
            ATL::CComObject<maro_SourceWindow>* maro_pane = nullptr;
            maro_result = ATL::CComObject<maro_SourceWindow>::CreateInstance(&maro_pane);
            if (SUCCEEDED(maro_result))
            {
                maro_sourceWindow_ = maro_pane;
                maro_pane->maro_SetNavigate([this](const maro_SourceItem& maro_item) {
                    if (!shuttingDown_ && maro_insightHash_ != 0 && maro_displayedInsightVersion_ == maro_insightVersion_)
                    {
                        maro_navigation_ = maro_item;
                        maro_navigationVersion_ = maro_insightVersion_;
                    }
                });
                BOOL maro_default = FALSE;
                maro_result = maro_shell->CreateToolWindow(CTW_fInitNew, 0, static_cast<IVsWindowPane*>(maro_pane),
                    GUID_NULL, maro_SourceWindowGuid, GUID_NULL, serviceProvider_, L"CLive_Maro 코드 목록",
                    &maro_default, &maro_sourceFrame_);
                if (FAILED(maro_result)) maro_sourceWindow_.Release();
            }
        }
        maro_creatingSource_ = false;
        if (FAILED(maro_result)) return maro_result;
    }
    return maro_sourceFrame_ ? (maro_show ? maro_sourceFrame_->ShowNoActivate() : S_OK) : E_FAIL;
}

void Maro_CLive_Maro_Package::maro_RequestInsight(const Maro_SourceRequest& maro_request)
{
    const auto maro_hash = Maro_HashSource(maro_request.sourceText);
    if (maro_insightHash_ == maro_hash && maro_insightPath_ == maro_request.sourcePath) return;
    maro_insightHash_ = maro_hash;
    maro_insightPath_ = maro_request.sourcePath;
    ++maro_insightVersion_;
    if (maro_insightWorker_) maro_insightWorker_->maro_Submit(maro_insightVersion_,
        maro_request.sourceText, maro_request.sourcePath);
}

void Maro_CLive_Maro_Package::maro_Navigate(const maro_SourceItem& maro_item)
{
    if (!serviceProvider_ || maro_item.maro_path.empty() || maro_item.maro_line == 0) return;
    ATL::CComPtr<IVsUIShellOpenDocument> maro_open;
    serviceProvider_->QueryService(SID_SVsUIShellOpenDocument, IID_IVsUIShellOpenDocument,
        reinterpret_cast<void**>(&maro_open));
    ATL::CComPtr<IServiceProvider> maro_provider;
    ATL::CComPtr<IVsUIHierarchy> maro_hierarchy;
    ATL::CComPtr<IVsWindowFrame> maro_frame;
    VSITEMID maro_id = VSITEMID_NIL;
    if (!maro_open || FAILED(maro_open->OpenDocumentViaProject(maro_item.maro_path.c_str(), LOGVIEWID_TextView,
        &maro_provider, &maro_hierarchy, &maro_id, &maro_frame)) || !maro_frame) return;
    maro_frame->Show();
    ATL::CComVariant maro_viewValue;
    if (FAILED(maro_frame->GetProperty(VSFPROPID_DocView, &maro_viewValue))) return;
    IUnknown* maro_unknown = maro_viewValue.vt == VT_UNKNOWN ? maro_viewValue.punkVal :
        maro_viewValue.vt == VT_DISPATCH ? maro_viewValue.pdispVal : nullptr;
    ATL::CComQIPtr<IVsTextView> maro_view(maro_unknown);
    if (!maro_view)
    {
        ATL::CComQIPtr<IVsCodeWindow> maro_code(maro_unknown);
        if (maro_code) maro_code->GetPrimaryView(&maro_view);
    }
    if (maro_view)
    {
        const auto maro_line = static_cast<long>(maro_item.maro_line - 1);
        const auto maro_column = static_cast<long>(maro_item.maro_column ? maro_item.maro_column - 1 : 0);
        maro_view->SetCaretPos(maro_line, maro_column);
        maro_view->EnsureSpanVisible(TextSpan{maro_line, maro_column, maro_line, maro_column});
    }
}

HRESULT Maro_CLive_Maro_Package::maro_StartTrace()
{
    if (maro_projectMode_)
    {
        updateNotice_.Push(L"줄별 실행은 파일 모드의 MSVC x64 코드에서 지원합니다. 프로젝트는 실행 출력과 코드 미리보기를 사용하세요.");
        return S_OK;
    }
    Maro_SourceRequest maro_request;
    std::wstring maro_path;
    if (FAILED(ReadActiveSource(maro_request, maro_path))) return S_FALSE;
    const auto maro_version = maro_request.sourceVersion;
    const auto maro_sourcePath = maro_request.sourcePath;
    maro_request.maro_trace = std::make_shared<maro_TraceSession>([this, maro_version, maro_sourcePath](const maro_TraceSnapshot& maro_snapshot) {
        if (runDiagnosticVersion_.load(std::memory_order_acquire) != maro_version) return;
        auto maro_mapped = maro_snapshot;
        if (maro_mapped.maro_line) maro_mapped.maro_file = maro_sourcePath;
        for (auto& maro_frame : maro_mapped.maro_frames)
            if (_wcsicmp(maro_frame.maro_file.c_str(), maro_snapshot.maro_file.c_str()) == 0)
                maro_frame.maro_file = maro_sourcePath;
        std::lock_guard maro_lock(maro_traceMutex_);
        if (runDiagnosticVersion_.load(std::memory_order_acquire) == maro_version) maro_traceSnapshot_ = std::move(maro_mapped);
    });
    maro_stopped_ = false;
    return maro_SubmitSource(std::move(maro_request), maro_path, true, true);
}

STDMETHODIMP Maro_CLive_Maro_Package::QueryClose(BOOL* canClose)
{
    if (canClose == nullptr)
    {
        return E_POINTER;
    }
    *canClose = TRUE;
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::Close()
{
    Shutdown();
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::GetAutomationObject(LPCOLESTR, IDispatch** dispatch)
{
    if (dispatch == nullptr)
    {
        return E_POINTER;
    }
    *dispatch = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP Maro_CLive_Maro_Package::CreateTool(REFGUID slot)
{
    if (slot != maro_DiagnosticWindowGuid && slot != maro_SourceWindowGuid && slot != maro_LiveOutputGuid)
    {
        return E_NOTIMPL;
    }
    const bool maro_wasBusy = std::exchange(uiBusy_, true);
    HRESULT maro_result = E_FAIL;
    try
    {
        maro_result = slot == maro_SourceWindowGuid ? maro_EnsureSourceWindow(false) :
            slot == maro_LiveOutputGuid ? maro_EnsureLiveOutput(false) : EnsureDiagnosticWindow(false, false);
    }
    catch (...)
    {
        creatingWindow_ = false;
    }
    uiBusy_ = maro_wasBusy;
    return maro_result;
}

STDMETHODIMP Maro_CLive_Maro_Package::ResetDefaults(PKGRESETFLAGS)
{
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::GetPropertyPage(REFGUID, VSPROPSHEETPAGE* propertyPage)
{
    if (propertyPage == nullptr)
    {
        return E_POINTER;
    }
    return E_NOTIMPL;
}

STDMETHODIMP Maro_CLive_Maro_Package::QueryStatus(
    const GUID* commandGroup,
    ULONG commandCount,
    OLECMD commands[],
    OLECMDTEXT*)
{
    if (commandGroup == nullptr || commands == nullptr)
    {
        return E_POINTER;
    }
    if (*commandGroup != Maro_CLive_Maro_CommandSet)
    {
        return OLECMDERR_E_UNKNOWNGROUP;
    }

    bool handled = false;
    for (ULONG index = 0; index < commandCount; ++index)
    {
        if (commands[index].cmdID == Maro_CLive_Maro_CommandAnalyze ||
            commands[index].cmdID == Maro_CLive_Maro_CommandRun ||
            commands[index].cmdID == Maro_CLive_Maro_CommandUpdate ||
            commands[index].cmdID == maro_CommandDiagnostics)
        {
            commands[index].cmdf = OLECMDF_SUPPORTED | OLECMDF_ENABLED;
            if (commands[index].cmdID == Maro_CLive_Maro_CommandUpdate &&
                (updateRunning_.load() || (maro_pendingCommands_.load() & 4) != 0))
            {
                commands[index].cmdf = OLECMDF_SUPPORTED;
            }
            handled = true;
        }
    }
    return handled ? S_OK : OLECMDERR_E_NOTSUPPORTED;
}

STDMETHODIMP Maro_CLive_Maro_Package::Exec(
    const GUID* commandGroup,
    DWORD commandId,
    DWORD,
    VARIANT*,
    VARIANT*)
{
    if (commandGroup == nullptr)
    {
        return E_POINTER;
    }
    if (*commandGroup != Maro_CLive_Maro_CommandSet)
    {
        return OLECMDERR_E_UNKNOWNGROUP;
    }

    if (commandId < Maro_CLive_Maro_CommandAnalyze || commandId > maro_CommandDiagnostics)
    {
        return OLECMDERR_E_NOTSUPPORTED;
    }
    if (shuttingDown_ || serviceProvider_ == nullptr || uiTimer_ == 0)
    {
        return E_UNEXPECTED;
    }
    if (FAILED(initializationResult_) && initializationResult_ != E_PENDING)
    {
        return initializationResult_;
    }
    maro_pendingCommands_.fetch_or(DWORD{1} << (commandId - Maro_CLive_Maro_CommandAnalyze));
    return S_OK;
}

void STDMETHODCALLTYPE Maro_CLive_Maro_Package::OnChangeLineText(const TextLineChange*, BOOL last)
{
    if (last)
    {
        ++maro_insightVersion_;
        maro_insightHash_ = 0;
        maro_navigation_.reset();
        maro_stopped_ = false;
        maro_CancelLiveWork();
        ScheduleLiveAnalysis();
    }
}

void STDMETHODCALLTYPE Maro_CLive_Maro_Package::OnChangeLineAttributes(long, long)
{
}

STDMETHODIMP Maro_CLive_Maro_Package::OnSelectionChanged(
    IVsHierarchy*,
    VSITEMID,
    IVsMultiItemSelect*,
    ISelectionContainer*,
    IVsHierarchy*,
    VSITEMID,
    IVsMultiItemSelect*,
    ISelectionContainer*)
{
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::OnElementValueChanged(VSSELELEMID element, VARIANT oldValue, VARIANT newValue)
{
    if (element == SEID_DocumentFrame && (oldValue.vt != newValue.vt ||
        (oldValue.vt == VT_UNKNOWN && oldValue.punkVal != newValue.punkVal) ||
        (oldValue.vt == VT_DISPATCH && oldValue.pdispVal != newValue.pdispVal)))
    {
        ++maro_insightVersion_;
        maro_insightHash_ = 0;
        maro_navigation_.reset();
        maro_CancelLiveWork();
        ScheduleLiveAnalysis();
    }
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::OnCmdUIContextChanged(VSCOOKIE, BOOL)
{
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::OnAfterSave(VSCOOKIE)
{
    if (maro_projectMode_)
    {
        maro_stopped_ = false;
        maro_CancelLiveWork();
        ScheduleLiveAnalysis();
    }
    return S_OK;
}

HRESULT Maro_CLive_Maro_Package::EnsureOutputPanes()
{
    if (diagnosticPane_ != nullptr && runPane_ != nullptr)
    {
        return S_OK;
    }
    if (serviceProvider_ == nullptr)
    {
        return E_UNEXPECTED;
    }

    ATL::CComPtr<IVsOutputWindow> outputWindow;
    HRESULT result = serviceProvider_->QueryService(
        SID_SVsOutputWindow,
        IID_IVsOutputWindow,
        reinterpret_cast<void**>(&outputWindow));
    if (FAILED(result) || outputWindow == nullptr)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }

    const auto ensurePane = [&outputWindow](
        const GUID& id,
        const wchar_t* name,
        ATL::CComPtr<IVsOutputWindowPane>& pane) {
        HRESULT paneResult = outputWindow->GetPane(id, &pane);
        if (FAILED(paneResult) || pane == nullptr)
        {
            paneResult = outputWindow->CreatePane(id, name, TRUE, FALSE);
            if (SUCCEEDED(paneResult))
            {
                paneResult = outputWindow->GetPane(id, &pane);
            }
        }
        return pane != nullptr ? paneResult : E_NOINTERFACE;
    };

    result = ensurePane(
        Maro_CLive_Maro_DiagnosticPane,
        L"CLive_Maro 실시간 진단",
        diagnosticPane_);
    if (FAILED(result))
    {
        return result;
    }
    return ensurePane(Maro_CLive_Maro_RunPane, L"CLive_Maro 실행 출력", runPane_);
}

HRESULT Maro_CLive_Maro_Package::StartLiveTracking()
{
    liveInstance_ = this;
    HRESULT result = serviceProvider_->QueryService(
        SID_SVsShellMonitorSelection,
        IID_IVsMonitorSelection,
        reinterpret_cast<void**>(&selectionMonitor_));
    if (SUCCEEDED(result) && selectionMonitor_ != nullptr)
    {
        result = selectionMonitor_->AdviseSelectionEvents(this, &selectionCookie_);
    }
    ScheduleLiveAnalysis();
    return result;
}

HRESULT Maro_CLive_Maro_Package::BindActiveBuffer()
{
    ATL::CComPtr<IVsTextLines> lines;
    HRESULT result = maro_GetDocumentLines(&lines);
    std::wstring path;
    if (FAILED(result) || !maro_ReadSourcePath(lines, path))
    {
        lines.Release();
    }
    if (observedLines_.p == lines.p)
    {
        return S_OK;
    }

    if (observedLines_ != nullptr && textCookie_ != 0)
    {
        ATL::AtlUnadvise(observedLines_, IID_IVsTextLinesEvents, textCookie_);
    }
    observedLines_.Release();
    textCookie_ = 0;
    if (lines == nullptr)
    {
        return S_FALSE;
    }
    observedLines_ = lines;
    result = ATL::AtlAdvise(observedLines_, static_cast<IVsTextLinesEvents*>(this),
        IID_IVsTextLinesEvents, &textCookie_);
    if (FAILED(result))
    {
        observedLines_.Release();
        textCookie_ = 0;
    }
    return result;
}

void Maro_CLive_Maro_Package::ScheduleLiveAnalysis() noexcept
{
    if (liveInstance_ != this || !initialized_ || shuttingDown_)
    {
        return;
    }
    liveDeadline_ = GetTickCount64() + maro_debounceMilliseconds_;
}

void Maro_CLive_Maro_Package::maro_CancelLiveWork() noexcept
{
    runDiagnosticVersion_.store(0, std::memory_order_release);
    const bool maro_hadTrace = maro_trace_ != nullptr;
    if (maro_trace_) maro_trace_->maro_Cancel();
    maro_trace_.reset();
    { std::lock_guard maro_lock(maro_traceMutex_); maro_traceSnapshot_.reset(); }
    if (maro_hadTrace && diagnosticWindow_) diagnosticWindow_->maro_SetTrace({});
    if (maro_hadTrace && maro_liveOutput_) maro_liveOutput_->maro_SetTrace({});
    if (maro_input_) maro_input_->maro_Close();
    maro_input_.reset();
    maro_runningVersion_.store(0, std::memory_order_release);
    runDiagnosticVersion_.store(0, std::memory_order_release);
    diagnosticSourceVersion_.store(0, std::memory_order_release);
    runOutput_.maro_Reset(0);
    diagnosticOutput_.maro_Reset(0);
    if (runEngine_ != nullptr)
    {
        runEngine_->Cancel();
    }
    if (diagnosticEngine_ != nullptr)
    {
        diagnosticEngine_->Cancel();
    }
}

void Maro_CLive_Maro_Package::RunLiveAnalysis() noexcept
{
    try
    {
        BindActiveBuffer();
        Maro_SourceRequest request;
        std::wstring displayPath;
        const HRESULT readResult = ReadActiveSource(request, displayPath);
        if (FAILED(readResult))
        {
            if (readResult == E_ABORT) return;
            if (!lastLivePath_.empty())
            {
                lastLivePath_.clear();
                lastLiveHash_ = 0;
                maro_CancelLiveWork();
                diagnosticOutput_.Clear();
                diagnosticPane_->Clear();
                WriteDiagnostic(L"C/C++ 문서를 열어 주세요.\r\n");
                SetDiagnosticPending({}, L"C/C++ 문서를 열어 주세요.");
            }
            return;
        }
        if (maro_stopped_ || maro_trace_ || !maro_automatic_) return;
        if (maro_projectMode_ && maro_automatic_ && FAILED(maro_ReadProject(request))) return;

        const std::uint64_t hash = Maro_HashSource(request.sourceText + request.maro_projectPath +
            request.maro_configuration + request.maro_platform);
        if (request.sourcePath == lastLivePath_ && hash == lastLiveHash_ &&
            diagnosticSourceVersion_.load(std::memory_order_acquire) != 0)
        {
            return;
        }
        if (FAILED(maro_SubmitSource(std::move(request), displayPath, maro_automatic_, false)))
        {
            WriteDiagnostic(L"분석을 시작하지 못했습니다.\r\n");
        }
    }
    catch (...)
    {
        WriteDiagnostic(L"실시간 분석을 시작하지 못했습니다.\r\n");
    }
}

HRESULT Maro_CLive_Maro_Package::maro_GetDocumentLines(IVsTextLines** maro_lines)
{
    if (maro_lines == nullptr)
    {
        return E_POINTER;
    }
    *maro_lines = nullptr;
    if (serviceProvider_ == nullptr)
    {
        return E_UNEXPECTED;
    }

    if (selectionMonitor_ != nullptr)
    {
        ATL::CComVariant maro_value;
        const HRESULT maro_selected = selectionMonitor_->GetCurrentElementValue(SEID_DocumentFrame, &maro_value);
        if (SUCCEEDED(maro_selected))
        {
            ATL::CComQIPtr<IVsWindowFrame> maro_frame(maro_value.vt == VT_UNKNOWN ? maro_value.punkVal :
                maro_value.vt == VT_DISPATCH ? maro_value.pdispVal : nullptr);
            if (maro_frame == nullptr)
            {
                return E_FAIL;
            }
            ATL::CComVariant maro_data;
            const HRESULT maro_result = maro_frame->GetProperty(VSFPROPID_DocData, &maro_data);
            if (FAILED(maro_result))
            {
                return maro_result;
            }
            ATL::CComQIPtr<IVsTextLines> maro_buffer(maro_data.vt == VT_UNKNOWN ? maro_data.punkVal :
                maro_data.vt == VT_DISPATCH ? maro_data.pdispVal : nullptr);
            return maro_buffer != nullptr ? maro_buffer.CopyTo(maro_lines) : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
    }

    ATL::CComPtr<IVsTextManager> textManager;
    HRESULT result = serviceProvider_->QueryService(
        SID_SVsTextManager,
        IID_IVsTextManager,
        reinterpret_cast<void**>(&textManager));
    if (FAILED(result) || textManager == nullptr)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }

    ATL::CComPtr<IVsTextView> view;
    result = textManager->GetActiveView(FALSE, nullptr, &view);
    if (FAILED(result) || view == nullptr)
    {
        return FAILED(result) ? result : E_FAIL;
    }

    return view->GetBuffer(maro_lines);
}

HRESULT Maro_CLive_Maro_Package::ReadActiveSource(
    Maro_SourceRequest& request,
    std::wstring& displayPath)
{
    ATL::CComPtr<IVsTextLines> lines;
    HRESULT result = maro_GetDocumentLines(&lines);
    if (FAILED(result) || lines == nullptr)
    {
        return FAILED(result) ? result : E_FAIL;
    }

    if (!maro_ReadSourcePath(lines, request.sourcePath))
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    if (request.sourcePath.empty())
    {
        request.sourcePath = L"maro_Active.cpp";
        displayPath = L"저장되지 않은 C++ 문서";
        request.language = Maro_Language::Cpp20;
    }
    else
    {
        displayPath = request.sourcePath;
    }

    long lastLine = 0;
    long lastIndex = 0;
    result = lines->GetLastLineIndex(&lastLine, &lastIndex);
    if (FAILED(result))
    {
        return result;
    }

    long maro_length = 0;
    if (FAILED(lines->GetSize(&maro_length)) || maro_length < 0 || maro_length > 8 * 1024 * 1024)
    {
        SetDiagnosticPending(request.sourcePath, L"이 파일은 미리보기 크기 제한(UTF-16 8M)을 초과합니다. 작은 소스 문서에서 프로젝트 모드를 사용하세요.");
        return E_ABORT;
    }
    ATL::CComBSTR source;
    result = lines->GetLineText(0, 0, lastLine, lastIndex, &source);
    if (FAILED(result))
    {
        return result;
    }

    request.sourceText.assign(source.m_str != nullptr ? source.m_str : L"", source.Length());
    if (!request.sourcePath.empty())
    {
        const std::optional<Maro_Language> language =
            Maro_InferVisualStudioLanguage(request.sourcePath, {}, request.sourceText);
        if (!language)
        {
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        request.language = *language;
    }
    request.sourceVersion = ++sourceVersion_;
    request.mode = Maro_SourceMode::Program;
    maro_RequestInsight(request);
    return S_OK;
}

bool Maro_CLive_Maro_Package::maro_HasDirtyDocuments()
{
    if (!maro_documents_) return true;
    ATL::CComPtr<IEnumRunningDocuments> maro_enumerator;
    if (FAILED(maro_documents_->GetRunningDocumentsEnum(&maro_enumerator)) || !maro_enumerator) return true;
    for (unsigned maro_count = 0; maro_count < 512; ++maro_count)
    {
        VSCOOKIE maro_cookie = 0;
        ULONG maro_fetched = 0;
        if (maro_enumerator->Next(1, &maro_cookie, &maro_fetched) != S_OK || maro_fetched == 0) return false;
        VSRDTFLAGS maro_flags = 0;
        DWORD maro_reads = 0, maro_edits = 0;
        ATL::CComBSTR maro_path;
        ATL::CComPtr<IVsHierarchy> maro_hierarchy;
        ATL::CComPtr<IUnknown> maro_data;
        VSITEMID maro_item = VSITEMID_NIL;
        if (FAILED(maro_documents_->GetDocumentInfo(maro_cookie, &maro_flags, &maro_reads, &maro_edits,
            &maro_path, &maro_hierarchy, &maro_item, &maro_data))) return true;
        ATL::CComQIPtr<IVsPersistDocData> maro_persist(maro_data);
        if (maro_persist)
        {
            BOOL maro_dirty = FALSE;
            if (FAILED(maro_persist->IsDocDataDirty(&maro_dirty)) || maro_dirty) return true;
        }
    }
    return true;
}

HRESULT Maro_CLive_Maro_Package::maro_ReadProject(Maro_SourceRequest& maro_request)
{
    auto maro_fail = [this](const wchar_t* maro_text) {
        maro_CancelLiveWork();
        lastLivePath_.clear();
        SetDiagnosticPending({}, maro_text);
        WriteDiagnostic(maro_text);
        return E_ABORT;
    };
    if (!selectionMonitor_) return maro_fail(L"C/C++ 프로젝트의 소스 파일을 열어 주세요.");
    ATL::CComVariant maro_frameValue;
    selectionMonitor_->GetCurrentElementValue(SEID_DocumentFrame, &maro_frameValue);
    ATL::CComQIPtr<IVsWindowFrame> maro_frame(maro_frameValue.vt == VT_UNKNOWN ? maro_frameValue.punkVal :
        maro_frameValue.vt == VT_DISPATCH ? maro_frameValue.pdispVal : nullptr);
    if (!maro_frame) return maro_fail(L"프로젝트 문서를 선택해 주세요.");
    ATL::CComVariant maro_hierarchyValue;
    maro_frame->GetProperty(VSFPROPID_Hierarchy, &maro_hierarchyValue);
    ATL::CComQIPtr<IVsHierarchy> maro_hierarchy(maro_hierarchyValue.vt == VT_UNKNOWN ? maro_hierarchyValue.punkVal :
        maro_hierarchyValue.vt == VT_DISPATCH ? maro_hierarchyValue.pdispVal : nullptr);
    ATL::CComQIPtr<IVsProject> maro_project(maro_hierarchy);
    ATL::CComBSTR maro_projectPath;
    if (!maro_project || FAILED(maro_project->GetMkDocument(VSITEMID_ROOT, &maro_projectPath)) ||
        !maro_projectPath || _wcsicmp(std::filesystem::path(maro_projectPath.m_str).extension().c_str(), L".vcxproj") != 0)
        return maro_fail(L"프로젝트 모드는 .vcxproj가 필요합니다. 단일 파일은 '파일'로 전환하세요.");
    ATL::CComPtr<IVsSolutionBuildManager> maro_build;
    serviceProvider_->QueryService(SID_SVsSolutionBuildManager, IID_IVsSolutionBuildManager,
        reinterpret_cast<void**>(&maro_build));
    ATL::CComPtr<IVsProjectCfg> maro_config;
    ATL::CComBSTR maro_display;
    if (!maro_build || FAILED(maro_build->FindActiveProjectCfg(nullptr, nullptr, maro_hierarchy, &maro_config)) ||
        !maro_config || FAILED(maro_config->get_DisplayName(&maro_display)) || !maro_display)
        return maro_fail(L"Visual Studio의 프로젝트 구성과 플랫폼을 확인해 주세요.");
    BOOL maro_busy = FALSE;
    if (FAILED(maro_build->QueryBuildManagerBusy(&maro_busy)) || maro_busy)
        return maro_fail(L"Visual Studio 빌드가 끝난 뒤 다시 실행해 주세요.");
    const std::wstring maro_name(maro_display.m_str);
    const auto maro_separator = maro_name.find(L'|');
    if (maro_separator == std::wstring::npos) return maro_fail(L"프로젝트 플랫폼을 확인하지 못했습니다.");
    if (maro_HasDirtyDocuments()) return maro_fail(L"프로젝트 실행: 수정한 파일을 모두 저장하세요 (Ctrl+Shift+S).");
    maro_request.maro_projectPath = maro_projectPath.m_str;
    maro_request.maro_configuration = maro_name.substr(0, maro_separator);
    maro_request.maro_platform = maro_name.substr(maro_separator + 1);
    wchar_t maro_executable[32768]{};
    if (GetModuleFileNameW(nullptr, maro_executable, 32768))
    {
        const auto maro_install = std::filesystem::path(maro_executable).parent_path().parent_path().parent_path();
        maro_request.maro_msbuildPath = (maro_install / L"MSBuild" / L"Current" / L"Bin" / L"MSBuild.exe").wstring();
    }
    ATL::CComPtr<IVsSolution> maro_solution;
    serviceProvider_->QueryService(SID_SVsSolution, IID_IVsSolution, reinterpret_cast<void**>(&maro_solution));
    if (maro_solution)
    {
        ATL::CComBSTR maro_directory, maro_path, maro_options;
        if (SUCCEEDED(maro_solution->GetSolutionInfo(&maro_directory, &maro_path, &maro_options)) && maro_path)
            maro_request.maro_solutionPath = maro_path.m_str;
    }
    return S_OK;
}

HRESULT Maro_CLive_Maro_Package::StartAnalysis(bool execute)
{
    maro_stopped_ = false;
    try
    {
        Maro_Engine* engine = execute ? runEngine_.get() : diagnosticEngine_.get();
        if (engine == nullptr)
        {
            return E_UNEXPECTED;
        }

        BindActiveBuffer();
        Maro_SourceRequest request;
        std::wstring displayPath;
        const HRESULT readResult = ReadActiveSource(request, displayPath);
        if (FAILED(readResult))
        {
            if (readResult == E_ABORT) return S_OK;
            EnsureDiagnosticWindow(false);
            SetDiagnosticPending({}, L"C/C++ 문서를 열어 주세요.");
            diagnosticOutput_.Clear();
            diagnosticPane_->Clear();
            WriteDiagnostic(readResult == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
                ? L"C/C++ 문서를 열어 주세요.\r\n"
                : L"활성 편집기 내용을 읽지 못했습니다.\r\n");
            return S_OK;
        }

        if (maro_projectMode_ && execute && FAILED(maro_ReadProject(request))) return S_OK;
        return maro_SubmitSource(std::move(request), displayPath, execute, true);
    }
    catch (const std::bad_alloc&)
    {
        WriteDiagnostic(L"메모리가 부족합니다.\r\n");
        return E_OUTOFMEMORY;
    }
    catch (...)
    {
        WriteDiagnostic(L"작업을 시작하지 못했습니다.\r\n");
        return E_FAIL;
    }
}

HRESULT Maro_CLive_Maro_Package::maro_SubmitSource(
    Maro_SourceRequest request, const std::wstring& path, bool execute, bool show)
{
    maro_CancelLiveWork();
    maro_clearOutputPending_ = false;
    liveDeadline_ = 0;
    lastLivePath_ = request.sourcePath;
    lastLiveHash_ = Maro_HashSource(request.sourceText + request.maro_projectPath +
        request.maro_configuration + request.maro_platform);
    request.execute = execute;
    request.maro_background = !show;
    request.maro_outputCodePage = maro_codePage_;
    if (request.maro_trace) maro_trace_ = request.maro_trace;
    SetDiagnosticPending(path, L"검사 중...");
    diagnosticOutput_.maro_Reset(request.sourceVersion);
    diagnosticPane_->Clear();
    runPane_->Clear();
    maro_outputPaneSize_ = 0;
    maro_diagnosticPaneSize_ = 0;
    if (show)
    {
        EnsureDiagnosticWindow(false);
    }
    if (diagnosticWindow_ != nullptr)
    {
        diagnosticWindow_->maro_ClearOutput();
    }
    if (maro_liveOutput_) maro_liveOutput_->maro_ClearOutput();
    WriteDiagnostic(L"CLive_Maro 실시간 진단 | " + path + L"\r\n검사 중...\r\n");
    if (execute)
    {
        maro_input_ = std::make_shared<maro_ProcessInput>();
        request.maro_input = maro_input_;
        runDiagnosticVersion_.store(request.sourceVersion, std::memory_order_release);
        runOutput_.maro_Reset(request.sourceVersion);
    }
    Maro_Engine* engine = execute ? runEngine_.get() : diagnosticEngine_.get();
    if (engine == nullptr || engine->Submit(std::move(request)) == 0)
    {
        WriteDiagnostic(L"작업을 시작하지 못했습니다.\r\n");
        return E_FAIL;
    }
    return S_OK;
}

HRESULT Maro_CLive_Maro_Package::StartUpdate()
{
    if (updateRunning_.exchange(true))
    {
        return S_OK;
    }
    try
    {
        if (updateThread_.joinable())
        {
            updateThread_.join();
        }
        updateCancelled_.store(false);
        EnsureDiagnosticWindow(false);
        updateNotice_.Push(L"업데이트 확인 중...");
        WriteDiagnostic(L"\r\n업데이트 확인 중...\r\n");
        updateThread_ = std::thread([this] { RunUpdate(); });
        return S_OK;
    }
    catch (...)
    {
        updateRunning_.store(false);
        WriteDiagnostic(L"업데이트를 시작하지 못했습니다.\r\n");
        return E_FAIL;
    }
}

void Maro_CLive_Maro_Package::QueueUpdateMessage(std::wstring text)
{
    WriteDiagnostic(text);
    updateNotice_.Push(text);
}

void Maro_CLive_Maro_Package::RunUpdate() noexcept
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try
    {
        const auto check = maro_CheckForUpdate({2, 1, 0}, &updateCancelled_);
        if (check.status == Maro_UpdateCheckStatus::Current)
        {
            QueueUpdateMessage(L"최신 버전입니다. (v2.1.0)\r\n");
        }
        else if (check.status == Maro_UpdateCheckStatus::Failed)
        {
            QueueUpdateMessage(check.error + L"\r\n");
        }
        else if (check.status == Maro_UpdateCheckStatus::Available)
        {
            QueueUpdateMessage(Maro_Utf8ToWide(check.release.tag) + L" 다운로드 중...\r\n");
            const auto install = maro_DownloadAndLaunchUpdate(check.release, &updateCancelled_);
            if (install.status == Maro_UpdateInstallStatus::Launched)
            {
                QueueUpdateMessage(L"설치를 시작했습니다. 작업을 저장하고 Visual Studio를 닫아 업데이트를 완료하세요.\r\n");
            }
            else if (install.status == Maro_UpdateInstallStatus::Failed)
            {
                QueueUpdateMessage(install.error + L"\r\n");
            }
        }
    }
    catch (...)
    {
        try { QueueUpdateMessage(L"업데이트 중 오류가 발생했습니다.\r\n"); } catch (...) {}
    }
    if (SUCCEEDED(initialized))
    {
        CoUninitialize();
    }
    updateRunning_.store(false);
}

void Maro_CLive_Maro_Package::PublishDiagnosticResult(const Maro_ResultEnvelope& result) noexcept
{
    try
    {
        const bool maro_analyzed = result.phase == Maro_Phase::Running;
        if ((!Maro_IsCompleted(result) && !maro_analyzed) ||
            result.sourceVersion != diagnosticSourceVersion_.load(std::memory_order_acquire))
        {
            return;
        }

        if (result.sourceVersion == diagnosticSourceVersion_.load(std::memory_order_acquire))
        {
            Maro_ResultEnvelope snapshot;
            snapshot.sourceVersion = result.sourceVersion;
            snapshot.status = maro_analyzed ? Maro_Status::Success : result.status;
            snapshot.statusText = maro_analyzed ? L"검사 완료" : result.statusText;
            snapshot.diagnostics = maro_GroupDiagnostics(result.diagnostics);
            snapshot.maro_compileMilliseconds = result.maro_compileMilliseconds;
            snapshot.maro_runMilliseconds = result.maro_runMilliseconds;
            snapshot.hasExitCode = result.hasExitCode;
            snapshot.exitCode = result.exitCode;
            std::lock_guard lock(diagnosticMutex_);
            if (result.sourceVersion == diagnosticSourceVersion_.load(std::memory_order_acquire))
            {
                pendingDiagnostic_ = std::move(snapshot);
            }
        }

        std::wostringstream text;
        text << L"\r\n" << (maro_analyzed ? L"검사 완료" : result.statusText) << L"\r\n";
        if (result.diagnostics.empty() && result.status == Maro_Status::Success)
        {
            text << L"문제 없음.\r\n";
        }
        for (const Maro_Diagnostic& diagnostic : result.diagnostics)
        {
            text << (diagnostic.range.start.line == 0 ? 1 : diagnostic.range.start.line);
            if (diagnostic.range.start.column != 0)
            {
                text << L":" << diagnostic.range.start.column;
            }
            text << L" " << Maro_SeverityText(diagnostic.severity);
            if (!diagnostic.code.empty())
            {
                text << L" " << diagnostic.code;
            }
            text << L" | " << diagnostic.friendlyMessage << L"\r\n";
        }

        if (result.diagnostics.empty() && !result.compilerOutput.empty() &&
            result.status != Maro_Status::Success)
        {
            text << L"\r\n" << result.compilerOutput;
        }
        diagnosticOutput_.maro_PushVersion(result.sourceVersion, text.str());
    }
    catch (...)
    {
        diagnosticOutput_.maro_PushVersion(result.sourceVersion, L"진단 표시 중 오류가 발생했습니다.\r\n");
    }
}

void Maro_CLive_Maro_Package::PublishRunResult(const Maro_ResultEnvelope& result) noexcept
{
    try
    {
        if (result.sourceVersion != runDiagnosticVersion_.load(std::memory_order_acquire))
        {
            return;
        }
        if (result.phase == Maro_Phase::Running)
        {
            maro_runningVersion_.store(result.sourceVersion, std::memory_order_release);
            if (result.standardOutput.empty() && result.standardError.empty()) PublishDiagnosticResult(result);
        }
        if (!Maro_IsCompleted(result) && !result.compilerOutput.empty() && result.phase == Maro_Phase::Analyzing)
        {
            diagnosticOutput_.maro_PushVersion(result.sourceVersion, result.compilerOutput);
            Maro_ResultEnvelope maro_progress;
            maro_progress.sourceVersion = result.sourceVersion;
            maro_progress.statusText = L"프로젝트 빌드 중...\r\n" +
                result.compilerOutput.substr(result.compilerOutput.size() > 2048 ? result.compilerOutput.size() - 2048 : 0);
            std::lock_guard maro_lock(diagnosticMutex_);
            if (result.sourceVersion == diagnosticSourceVersion_.load()) pendingDiagnostic_ = std::move(maro_progress);
        }
        if (!Maro_IsCompleted(result))
        {
            if (!result.standardOutput.empty())
            {
                runOutput_.maro_PushVersion(result.sourceVersion, result.standardOutput);
            }
            if (!result.standardError.empty())
            {
                runOutput_.maro_PushVersion(result.sourceVersion, L"[stderr] " + result.standardError);
            }
            return;
        }
        if (runDiagnosticVersion_.load(std::memory_order_acquire) == result.sourceVersion)
        {
            maro_finishedVersion_.store(result.sourceVersion, std::memory_order_release);
            PublishDiagnosticResult(result);
        }
    }
    catch (...)
    {
        runOutput_.maro_PushVersion(result.sourceVersion, L"실행 출력 표시 중 오류가 발생했습니다.\r\n");
    }
}

void Maro_CLive_Maro_Package::WriteDiagnostic(std::wstring_view text) noexcept
{
    diagnosticOutput_.Push(text);
}

void Maro_CLive_Maro_Package::WriteRun(std::wstring_view text) noexcept
{
    runOutput_.Push(text);
}

void Maro_CLive_Maro_Package::Shutdown() noexcept
{
    if (maro_trace_) maro_trace_->maro_Cancel();
    maro_trace_.reset();
    maro_insightWorker_.reset();
    if (maro_input_) maro_input_->maro_Close();
    maro_input_.reset();
    shuttingDown_ = true;
    initialized_ = false;
    if (uiTimer_ != 0)
    {
        KillTimer(nullptr, uiTimer_);
        uiTimer_ = 0;
    }
    liveDeadline_ = 0;
    maro_pendingCommands_.store(0);
    updateCancelled_.store(true, std::memory_order_release);
    if (updateThread_.joinable())
    {
        updateThread_.join();
    }
    updateRunning_.store(false, std::memory_order_release);
    if (liveInstance_ == this)
    {
        liveInstance_ = nullptr;
    }
    runDiagnosticVersion_.store(0, std::memory_order_release);
    if (observedLines_ != nullptr && textCookie_ != 0)
    {
        ATL::AtlUnadvise(observedLines_, IID_IVsTextLinesEvents, textCookie_);
    }
    textCookie_ = 0;
    observedLines_.Release();
    if (maro_documents_ != nullptr && maro_documentCookie_ != 0)
    {
        maro_documents_->UnadviseRunningDocTableEvents(maro_documentCookie_);
    }
    maro_documentCookie_ = 0;
    maro_documents_.Release();
    if (selectionMonitor_ != nullptr && selectionCookie_ != 0)
    {
        selectionMonitor_->UnadviseSelectionEvents(selectionCookie_);
    }
    selectionCookie_ = 0;
    selectionMonitor_.Release();
    if (diagnosticEngine_ != nullptr)
    {
        diagnosticEngine_->Shutdown();
        diagnosticEngine_.reset();
    }
    if (runEngine_ != nullptr)
    {
        runEngine_->Shutdown();
        runEngine_.reset();
    }
    diagnosticOutput_.Clear();
    runOutput_.Clear();
    updateNotice_.Clear();
    {
        std::lock_guard lock(diagnosticMutex_);
        pendingDiagnostic_.reset();
    }
    if (diagnosticWindow_ != nullptr)
    {
        diagnosticWindow_->ClosePane();
    }
    diagnosticFrame_.Release();
    diagnosticWindow_.Release();
    if (maro_liveOutput_) maro_liveOutput_->ClosePane();
    maro_liveOutputFrame_.Release();
    maro_liveOutput_.Release();
    if (maro_sourceWindow_) maro_sourceWindow_->ClosePane();
    maro_sourceFrame_.Release();
    maro_sourceWindow_.Release();
    diagnosticPane_.Release();
    runPane_.Release();
    serviceProvider_.Release();
}
