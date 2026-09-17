#pragma once

#include "maro_Engine.hpp"
#include "maro_OutputQueue.hpp"
#include "maro_DiagnosticWindow.hpp"

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
      public IVsSelectionEvents
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

private:
    HRESULT InitializeUi();
    HRESULT EnsureDiagnosticWindow(bool activate);
    void SetDiagnosticPending(const std::wstring& path, const wchar_t* status);
    void ProcessUi() noexcept;
    static void CALLBACK UiTimerProc(HWND window, UINT message, UINT_PTR timer, DWORD time) noexcept;
    HRESULT EnsureOutputPanes();
    HRESULT StartLiveTracking();
    HRESULT BindActiveBuffer();
    HRESULT ReadActiveSource(Maro_SourceRequest& request, std::wstring& displayPath);
    HRESULT StartAnalysis(bool execute);
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
    bool creatingWindow_ = false;
    std::mutex diagnosticMutex_;
    std::optional<Maro_ResultEnvelope> pendingDiagnostic_;
    std::atomic<std::uint64_t> diagnosticSourceVersion_{0};
    maro_OutputQueue updateNotice_;
    ATL::CComPtr<IVsMonitorSelection> selectionMonitor_;
    ATL::CComPtr<IVsTextLines> observedLines_;
    std::unique_ptr<Maro_Engine> diagnosticEngine_;
    std::unique_ptr<Maro_Engine> runEngine_;
    DWORD selectionCookie_ = 0;
    DWORD textCookie_ = 0;
    UINT_PTR uiTimer_ = 0;
    ULONGLONG liveDeadline_ = 0;
    bool initialized_ = false;
    bool initializing_ = false;
    bool uiBusy_ = false;
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
