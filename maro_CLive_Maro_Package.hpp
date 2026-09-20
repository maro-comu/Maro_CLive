#pragma once

#include "maro_Engine.hpp"
#include "maro_OutputQueue.hpp"
#include "maro_DiagnosticWindow.hpp"
#include "maro_SourceWindow.hpp"
#include "maro_InsightWorker.hpp"
#include "maro_Trace.hpp"

#include <atlbase.h>
#include <atlcom.h>
#include <fpstfmt.h>
#include <textmgr.h>
#include <vsshell.h>
#include <vsshell140.h>

#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

extern const CLSID Maro_CLive_Maro_PackageClsid;
extern const GUID Maro_CLive_Maro_CommandSet;
extern const GUID Maro_CLive_Maro_DiagnosticPane;
extern const GUID Maro_CLive_Maro_RunPane;

constexpr DWORD Maro_CLive_Maro_CommandAnalyze = 0x0100;
constexpr DWORD Maro_CLive_Maro_CommandRun = 0x0101;
constexpr DWORD Maro_CLive_Maro_CommandUpdate = 0x0102;
constexpr DWORD maro_CommandDiagnostics = 0x0103;

class ATL_NO_VTABLE Maro_CLive_Maro_Package
    : public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>,
      public ATL::CComCoClass<Maro_CLive_Maro_Package, &Maro_CLive_Maro_PackageClsid>,
      public IVsPackage,
      public IAsyncLoadablePackageInitialize,
      public IOleCommandTarget,
      public IVsTextLinesEvents,
      public IVsSelectionEvents,
      public IVsRunningDocTableEvents
{
public:
    Maro_CLive_Maro_Package() = default;
    ~Maro_CLive_Maro_Package() { Shutdown(); }

    DECLARE_NO_REGISTRY()
    DECLARE_NOT_AGGREGATABLE(Maro_CLive_Maro_Package)

    BEGIN_COM_MAP(Maro_CLive_Maro_Package)
        COM_INTERFACE_ENTRY(IVsPackage)
        COM_INTERFACE_ENTRY(IAsyncLoadablePackageInitialize)
        COM_INTERFACE_ENTRY(IOleCommandTarget)
        COM_INTERFACE_ENTRY(IVsTextLinesEvents)
        COM_INTERFACE_ENTRY(IVsSelectionEvents)
        COM_INTERFACE_ENTRY(IVsRunningDocTableEvents)
    END_COM_MAP()

    STDMETHOD(SetSite)(IServiceProvider* serviceProvider) override;
    STDMETHOD(Initialize)(
        IAsyncServiceProvider* serviceProvider,
        IProfferAsyncService* profferService,
        IAsyncProgressCallback* progress,
        IVsTask** task) override;
    STDMETHOD(QueryClose)(BOOL* canClose) override;
    STDMETHOD(Close)() override;
    STDMETHOD(GetAutomationObject)(LPCOLESTR name, IDispatch** dispatch) override;
    STDMETHOD(CreateTool)(REFGUID persistenceSlot) override;
    STDMETHOD(ResetDefaults)(PKGRESETFLAGS flags) override;
    STDMETHOD(GetPropertyPage)(REFGUID page, VSPROPSHEETPAGE* propertyPage) override;

    STDMETHOD(QueryStatus)(
        const GUID* commandGroup,
        ULONG commandCount,
        OLECMD commands[],
        OLECMDTEXT* commandText) override;
    STDMETHOD(Exec)(
        const GUID* commandGroup,
        DWORD commandId,
        DWORD executeOptions,
        VARIANT* input,
        VARIANT* output) override;

    void STDMETHODCALLTYPE OnChangeLineText(const TextLineChange* change, BOOL last) override;
    void STDMETHODCALLTYPE OnChangeLineAttributes(long firstLine, long lastLine) override;
    STDMETHOD(OnSelectionChanged)(
        IVsHierarchy* oldHierarchy,
        VSITEMID oldItem,
        IVsMultiItemSelect* oldSelection,
        ISelectionContainer* oldContainer,
        IVsHierarchy* newHierarchy,
        VSITEMID newItem,
        IVsMultiItemSelect* newSelection,
        ISelectionContainer* newContainer) override;
    STDMETHOD(OnElementValueChanged)(VSSELELEMID element, VARIANT oldValue, VARIANT newValue) override;
    STDMETHOD(OnCmdUIContextChanged)(VSCOOKIE cookie, BOOL active) override;
    STDMETHOD(OnAfterFirstDocumentLock)(VSCOOKIE, VSRDTFLAGS, DWORD, DWORD) override { return S_OK; }
    STDMETHOD(OnBeforeLastDocumentUnlock)(VSCOOKIE, VSRDTFLAGS, DWORD, DWORD) override { return S_OK; }
    STDMETHOD(OnAfterSave)(VSCOOKIE maro_cookie) override;
    STDMETHOD(OnAfterAttributeChange)(VSCOOKIE, VSRDTATTRIB) override { return S_OK; }
    STDMETHOD(OnBeforeDocumentWindowShow)(VSCOOKIE, BOOL, IVsWindowFrame*) override { return S_OK; }
    STDMETHOD(OnAfterDocumentWindowHide)(VSCOOKIE, IVsWindowFrame*) override { return S_OK; }

private:
    HRESULT InitializeUi();
    HRESULT EnsureDiagnosticWindow(bool activate, bool show = true);
    HRESULT maro_EnsureLiveOutput(bool maro_show = true);
    void maro_ConfigurePane(maro_DiagnosticWindow* maro_pane, bool maro_output);
    HRESULT maro_EnsureSourceWindow(bool maro_show = true);
    void maro_Navigate(const maro_SourceItem& maro_item);
    void maro_ApplyFix(const Maro_Diagnostic& maro_diagnostic);
    void maro_NavigateDiagnostic(const Maro_Diagnostic& maro_diagnostic);
    void maro_RequestInsight(const Maro_SourceRequest& maro_request);
    HRESULT maro_StartTrace();
    void SetDiagnosticPending(const std::wstring& path, const wchar_t* status);
    void ProcessUi() noexcept;
    static void CALLBACK UiTimerProc(HWND window, UINT message, UINT_PTR timer, DWORD time) noexcept;
    HRESULT EnsureOutputPanes();
    HRESULT StartLiveTracking();
    HRESULT BindActiveBuffer();
    HRESULT maro_GetDocumentLines(IVsTextLines** maro_lines);
    HRESULT ReadActiveSource(Maro_SourceRequest& request, std::wstring& displayPath);
    HRESULT maro_ReadProject(Maro_SourceRequest& maro_request);
    bool maro_HasDirtyDocuments();
    HRESULT StartAnalysis(bool execute);
    HRESULT maro_SubmitSource(Maro_SourceRequest request, const std::wstring& path, bool execute, bool show);
    void maro_CancelLiveWork() noexcept;
    HRESULT StartUpdate();
    void RunUpdate() noexcept;
    void QueueUpdateMessage(std::wstring text);
    void ScheduleLiveAnalysis() noexcept;
    void RunLiveAnalysis() noexcept;
    void PublishDiagnosticResult(const Maro_ResultEnvelope& result) noexcept;
    void PublishRunResult(const Maro_ResultEnvelope& result) noexcept;
    void WriteDiagnostic(std::wstring_view text) noexcept;
    void WriteRun(std::wstring_view text) noexcept;
    void Shutdown() noexcept;

    ATL::CComPtr<IServiceProvider> serviceProvider_;
    ATL::CComPtr<IVsOutputWindowPane> diagnosticPane_;
    ATL::CComPtr<IVsOutputWindowPane> runPane_;
    ATL::CComPtr<maro_DiagnosticWindow> diagnosticWindow_;
    ATL::CComPtr<IVsWindowFrame> diagnosticFrame_;
    ATL::CComPtr<maro_DiagnosticWindow> maro_liveOutput_;
    ATL::CComPtr<IVsWindowFrame> maro_liveOutputFrame_;
    bool maro_creatingOutput_ = false;
    ATL::CComPtr<maro_SourceWindow> maro_sourceWindow_;
    ATL::CComPtr<IVsWindowFrame> maro_sourceFrame_;
    std::unique_ptr<maro_InsightWorker> maro_insightWorker_;
    std::optional<maro_SourceItem> maro_navigation_;
    std::optional<Maro_Diagnostic> maro_pendingFix_;
    std::optional<Maro_Diagnostic> maro_pendingDiagnosticNavigation_;
    std::wstring maro_pendingDocumentation_;
    Maro_SourceRequest maro_fixSource_;
    std::uint64_t maro_navigationVersion_ = 0;
    std::uint64_t maro_insightVersion_ = 0, maro_insightHash_ = 0;
    std::uint64_t maro_displayedInsightVersion_ = 0;
    std::wstring maro_insightPath_;
    bool maro_creatingSource_ = false;
    bool maro_automatic_ = true;
    unsigned maro_codePage_ = 0;
    std::shared_ptr<maro_TraceSession> maro_trace_;
    std::mutex maro_traceMutex_;
    std::optional<maro_TraceSnapshot> maro_traceSnapshot_;
    bool creatingWindow_ = false;
    std::mutex diagnosticMutex_;
    std::optional<Maro_ResultEnvelope> pendingDiagnostic_;
    std::atomic<std::uint64_t> diagnosticSourceVersion_{0};
    maro_OutputQueue updateNotice_;
    ATL::CComPtr<IVsMonitorSelection> selectionMonitor_;
    ATL::CComPtr<IVsTextLines> observedLines_;
    ATL::CComPtr<IVsRunningDocumentTable> maro_documents_;
    VSCOOKIE maro_documentCookie_ = 0;
    std::shared_ptr<maro_ProcessInput> maro_input_;
    std::atomic<std::uint64_t> maro_runningVersion_{0};
    std::atomic<std::uint64_t> maro_finishedVersion_{0};
    bool maro_projectMode_ = false;
    bool maro_stopped_ = false;
    std::size_t maro_outputPaneSize_ = 0;
    std::size_t maro_diagnosticPaneSize_ = 0;
    std::unique_ptr<Maro_Engine> diagnosticEngine_;
    std::unique_ptr<Maro_Engine> runEngine_;
    DWORD selectionCookie_ = 0;
    DWORD textCookie_ = 0;
    UINT_PTR uiTimer_ = 0;
    ULONGLONG liveDeadline_ = 0;
    UINT maro_idleInterval_ = 100;
    std::uint64_t maro_debounceMilliseconds_ = 1000;
    bool initialized_ = false;
    bool initializing_ = false;
    bool uiBusy_ = false;
    bool maro_clearOutputPending_ = false;
    std::atomic<DWORD> maro_pendingCommands_{0};
    bool shuttingDown_ = false;
    HRESULT initializationResult_ = E_PENDING;
    maro_OutputQueue diagnosticOutput_;
    maro_OutputQueue runOutput_;
    std::uint64_t sourceVersion_ = 0;
    std::uint64_t lastLiveHash_ = 0;
    std::wstring lastLivePath_;
    std::atomic<std::uint64_t> runDiagnosticVersion_{0};
    std::atomic_bool updateRunning_{false};
    std::atomic_bool updateCancelled_{false};
    std::thread updateThread_;
    inline static Maro_CLive_Maro_Package* liveInstance_ = nullptr;
};
