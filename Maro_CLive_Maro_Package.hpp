#pragma once

#include "Maro_Engine.hpp"

#include <atlbase.h>
#include <atlcom.h>
#include <fpstfmt.h>
#include <textmgr.h>
#include <vsshell.h>

#include <cstdint>
#include <memory>
#include <string>

extern const CLSID Maro_CLive_Maro_PackageClsid;
extern const GUID Maro_CLive_Maro_CommandSet;
extern const GUID Maro_CLive_Maro_OutputPane;

constexpr DWORD Maro_CLive_Maro_CommandAnalyze = 0x0100;
constexpr DWORD Maro_CLive_Maro_CommandRun = 0x0101;

class ATL_NO_VTABLE Maro_CLive_Maro_Package
    : public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>,
      public ATL::CComCoClass<Maro_CLive_Maro_Package, &Maro_CLive_Maro_PackageClsid>,
      public IVsPackage,
      public IOleCommandTarget
{
public:
    Maro_CLive_Maro_Package() = default;
    ~Maro_CLive_Maro_Package() = default;

    DECLARE_NO_REGISTRY()
    DECLARE_NOT_AGGREGATABLE(Maro_CLive_Maro_Package)

    BEGIN_COM_MAP(Maro_CLive_Maro_Package)
        COM_INTERFACE_ENTRY(IVsPackage)
        COM_INTERFACE_ENTRY(IOleCommandTarget)
    END_COM_MAP()

    STDMETHOD(SetSite)(IServiceProvider* serviceProvider) override;
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

private:
    HRESULT EnsureOutputPane();
    HRESULT ReadActiveSource(Maro_SourceRequest& request, std::wstring& displayPath);
    HRESULT StartAnalysis(bool execute);
    void PublishResult(const Maro_ResultEnvelope& result) noexcept;
    void WriteOutput(std::wstring_view text) noexcept;
    void Shutdown() noexcept;

    ATL::CComPtr<IServiceProvider> serviceProvider_;
    ATL::CComPtr<IVsOutputWindowPane> outputPane_;
    std::unique_ptr<Maro_Engine> engine_;
    std::uint64_t sourceVersion_ = 0;
};
