#include "maro_CLive_Maro_Package.hpp"
#include "maro_Text.hpp"
#include "maro_VisualStudio.hpp"
#include "maro_Update.hpp"

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
            initialized_ = true;
            StartLiveTracking();
            EnsureDiagnosticWindow(false);
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
            if (liveDeadline_ != 0 && GetTickCount64() >= liveDeadline_)
            {
                liveDeadline_ = 0;
                RunLiveAnalysis();
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
            }
            auto notice = updateNotice_.Take(16 * 1024);
            if (diagnosticWindow_ != nullptr && !notice.empty())
            {
                diagnosticWindow_->maro_SetNotice(std::move(notice));
            }
            const auto diagnostic = diagnosticOutput_.Take(32 * 1024);
            const auto output = runOutput_.Take(32 * 1024);
            if (!shuttingDown_)
            {
                maro_WritePane(diagnosticPane_, diagnostic);
            }
            if (!shuttingDown_)
            {
                maro_WritePane(runPane_, output);
            }
        }
    }
    catch (...)
    {
    }
    uiBusy_ = false;
}

HRESULT Maro_CLive_Maro_Package::EnsureDiagnosticWindow(bool activate)
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
                BOOL defaultPosition = FALSE;
                result = shell->CreateToolWindow(CTW_fInitNew, 0,
                    static_cast<IVsWindowPane*>(pane), GUID_NULL, maro_DiagnosticWindowGuid,
                    GUID_NULL, serviceProvider_, L"CLive_Maro", &defaultPosition, &diagnosticFrame_);
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
    return activate ? diagnosticFrame_->Show() : diagnosticFrame_->ShowNoActivate();
}

void Maro_CLive_Maro_Package::SetDiagnosticPending(const std::wstring& path, const wchar_t* status)
{
    diagnosticSourceVersion_.store(path.empty() ? 0 : sourceVersion_, std::memory_order_release);
    if (diagnosticWindow_ != nullptr)
    {
        diagnosticWindow_->maro_SetPending(path, status);
    }
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
    return slot == maro_DiagnosticWindowGuid ? EnsureDiagnosticWindow(false) : E_NOTIMPL;
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
            if (commands[index].cmdID == Maro_CLive_Maro_CommandUpdate && updateRunning_.load())
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

    const HRESULT initialized = InitializeUi();
    if (FAILED(initialized))
    {
        return initialized;
    }
    if (commandId == Maro_CLive_Maro_CommandAnalyze)
    {
        return StartAnalysis(false);
    }
    if (commandId == Maro_CLive_Maro_CommandRun)
    {
        return StartAnalysis(true);
    }
    if (commandId == Maro_CLive_Maro_CommandUpdate)
    {
        return StartUpdate();
    }
    if (commandId == maro_CommandDiagnostics)
    {
        return EnsureDiagnosticWindow(true);
    }
    return OLECMDERR_E_NOTSUPPORTED;
}

