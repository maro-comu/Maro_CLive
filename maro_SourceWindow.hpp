#pragma once
#include "maro_SourceInsight.hpp"
#include <atlbase.h>
#include <atlcom.h>
#include <vsshell.h>
#include <functional>
#include <optional>

inline const GUID maro_SourceWindowGuid =
    {0x7e10a0c6,0xee3d,0x460c,{0xaa,0x5c,0x83,0x9a,0x0b,0x7d,0x2d,0x34}};

class ATL_NO_VTABLE maro_SourceWindow : public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>, public IVsWindowPane
{
public:
    BEGIN_COM_MAP(maro_SourceWindow)
        COM_INTERFACE_ENTRY(IVsWindowPane)
    END_COM_MAP()
    STDMETHOD(SetSite)(IServiceProvider*) override { return S_OK; }
    STDMETHOD(CreatePaneWindow)(HWND,int,int,int,int,HWND*) override;
    STDMETHOD(GetDefaultSize)(SIZE* maro_size) override { if(!maro_size) return E_POINTER; *maro_size={300,600}; return S_OK; }
    STDMETHOD(ClosePane)() override;
    STDMETHOD(LoadViewState)(IStream*) override { return S_OK; }
    STDMETHOD(SaveViewState)(IStream*) override { return S_OK; }
    STDMETHOD(TranslateAccelerator)(LPMSG) override { return S_FALSE; }
    void maro_SetSource(maro_SourceInsight maro_insight);
    void maro_SetNavigate(std::function<void(const maro_SourceItem&)> maro_callback) { maro_navigate_=std::move(maro_callback); }
private:
    static LRESULT CALLBACK maro_Proc(HWND,UINT,WPARAM,LPARAM) noexcept;
    static LRESULT CALLBACK maro_TreeProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR) noexcept;
    void maro_Click(POINT maro_point);
    void maro_Rebuild();
    HWND maro_window_=nullptr, maro_tree_=nullptr;
    HFONT maro_font_=nullptr;
    maro_SourceInsight maro_insight_;
    std::function<void(const maro_SourceItem&)> maro_navigate_;
    std::optional<POINT> maro_clickPoint_;
    bool maro_updating_=false;
};
