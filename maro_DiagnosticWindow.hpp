#pragma once

#include "maro_Models.hpp"

#include <atlbase.h>
#include <atlcom.h>
#include <vsshell.h>
#include <string>
#include <string_view>

inline const GUID maro_DiagnosticWindowGuid =
    {0x85d62dc1, 0xf3c5, 0x4f1b, {0x96, 0xf0, 0xa6, 0x79, 0x81, 0x8e, 0x13, 0xba}};

class ATL_NO_VTABLE maro_DiagnosticWindow
    : public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>,
      public IVsWindowPane
{
public:
    BEGIN_COM_MAP(maro_DiagnosticWindow)
        COM_INTERFACE_ENTRY(IVsWindowPane)
    END_COM_MAP()

    STDMETHOD(SetSite)(IServiceProvider* site) override;
    STDMETHOD(CreatePaneWindow)(HWND parent, int x, int y, int width, int height, HWND* window) override;
    STDMETHOD(GetDefaultSize)(SIZE* size) override;
    STDMETHOD(ClosePane)() override;
    STDMETHOD(LoadViewState)(IStream* stream) override;
    STDMETHOD(SaveViewState)(IStream* stream) override;
    STDMETHOD(TranslateAccelerator)(LPMSG message) override;

    void maro_SetPending(std::wstring path, std::wstring status);
    void maro_SetResult(const Maro_ResultEnvelope& result);
    void maro_SetNotice(std::wstring text);
    void maro_AppendOutput(std::wstring_view text);
    void maro_ClearOutput();

private:
    static LRESULT CALLBACK maro_WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT maro_HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    bool maro_CreateControls();
    void maro_Layout();
    void maro_Refresh();
    void maro_UpdateFont();

    HWND maro_window_ = nullptr;
    HWND maro_summaryLabel_ = nullptr;
    HWND maro_summary_ = nullptr;
    HWND maro_issuesLabel_ = nullptr;
    HWND maro_issues_ = nullptr;
    HFONT maro_font_ = nullptr;
    HBRUSH maro_background_ = nullptr;
    int maro_split_ = 45;
    int maro_splitY_ = 0;
    std::wstring maro_path_;
    std::wstring maro_output_;
    std::wstring maro_status_ = L"C/C++ 문서를 열어 주세요.";
    std::wstring maro_notice_;
    std::wstring maro_details_;
    std::wstring maro_counts_ = L"실시간 진단";
};
