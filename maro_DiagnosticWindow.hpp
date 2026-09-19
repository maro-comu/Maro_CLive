#pragma once

#include "maro_Models.hpp"
#include "maro_SourceInsight.hpp"
#include "maro_Trace.hpp"

#include <atlbase.h>
#include <atlcom.h>
#include <vsshell.h>
#include <functional>
#include <deque>
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
    void maro_SetTrace(const maro_TraceSnapshot& maro_trace);
    void maro_SetTraceCallbacks(std::function<void()> maro_start, std::function<void()> maro_step,
        std::function<void()> maro_continue);
    void maro_SetAutomaticCallback(std::function<void()> maro_toggle);
    void maro_SetAutomatic(bool maro_enabled);
    void maro_SetEncodingCallback(std::function<void(unsigned)> maro_change);
    void maro_SetEncoding(unsigned maro_encoding);
    void maro_SetOutputMode(bool maro_enabled);
    void maro_SetExpandCallback(std::function<void()> maro_expand);
    void maro_ToggleExpanded();
    void maro_SetRunCallback(std::function<void()> maro_run);

private:
    static LRESULT CALLBACK maro_WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    static LRESULT CALLBACK maro_ExpandedProc(HWND maro_window, UINT maro_message, WPARAM maro_wparam, LPARAM maro_lparam) noexcept;
    static LRESULT CALLBACK maro_InputProc(HWND maro_window, UINT maro_message, WPARAM maro_wparam,
        LPARAM maro_lparam, UINT_PTR maro_id, DWORD_PTR maro_data) noexcept;
    LRESULT maro_HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT maro_HandleExpandedMessage(UINT maro_message, WPARAM maro_wparam, LPARAM maro_lparam);
    bool maro_CreateOutputControls(HWND maro_parent, bool maro_expanded);
    void maro_LayoutOutput(HWND maro_parent, int maro_bottom);
    void maro_ExpandOutput();
    void maro_CloseExpanded();
    void maro_UpdateSessionControls();
    void maro_SubmitInput(HWND maro_parent);
    bool maro_HandleCommand(HWND maro_parent, WPARAM maro_command);
    bool maro_HandleKey(HWND maro_parent, WPARAM maro_key);
    void maro_RefreshDebug();
    void maro_SelectHistory(std::size_t maro_index);
    const maro_TraceSnapshot& maro_VisibleTrace() const;
    void maro_DrawButton(const DRAWITEMSTRUCT& maro_item);
    void maro_DrawVisualization(const DRAWITEMSTRUCT& maro_item);
    bool maro_CreateVisualization(HWND maro_parent, bool maro_expanded);
    void maro_UpdateVisualization();
    void maro_LayoutVisualization(int maro_top, int maro_bottom, int maro_width);
    void maro_UpdateOutput(HWND maro_control, std::wstring_view maro_appended, std::size_t maro_oldLength, std::size_t maro_removed);
    bool maro_CreateControls();
    void maro_Layout();
    void maro_Refresh();
    void maro_UpdateFont();

    HWND maro_window_ = nullptr;
    HWND maro_summaryLabel_ = nullptr;
    HWND maro_summary_ = nullptr;
    HWND maro_issuesLabel_ = nullptr;
    HWND maro_issues_ = nullptr;
    HWND maro_expandedWindow_ = nullptr;
    HWND maro_expandedOutput_ = nullptr;
    HFONT maro_font_ = nullptr;
    HFONT maro_expandedFont_ = nullptr;
    HBRUSH maro_background_ = nullptr;
    int maro_split_ = 45;
    int maro_splitY_ = 0;
    bool maro_running_ = false;
    bool maro_inputOpen_ = false;
    bool maro_closing_ = false;
    bool maro_projectMode_ = false;
    bool maro_automatic_ = true;
    bool maro_outputMode_ = false;
    unsigned maro_encoding_ = 0;
    std::size_t maro_previewLine_ = 0;
    std::size_t maro_sourceHash_ = 0;
    std::size_t maro_traceHash_ = 0;
    maro_SourceInsight maro_source_;
    maro_TraceSnapshot maro_trace_;
    std::deque<maro_TraceSnapshot> maro_history_;
    std::size_t maro_historyIndex_ = 0;
    std::size_t maro_visualOffset_ = 0;
    std::function<bool(std::wstring)> maro_submitInput_;
    std::function<void()> maro_closeInput_;
    std::function<void()> maro_stopRun_;
    std::function<void()> maro_toggleProject_;
    std::function<void()> maro_toggleAutomatic_;
    std::function<void(unsigned)> maro_changeEncoding_;
    std::function<void()> maro_startTrace_;
    std::function<void()> maro_stepTrace_;
    std::function<void()> maro_continueTrace_;
    std::function<void()> maro_expandCallback_;
    std::function<void()> maro_runCallback_;
    std::wstring maro_path_;
    std::wstring maro_output_;
    std::wstring maro_status_ = L"C/C++ 문서를 열어 주세요.";
    std::wstring maro_notice_;
    std::wstring maro_details_;
    std::wstring maro_counts_ = L"실시간 진단";
    std::wstring maro_displayedDetails_;
    std::wstring maro_displayedCounts_;
};
