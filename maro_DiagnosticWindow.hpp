#pragma once

#include "maro_Models.hpp"
#include "maro_SourceInsight.hpp"

#include <atlbase.h>
#include <atlcom.h>
#include <vsshell.h>
#include <functional>
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
    void maro_SetSessionCallbacks(std::function<bool(std::wstring)> maro_submit,
        std::function<void()> maro_eof, std::function<void()> maro_stop);
    void maro_SetSessionState(bool maro_running, bool maro_inputOpen);
    void maro_SetProjectCallback(std::function<void()> maro_toggle);
    void maro_SetProjectMode(bool maro_enabled);
    void maro_SetSource(maro_SourceInsight maro_source);
    void maro_SetAutomaticCallback(std::function<void()> maro_toggle);
    void maro_SetAutomatic(bool maro_enabled);
    void maro_SetEncodingCallback(std::function<void(unsigned)> maro_change);
    void maro_SetEncoding(unsigned maro_encoding);
    void maro_SetOutputMode(bool maro_enabled);
    void maro_SetRunCallback(std::function<void()> maro_run);
    void maro_SetFixCallback(std::function<void(const Maro_Diagnostic&)> maro_fix);
    void maro_SetNavigateCallback(std::function<void(const Maro_Diagnostic&)> maro_navigate);
    void maro_SetDocumentationCallback(std::function<void(const Maro_Diagnostic&)> maro_documentation);

private:
    static LRESULT CALLBACK maro_WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    static LRESULT CALLBACK maro_InputProc(HWND maro_window, UINT maro_message, WPARAM maro_wparam,
        LPARAM maro_lparam, UINT_PTR maro_id, DWORD_PTR maro_data) noexcept;
    LRESULT maro_HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    bool maro_CreateOutputControls(HWND maro_parent);
    void maro_LayoutOutput(HWND maro_parent, int maro_bottom);
    void maro_UpdateSessionControls();
    void maro_SubmitInput(HWND maro_parent);
    bool maro_HandleCommand(HWND maro_parent, WPARAM maro_command);
    bool maro_HandleKey(HWND maro_parent, WPARAM maro_key);
    void maro_DrawButton(const DRAWITEMSTRUCT& maro_item);
    void maro_UpdateOutput(HWND maro_control, std::wstring_view maro_appended, std::size_t maro_oldLength, std::size_t maro_removed);
    bool maro_CreateControls();
    void maro_Layout();
    void maro_Refresh();
    bool maro_CanInput() const;
    std::optional<std::size_t> maro_HitLink(POINT maro_point) const;
    void maro_InvokeLink(std::size_t maro_index);
    void maro_UpdateFont();

    HWND maro_window_ = nullptr;
    HWND maro_summaryLabel_ = nullptr;
    HWND maro_summary_ = nullptr;
    HWND maro_issuesLabel_ = nullptr;
    HWND maro_issues_ = nullptr;
    HFONT maro_font_ = nullptr;
    HBRUSH maro_background_ = nullptr;
    int maro_split_ = 45;
    bool maro_running_ = false;
    bool maro_inputOpen_ = false;
    bool maro_inputSubmitted_ = false;
    bool maro_projectMode_ = false;
    bool maro_automatic_ = true;
    bool maro_outputMode_ = false;
    unsigned maro_encoding_ = 0;
    std::size_t maro_sourceHash_ = 0;
    maro_SourceInsight maro_source_;
    std::function<bool(std::wstring)> maro_submitInput_;
    std::function<void()> maro_closeInput_;
    std::function<void()> maro_stopRun_;
    std::function<void()> maro_toggleProject_;
    std::function<void()> maro_toggleAutomatic_;
    std::function<void(unsigned)> maro_changeEncoding_;
    std::function<void()> maro_runCallback_;
    std::function<void(const Maro_Diagnostic&)> maro_applyFix_;
    std::function<void(const Maro_Diagnostic&)> maro_navigate_;
    std::function<void(const Maro_Diagnostic&)> maro_documentation_;
    enum class maro_LinkKind { maro_Fix, maro_Navigate, maro_Documentation };
    struct maro_Link { long maro_start; long maro_end; maro_LinkKind maro_kind; Maro_Diagnostic maro_diagnostic; };
    std::vector<maro_Link> maro_links_;
    std::optional<std::size_t> maro_pressedLink_;
    std::wstring maro_path_;
    std::wstring maro_output_;
    std::wstring maro_status_ = L"C/C++ 문서를 열어 주세요.";
    std::wstring maro_notice_;
    std::wstring maro_details_;
    std::wstring maro_counts_ = L"실시간 진단";
    std::wstring maro_displayedDetails_;
    std::wstring maro_displayedCounts_;
};