void STDMETHODCALLTYPE Maro_CLive_Maro_Package::OnChangeLineText(const TextLineChange*, BOOL last)
{
    if (last)
    {
        runDiagnosticVersion_.store(0, std::memory_order_release);
        diagnosticSourceVersion_.store(0, std::memory_order_release);
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
    ScheduleLiveAnalysis();
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::OnElementValueChanged(VSSELELEMID, VARIANT, VARIANT)
{
    ScheduleLiveAnalysis();
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::OnCmdUIContextChanged(VSCOOKIE, BOOL)
{
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
    if (serviceProvider_ == nullptr)
    {
        return E_UNEXPECTED;
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
    ATL::CComPtr<IVsTextLines> lines;
    if (SUCCEEDED(result) && view != nullptr)
    {
        result = view->GetBuffer(&lines);
    }
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
    liveDeadline_ = GetTickCount64() + 650;
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
            if (!lastLivePath_.empty())
            {
                lastLivePath_.clear();
                lastLiveHash_ = 0;
                runDiagnosticVersion_.store(0, std::memory_order_release);
                diagnosticEngine_->Cancel();
                diagnosticOutput_.Clear();
                diagnosticPane_->Clear();
                WriteDiagnostic(L"C/C++ 문서를 열어 주세요.\r\n");
                SetDiagnosticPending({}, L"C/C++ 문서를 열어 주세요.");
            }
            return;
        }

        const std::uint64_t hash = Maro_HashSource(request.sourceText);
        if (request.sourcePath == lastLivePath_ && hash == lastLiveHash_ &&
            diagnosticSourceVersion_.load(std::memory_order_acquire) != 0)
        {
            return;
        }
        lastLivePath_ = request.sourcePath;
        lastLiveHash_ = hash;
        runDiagnosticVersion_.store(0, std::memory_order_release);
        request.execute = false;
        SetDiagnosticPending(displayPath, L"검사 중...");
        diagnosticOutput_.Clear();
        diagnosticPane_->Clear();
        WriteDiagnostic(L"CLive_Maro 실시간 진단 | " + displayPath + L"\r\n검사 중...\r\n");
        if (diagnosticEngine_->Submit(std::move(request)) == 0)
        {
            WriteDiagnostic(L"분석을 시작하지 못했습니다.\r\n");
        }
    }
    catch (...)
    {
        WriteDiagnostic(L"실시간 분석을 시작하지 못했습니다.\r\n");
    }
}

HRESULT Maro_CLive_Maro_Package::ReadActiveSource(
    Maro_SourceRequest& request,
    std::wstring& displayPath)
{
    if (serviceProvider_ == nullptr)
    {
        return E_UNEXPECTED;
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

    ATL::CComPtr<IVsTextLines> lines;
    result = view->GetBuffer(&lines);
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
    return S_OK;
}

HRESULT Maro_CLive_Maro_Package::StartAnalysis(bool execute)
{
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
            EnsureDiagnosticWindow(true);
            SetDiagnosticPending({}, L"C/C++ 문서를 열어 주세요.");
            diagnosticOutput_.Clear();
            diagnosticPane_->Clear();
            WriteDiagnostic(readResult == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
                ? L"C/C++ 문서를 열어 주세요.\r\n"
                : L"활성 편집기 내용을 읽지 못했습니다.\r\n");
            return S_OK;
        }

        request.execute = execute;
        SetDiagnosticPending(displayPath, execute ? L"분석 및 실행 중..." : L"검사 중...");
        if (execute)
        {
            diagnosticEngine_->Cancel();
            lastLivePath_ = request.sourcePath;
            lastLiveHash_ = Maro_HashSource(request.sourceText);
            runDiagnosticVersion_.store(request.sourceVersion, std::memory_order_release);
            diagnosticOutput_.Clear();
            runOutput_.Clear();
            diagnosticPane_->Clear();
            runPane_->Clear();
            runPane_->Activate();
            WriteDiagnostic(L"CLive_Maro 실시간 진단 | " + displayPath + L"\r\n");
            WriteRun(L"CLive_Maro 실행 출력 | " + displayPath + L"\r\n\r\n");
        }
        else
        {
            runDiagnosticVersion_.store(0, std::memory_order_release);
            lastLivePath_ = request.sourcePath;
            lastLiveHash_ = Maro_HashSource(request.sourceText);
            diagnosticOutput_.Clear();
            diagnosticPane_->Clear();
            EnsureDiagnosticWindow(true);
            WriteDiagnostic(L"CLive_Maro 실시간 진단 | " + displayPath + L"\r\n검사 중...\r\n");
        }

        if (engine->Submit(std::move(request)) == 0)
        {
            if (execute)
            {
                WriteRun(L"작업을 시작하지 못했습니다.\r\n");
            }
            else
            {
                WriteDiagnostic(L"작업을 시작하지 못했습니다.\r\n");
            }
        }
        return S_OK;
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
        EnsureDiagnosticWindow(true);
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
        const auto check = maro_CheckForUpdate({1, 2, 3}, &updateCancelled_);
        if (check.status == Maro_UpdateCheckStatus::Current)
        {
            QueueUpdateMessage(L"최신 버전입니다. (v1.2.3)\r\n");
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
        if (!Maro_IsCompleted(result))
        {
            return;
        }

        if (result.sourceVersion == diagnosticSourceVersion_.load(std::memory_order_acquire))
        {
            Maro_ResultEnvelope snapshot;
            snapshot.sourceVersion = result.sourceVersion;
            snapshot.status = result.status;
            snapshot.statusText = result.statusText;
            snapshot.diagnostics = result.diagnostics;
            std::lock_guard lock(diagnosticMutex_);
            if (result.sourceVersion == diagnosticSourceVersion_.load(std::memory_order_acquire))
            {
                pendingDiagnostic_ = std::move(snapshot);
            }
        }

        std::wostringstream text;
        text << L"\r\n" << result.statusText << L"\r\n";
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
        WriteDiagnostic(text.str());
    }
    catch (...)
    {
        WriteDiagnostic(L"진단 표시 중 오류가 발생했습니다.\r\n");
    }
}

void Maro_CLive_Maro_Package::PublishRunResult(const Maro_ResultEnvelope& result) noexcept
{
    try
    {
        if (!Maro_IsCompleted(result))
        {
            if (!result.standardOutput.empty())
            {
                WriteRun(result.standardOutput);
            }
            if (!result.standardError.empty())
            {
                WriteRun(L"[stderr] " + result.standardError);
            }
            if (result.phase == Maro_Phase::Running && result.standardOutput.empty() &&
                result.standardError.empty() && !result.statusText.empty())
            {
                WriteRun(L"> " + result.statusText + L"\r\n");
            }
            return;
        }
        if (runDiagnosticVersion_.load(std::memory_order_acquire) == result.sourceVersion)
        {
            PublishDiagnosticResult(result);
        }
        WriteRun(L"\r\n" + result.statusText + L"\r\n");
    }
    catch (...)
    {
        WriteRun(L"실행 출력 표시 중 오류가 발생했습니다.\r\n");
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
    shuttingDown_ = true;
    initialized_ = false;
    if (uiTimer_ != 0)
    {
        KillTimer(nullptr, uiTimer_);
        uiTimer_ = 0;
    }
    liveDeadline_ = 0;
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
    diagnosticPane_.Release();
    runPane_.Release();
    serviceProvider_.Release();
}
