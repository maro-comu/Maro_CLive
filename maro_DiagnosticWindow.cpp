#include "maro_DiagnosticWindow.hpp"

#include <algorithm>
#include <commctrl.h>
#include <sstream>
#include <windowsx.h>

namespace
{
constexpr int maro_expandId = 105;
constexpr int maro_inputId = 106;
constexpr int maro_sendId = 107;
constexpr int maro_eofId = 108;
constexpr int maro_stopId = 109;
constexpr int maro_projectId = 110;
constexpr int maro_automaticId = 111;
constexpr int maro_encodingId = 112;
constexpr int maro_dockedPreviewId = 113;
constexpr int maro_sourceId = 114;
constexpr int maro_explanationId = 115;
constexpr int maro_visualizationId = 116;
constexpr int maro_previousId = 117;
constexpr int maro_nextId = 118;
constexpr int maro_traceStartId = 119;
constexpr int maro_traceStepId = 120;
constexpr int maro_traceContinueId = 121;
constexpr int maro_visualLabelId = 122;
constexpr int maro_timelineId = 123;
constexpr int maro_hintId = 124;
constexpr std::size_t maro_outputLimit = 64 * 1024;

void maro_SetTextChanged(HWND maro_control, const std::wstring& maro_text)
{
    if (!maro_control) return;
    const int maro_size = GetWindowTextLengthW(maro_control);
    if (maro_size == static_cast<int>(maro_text.size()))
    {
        std::wstring maro_previous(static_cast<std::size_t>(maro_size) + 1, L'\0');
        maro_previous.resize(GetWindowTextW(maro_control, maro_previous.data(), static_cast<int>(maro_previous.size())));
        if (maro_previous == maro_text) return;
    }
    SetWindowTextW(maro_control, maro_text.c_str());
}

void maro_HashText(std::size_t& maro_hash, std::wstring_view maro_text)
{
    maro_hash ^= std::hash<std::wstring_view>{}(maro_text) + 0x9e3779b9 + (maro_hash << 6) + (maro_hash >> 2);
}

unsigned maro_EncodingAt(LRESULT maro_selection)
{
    constexpr unsigned maro_codePages[] = {0, 65001, 949, 1252};
    return maro_selection >= 0 && maro_selection < 4 ? maro_codePages[maro_selection] : 0;
}

void maro_HashVariable(std::size_t& maro_hash, const maro_TraceVariable& maro_variable)
{
    maro_HashText(maro_hash, maro_variable.maro_name);
    maro_HashText(maro_hash, maro_variable.maro_value);
    maro_HashText(maro_hash, maro_variable.maro_type);
    maro_hash ^= static_cast<std::size_t>(maro_variable.maro_available);
    for (const auto maro_dimension : maro_variable.maro_dimensions) maro_hash ^= maro_dimension + (maro_hash << 6) + (maro_hash >> 2);
    for (const auto& maro_child : maro_variable.maro_children) maro_HashVariable(maro_hash, maro_child);
}

std::wstring maro_ReadInput(HWND maro_parent)
{
    const HWND maro_input = GetDlgItem(maro_parent, maro_inputId);
    std::wstring maro_text(static_cast<std::size_t>((std::min)(GetWindowTextLengthW(maro_input), 4096)) + 1, L'\0');
    maro_text.resize(GetWindowTextW(maro_input, maro_text.data(), static_cast<int>(maro_text.size())));
    return maro_text;
}

LRESULT CALLBACK maro_OutputScroll(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR id, DWORD_PTR remainder)
{
    if (message == WM_MOUSEWHEEL)
    {
        const int delta = static_cast<int>(static_cast<INT_PTR>(remainder)) + GET_WHEEL_DELTA_WPARAM(wparam);
        SetWindowSubclass(window, maro_OutputScroll, id, static_cast<DWORD_PTR>(static_cast<INT_PTR>(delta % WHEEL_DELTA)));
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const int steps = delta / WHEEL_DELTA;
        if (lines == WHEEL_PAGESCROLL)
        {
            for (int step = 0; step < abs(steps); ++step)
            {
                SendMessageW(window, EM_SCROLL, steps > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
            }
        }
        else
        {
            SendMessageW(window, EM_LINESCROLL, 0, -steps * static_cast<int>((std::min)(lines, 100U)));
        }
        return 0;
    }
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, maro_OutputScroll, id);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
}

STDMETHODIMP maro_DiagnosticWindow::SetSite(IServiceProvider*)
{
    return S_OK;
}

STDMETHODIMP maro_DiagnosticWindow::CreatePaneWindow(
    HWND parent, int x, int y, int width, int height, HWND* window)
{
    if (window == nullptr)
    {
        return E_POINTER;
    }
    *window = nullptr;
    maro_closing_ = false;
    if (maro_window_ != nullptr)
    {
        *window = maro_window_;
        return S_OK;
    }
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = maro_WindowProc;
    type.hInstance = ATL::_AtlBaseModule.GetModuleInstance();
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = L"maro_CLive_Diagnostics";
    if (RegisterClassExW(&type) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HWND created = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"CLive_Maro 출력 및 진단",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x, y, width, height, parent, nullptr, type.hInstance, this);
    if (created == nullptr)
    {
        return E_FAIL;
    }
    *window = created;
    return S_OK;
}

STDMETHODIMP maro_DiagnosticWindow::GetDefaultSize(SIZE* size)
{
    if (size == nullptr)
    {
        return E_POINTER;
    }
    *size = maro_outputMode_ ? SIZE{700, 250} : SIZE{340, 700};
    return S_OK;
}

STDMETHODIMP maro_DiagnosticWindow::ClosePane()
{
    maro_closing_ = true;
    maro_CloseExpanded();
    if (maro_window_ != nullptr)
    {
        DestroyWindow(maro_window_);
    }
    return S_OK;
}

STDMETHODIMP maro_DiagnosticWindow::LoadViewState(IStream* stream)
{
    if (stream != nullptr)
    {
        ULONG read = 0;
        int split = 45;
        if (SUCCEEDED(stream->Read(&split, sizeof(split), &read)) && read == sizeof(split))
        {
            maro_split_ = std::clamp(split, 20, 80);
            maro_Layout();
        }
    }
    return S_OK;
}

STDMETHODIMP maro_DiagnosticWindow::SaveViewState(IStream* stream)
{
    return stream == nullptr ? E_POINTER : stream->Write(&maro_split_, sizeof(maro_split_), nullptr);
}

STDMETHODIMP maro_DiagnosticWindow::TranslateAccelerator(LPMSG message)
{
    if (message != nullptr && (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN) &&
        maro_HandleKey(GetParent(message->hwnd), message->wParam)) return S_OK;
    if (message != nullptr && message->message == WM_KEYDOWN && message->wParam == VK_RETURN &&
        (message->hwnd == GetDlgItem(maro_window_, maro_inputId) ||
            (maro_expandedWindow_ && message->hwnd == GetDlgItem(maro_expandedWindow_, maro_inputId))))
    {
        maro_SubmitInput(GetParent(message->hwnd));
        return S_OK;
    }
    if (message != nullptr && message->message == WM_KEYDOWN && message->wParam == 'A' &&
        GetKeyState(VK_CONTROL) < 0 && (message->hwnd == maro_summary_ || message->hwnd == maro_issues_ ||
            message->hwnd == maro_expandedOutput_))
    {
        SendMessageW(message->hwnd, EM_SETSEL, 0, -1);
        return S_OK;
    }
    return S_FALSE;
}

void maro_DiagnosticWindow::maro_SetSessionCallbacks(std::function<bool(std::wstring)> maro_submit,
    std::function<void()> maro_eof, std::function<void()> maro_stop)
{
    maro_submitInput_ = std::move(maro_submit);
    maro_closeInput_ = std::move(maro_eof);
    maro_stopRun_ = std::move(maro_stop);
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_SetOutputMode(bool maro_enabled)
{
    if (maro_window_) return;
    maro_outputMode_ = maro_enabled;
}

void maro_DiagnosticWindow::maro_SetExpandCallback(std::function<void()> maro_expand)
{
    maro_expandCallback_ = std::move(maro_expand);
}

void maro_DiagnosticWindow::maro_ToggleExpanded()
{
    if (maro_expandedWindow_) maro_CloseExpanded();
    else if (maro_outputMode_) maro_ExpandOutput();
    else if (maro_expandCallback_) maro_expandCallback_();
}

void maro_DiagnosticWindow::maro_SetRunCallback(std::function<void()> maro_run)
{
    maro_runCallback_ = std::move(maro_run);
}

void maro_DiagnosticWindow::maro_SetSessionState(bool maro_running, bool maro_inputOpen)
{
    if (maro_running_ == maro_running && maro_inputOpen_ == (maro_running && maro_inputOpen)) return;
    maro_running_ = maro_running;
    maro_inputOpen_ = maro_running && maro_inputOpen;
    if (!maro_running && maro_trace_.maro_waiting)
    {
        maro_trace_.maro_waiting = false;
        maro_trace_.maro_message = L"추적 종료 · 마지막 관측값";
        maro_UpdateVisualization();
    }
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_SetProjectCallback(std::function<void()> maro_toggle)
{
    maro_toggleProject_ = std::move(maro_toggle);
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_SetProjectMode(bool maro_enabled)
{
    if (maro_projectMode_ == maro_enabled) return;
    maro_projectMode_ = maro_enabled;
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_SetSource(maro_SourceInsight maro_source)
{
    const bool maro_hadSource = !maro_source_.maro_lines.empty();
    std::size_t maro_hash = maro_source.maro_lines.size();
    maro_HashText(maro_hash, maro_source.maro_path);
    for (const auto& maro_line : maro_source.maro_lines)
        maro_HashText(maro_hash, maro_line.maro_text);
    if (maro_sourceHash_ == maro_hash && maro_source_.maro_path == maro_source.maro_path &&
        maro_source_.maro_lines.size() == maro_source.maro_lines.size()) return;
    maro_sourceHash_ = maro_hash;
    maro_source_ = std::move(maro_source);
    maro_previewLine_ = 0;
    if (_wcsicmp(maro_trace_.maro_file.c_str(), maro_source_.maro_path.c_str()) != 0)
    {
        maro_trace_ = {};
        maro_traceHash_ = 0;
        maro_history_.clear();
        maro_historyIndex_ = 0;
        maro_visualOffset_ = 0;
    }
    else if (maro_trace_.maro_line && maro_trace_.maro_line <= maro_source_.maro_lines.size())
        maro_previewLine_ = maro_trace_.maro_line - 1;
    maro_UpdateVisualization();
    maro_UpdateSessionControls();
    if (maro_hadSource != !maro_source_.maro_lines.empty()) maro_Layout();
}

void maro_DiagnosticWindow::maro_SetTrace(const maro_TraceSnapshot& maro_trace)
{
    std::size_t maro_hash = maro_trace.maro_line + static_cast<std::size_t>(maro_trace.maro_waiting) * 65537;
    maro_HashText(maro_hash, maro_trace.maro_file);
    maro_HashText(maro_hash, maro_trace.maro_function);
    maro_HashText(maro_hash, maro_trace.maro_message);
    for (const auto& maro_variable : maro_trace.maro_variables)
        maro_HashVariable(maro_hash, maro_variable);
    for (const auto& maro_frame : maro_trace.maro_frames)
    {
        maro_HashText(maro_hash, maro_frame.maro_file);
        maro_HashText(maro_hash, maro_frame.maro_function);
        maro_hash ^= maro_frame.maro_line;
    }
    if (maro_traceHash_ == maro_hash) return;
    maro_traceHash_ = maro_hash;
    maro_trace_ = maro_trace;
    if (maro_outputMode_ && maro_trace.maro_line > 0 && maro_trace.maro_waiting)
    {
        if (maro_history_.size() == 256) maro_history_.pop_front();
        maro_history_.push_back(maro_trace);
        maro_historyIndex_ = maro_history_.size() - 1;
    }
    else if (maro_trace.maro_line == 0 && maro_trace.maro_message.empty())
    {
        maro_history_.clear();
        maro_historyIndex_ = 0;
        maro_visualOffset_ = 0;
    }
    if (maro_trace.maro_line > 0 && maro_trace.maro_line <= maro_source_.maro_lines.size() &&
        _wcsicmp(maro_trace.maro_file.c_str(), maro_source_.maro_path.c_str()) == 0)
        maro_previewLine_ = maro_trace.maro_line - 1;
    maro_UpdateVisualization();
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_SetTraceCallbacks(std::function<void()> maro_start, std::function<void()> maro_step,
    std::function<void()> maro_continue)
{
    maro_startTrace_ = std::move(maro_start);
    maro_stepTrace_ = std::move(maro_step);
    maro_continueTrace_ = std::move(maro_continue);
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_SetAutomaticCallback(std::function<void()> maro_toggle)
{
    maro_toggleAutomatic_ = std::move(maro_toggle);
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_SetAutomatic(bool maro_enabled)
{
    if (maro_automatic_ == maro_enabled) return;
    maro_automatic_ = maro_enabled;
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_SetEncodingCallback(std::function<void(unsigned)> maro_change)
{
    maro_changeEncoding_ = std::move(maro_change);
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_SetEncoding(unsigned maro_encoding)
{
    if (maro_encoding_ == maro_encoding) return;
    maro_encoding_ = maro_encoding;
    maro_UpdateSessionControls();
}

void maro_DiagnosticWindow::maro_UpdateSessionControls()
{
    for (HWND maro_parent : {maro_window_, maro_expandedWindow_})
    {
        if (!maro_parent) continue;
        const bool maro_canInput = maro_running_ && maro_inputOpen_ && static_cast<bool>(maro_submitInput_);
        EnableWindow(GetDlgItem(maro_parent, maro_inputId), maro_canInput);
        EnableWindow(GetDlgItem(maro_parent, maro_stopId), static_cast<bool>(maro_toggleAutomatic_) || static_cast<bool>(maro_stopRun_));
        maro_SetTextChanged(GetDlgItem(maro_parent, maro_stopId), maro_automatic_ ? L"끄기" : L"켜기");
        const std::wstring maro_encoding = maro_encoding_ == 65001 ? L"UTF-8" : maro_encoding_ == 949 ? L"CP949" :
            maro_encoding_ == 1252 ? L"CP1252" : L"자동";
        const std::wstring maro_hint = (maro_projectMode_ ? L"프로젝트" : L"현재 파일") + std::wstring(L" · ") + maro_encoding +
            L" · F5 실행 · F10 한 줄 · F9 계속 · Alt+←/→ 기록 · Ctrl+P 모드 · Ctrl+E 인코딩";
        maro_SetTextChanged(GetDlgItem(maro_parent, maro_hintId), maro_hint);
    }
}

bool maro_DiagnosticWindow::maro_HandleKey(HWND maro_parent, WPARAM maro_key)
{
    if (maro_key == VK_F5 && maro_runCallback_)
    {
        maro_runCallback_();
        return true;
    }
    if (maro_key == VK_F10)
    {
        maro_HandleCommand(maro_parent, MAKEWPARAM(maro_trace_.maro_waiting ? maro_traceStepId : maro_traceStartId, BN_CLICKED));
        return true;
    }
    if (maro_key == VK_F9)
    {
        maro_HandleCommand(maro_parent, MAKEWPARAM(maro_traceContinueId, BN_CLICKED));
        return true;
    }
    if (GetKeyState(VK_MENU) < 0 && (maro_key == VK_LEFT || maro_key == VK_RIGHT))
    {
        if (!maro_history_.empty())
            maro_SelectHistory(maro_key == VK_LEFT ? (maro_historyIndex_ > 0 ? maro_historyIndex_ - 1 : 0) : maro_historyIndex_ + 1);
        else maro_HandleCommand(maro_parent, MAKEWPARAM(maro_key == VK_LEFT ? maro_previousId : maro_nextId, BN_CLICKED));
        return true;
    }
    if (GetKeyState(VK_CONTROL) >= 0) return false;
    if (maro_key == 'D')
    {
        maro_HandleCommand(maro_parent, MAKEWPARAM(maro_eofId, BN_CLICKED));
        return true;
    }
    if (maro_key == 'P' && maro_toggleProject_)
    {
        maro_toggleProject_();
        return true;
    }
    if (maro_key == 'E' && maro_changeEncoding_)
    {
        const unsigned maro_next = maro_encoding_ == 0 ? 65001 : maro_encoding_ == 65001 ? 949 : maro_encoding_ == 949 ? 1252 : 0;
        maro_SetEncoding(maro_next);
        maro_changeEncoding_(maro_next);
        return true;
    }
    return false;
}

void maro_DiagnosticWindow::maro_SelectHistory(std::size_t maro_index)
{
    if (maro_history_.empty()) return;
    maro_historyIndex_ = (std::min)(maro_index, maro_history_.size() - 1);
    const auto& maro_trace = maro_VisibleTrace();
    if (maro_trace.maro_line && maro_trace.maro_line <= maro_source_.maro_lines.size() &&
        _wcsicmp(maro_trace.maro_file.c_str(), maro_source_.maro_path.c_str()) == 0) maro_previewLine_ = maro_trace.maro_line - 1;
    maro_UpdateVisualization();
}

const maro_TraceSnapshot& maro_DiagnosticWindow::maro_VisibleTrace() const
{
    return maro_history_.empty() ? maro_trace_ : maro_history_[(std::min)(maro_historyIndex_, maro_history_.size() - 1)];
}

void maro_DiagnosticWindow::maro_SubmitInput(HWND maro_parent)
{
    if (!maro_running_ || !maro_inputOpen_ || !maro_submitInput_) return;
    const std::wstring maro_text = maro_ReadInput(maro_parent);
    if (maro_submitInput_(maro_text))
    {
        for (HWND maro_target : {maro_window_, maro_expandedWindow_})
        {
            if (maro_target) SetWindowTextW(GetDlgItem(maro_target, maro_inputId), L"");
        }
    }
}

bool maro_DiagnosticWindow::maro_HandleCommand(HWND maro_parent, WPARAM maro_command)
{
    const auto maro_id = LOWORD(maro_command);
    if (maro_id == maro_encodingId && HIWORD(maro_command) == CBN_SELCHANGE)
    {
        const auto maro_encoding = maro_EncodingAt(SendDlgItemMessageW(maro_parent, maro_encodingId, CB_GETCURSEL, 0, 0));
        maro_SetEncoding(maro_encoding);
        if (maro_changeEncoding_) maro_changeEncoding_(maro_encoding);
        return true;
    }
    if (maro_id == maro_inputId && HIWORD(maro_command) == EN_CHANGE)
    {
        const HWND maro_other = maro_parent == maro_window_ ? maro_expandedWindow_ : maro_window_;
        if (maro_other)
        {
            const auto maro_text = maro_ReadInput(maro_parent);
            if (maro_text != maro_ReadInput(maro_other)) SetWindowTextW(GetDlgItem(maro_other, maro_inputId), maro_text.c_str());
        }
        return true;
    }
    if (HIWORD(maro_command) != BN_CLICKED) return false;
    switch (maro_id)
    {
    case maro_expandId:
        maro_ToggleExpanded();
        return true;
    case maro_sendId:
        maro_SubmitInput(maro_parent);
        return true;
    case maro_eofId:
        if (maro_running_ && maro_inputOpen_ && maro_closeInput_)
        {
            maro_closeInput_();
            maro_inputOpen_ = false;
            maro_UpdateSessionControls();
        }
        return true;
    case maro_stopId:
        if (maro_toggleAutomatic_) maro_toggleAutomatic_();
        else if (maro_stopRun_) maro_stopRun_();
        return true;
    case maro_projectId:
        if (maro_toggleProject_) maro_toggleProject_();
        return true;
    case maro_automaticId:
        if (maro_toggleAutomatic_) maro_toggleAutomatic_();
        return true;
    case maro_previousId:
    case maro_nextId:
        if (maro_id == maro_previousId && maro_previewLine_ > 0) --maro_previewLine_;
        if (maro_id == maro_nextId && maro_previewLine_ + 1 < maro_source_.maro_lines.size()) ++maro_previewLine_;
        maro_UpdateVisualization();
        maro_UpdateSessionControls();
        return true;
    case maro_traceStartId:
        if (!maro_trace_.maro_waiting && maro_startTrace_) maro_startTrace_();
        return true;
    case maro_traceStepId:
        if (maro_trace_.maro_waiting && maro_stepTrace_) maro_stepTrace_();
        if (!maro_history_.empty()) maro_historyIndex_ = maro_history_.size() - 1;
        maro_trace_.maro_waiting = false;
        maro_traceHash_ = 0;
        maro_UpdateVisualization();
        maro_UpdateSessionControls();
        return true;
    case maro_traceContinueId:
        if (maro_trace_.maro_waiting && maro_continueTrace_) maro_continueTrace_();
        maro_trace_.maro_waiting = false;
        maro_traceHash_ = 0;
        maro_UpdateVisualization();
        maro_UpdateSessionControls();
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK maro_DiagnosticWindow::maro_InputProc(HWND maro_window, UINT maro_message, WPARAM maro_wparam,
    LPARAM maro_lparam, UINT_PTR maro_id, DWORD_PTR maro_data) noexcept
{
    auto* maro_instance = reinterpret_cast<maro_DiagnosticWindow*>(maro_data);
    if (maro_message == WM_NCDESTROY) RemoveWindowSubclass(maro_window, maro_InputProc, maro_id);
    try
    {
        if (maro_message == WM_GETDLGCODE && maro_wparam == VK_RETURN) return DLGC_WANTALLKEYS;
        if ((maro_message == WM_KEYDOWN || maro_message == WM_SYSKEYDOWN) &&
            maro_instance->maro_HandleKey(GetParent(maro_window), maro_wparam)) return 0;
        if (GetDlgCtrlID(maro_window) == maro_visualizationId && maro_message == WM_MOUSEWHEEL)
        {
            const auto maro_count = maro_instance->maro_VisibleTrace().maro_variables.size();
            if (GET_WHEEL_DELTA_WPARAM(maro_wparam) > 0)
                maro_instance->maro_visualOffset_ -= maro_instance->maro_visualOffset_ > 0;
            else if (maro_instance->maro_visualOffset_ + 1 < maro_count) ++maro_instance->maro_visualOffset_;
            InvalidateRect(maro_window, nullptr, FALSE);
            return 0;
        }
        if (GetDlgCtrlID(maro_window) == maro_inputId && maro_message == WM_KEYDOWN && maro_wparam == VK_RETURN)
        {
            maro_instance->maro_SubmitInput(GetParent(maro_window));
            return 0;
        }
        if (maro_message == WM_CHAR && maro_wparam == VK_RETURN) return 0;
        if (maro_message == WM_KEYDOWN && maro_wparam == 'A' && GetKeyState(VK_CONTROL) < 0)
        {
            SendMessageW(maro_window, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    catch (...) { return 0; }
    return DefSubclassProc(maro_window, maro_message, maro_wparam, maro_lparam);
}

void maro_DiagnosticWindow::maro_SetPending(std::wstring path, std::wstring status)
{
    maro_path_ = std::move(path);
    maro_status_ = std::move(status);
    maro_details_.clear();
    maro_counts_ = L"실시간 진단";
    maro_Refresh();
}

void maro_DiagnosticWindow::maro_SetResult(const Maro_ResultEnvelope& result)
{
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::wostringstream details;
    for (const auto& diagnostic : result.diagnostics)
    {
        const bool error = diagnostic.severity == Maro_Severity::Error || diagnostic.severity == Maro_Severity::Fatal;
        const bool warning = diagnostic.severity == Maro_Severity::Warning;
        if (!error && !warning)
        {
            continue;
        }
        errors += error;
        warnings += warning;
        details << (diagnostic.code.empty() ? L"CLIVE-DIAG" : diagnostic.code)
            << L" · " << (error ? L"오류" : L"경고");
        if (diagnostic.range.start.line != 0)
        {
            details << L" · " << diagnostic.range.start.line << L"행";
            if (diagnostic.range.start.column != 0) details << L" " << diagnostic.range.start.column << L"열";
        }
        if (!diagnostic.sourcePath.empty())
        {
            details << L"\r\n" << diagnostic.sourcePath;
        }
        details << L"\r\n" << diagnostic.friendlyMessage;
        if (!diagnostic.originalDiagnostic.empty() && diagnostic.friendlyMessage.find(diagnostic.originalDiagnostic) == std::wstring::npos)
            details << L"\r\n컴파일러: " << diagnostic.originalDiagnostic;
        if (diagnostic.maro_relatedCount != 0)
            details << L"\r\n연관 진단 " << diagnostic.maro_relatedCount << L"개 접음";
        details << L"\r\n\r\n";
    }
    maro_status_ = result.statusText;
    maro_counts_ = L"실시간 진단 · 오류 " + std::to_wstring(errors) + L" · 경고 " + std::to_wstring(warnings);
    maro_details_ = details.str();
    if (maro_details_.empty())
    {
        maro_details_ = result.status == Maro_Status::Success ? L"문제 없음." : result.statusText;
    }
    if (result.maro_compileMilliseconds != 0 || result.maro_runMilliseconds != 0)
        maro_details_ += L"\r\n컴파일 " + std::to_wstring(result.maro_compileMilliseconds) +
            L" ms · 실행 " + std::to_wstring(result.maro_runMilliseconds) + L" ms";
    maro_Refresh();
}

void maro_DiagnosticWindow::maro_SetNotice(std::wstring text)
{
    maro_notice_ = std::move(text);
    maro_Refresh();
    if (maro_expandedWindow_) maro_UpdateVisualization();
}

void maro_DiagnosticWindow::maro_AppendOutput(std::wstring_view text)
{
    if (!maro_outputMode_) return;
    if (text.empty())
    {
        return;
    }
    wchar_t previous = maro_output_.empty() ? L'\0' : maro_output_.back();
    if (text.size() > maro_outputLimit)
    {
        previous = text[text.size() - maro_outputLimit - 1];
        text.remove_prefix(text.size() - maro_outputLimit);
    }
    std::wstring appended;
    appended.reserve(text.size() * 2);
    for (const wchar_t character : text)
    {
        if (character == L'\n' && previous != L'\r')
        {
            appended.push_back(L'\r');
        }
        appended.push_back(character == L'\0' ? L' ' : character);
        previous = character;
    }
    const auto oldLength = maro_output_.size();
    maro_output_ += appended;
    std::size_t removed = 0;
    if (maro_output_.size() > maro_outputLimit)
    {
        removed = maro_output_.size() - maro_outputLimit;
        if (maro_output_[removed] == L'\n' && maro_output_[removed - 1] == L'\r')
        {
            ++removed;
        }
        if (removed < maro_output_.size() && maro_output_[removed] >= 0xdc00 && maro_output_[removed] <= 0xdfff)
        {
            ++removed;
        }
        maro_output_.erase(0, removed);
    }
    maro_UpdateOutput(maro_expandedOutput_ ? maro_expandedOutput_ : maro_summary_, appended, oldLength, removed);
}

void maro_DiagnosticWindow::maro_UpdateOutput(HWND maro_control, std::wstring_view maro_appended,
    std::size_t maro_oldLength, std::size_t maro_removed)
{
    if (maro_control == nullptr)
    {
        return;
    }
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    SendMessageW(maro_control, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
    const bool follow = selectionStart == maro_oldLength && selectionEnd == maro_oldLength;
    const LRESULT firstLine = SendMessageW(maro_control, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (maro_removed != 0)
    {
        SetWindowTextW(maro_control, maro_output_.c_str());
    }
    else
    {
        SendMessageW(maro_control, EM_SETSEL, maro_oldLength, maro_oldLength);
        SendMessageW(maro_control, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(maro_appended.data()));
    }
    if (follow)
    {
        SendMessageW(maro_control, EM_SETSEL, maro_output_.size(), maro_output_.size());
        SendMessageW(maro_control, EM_SCROLLCARET, 0, 0);
    }
    else
    {
        SendMessageW(maro_control, EM_SETSEL,
            selectionStart > maro_removed ? selectionStart - maro_removed : 0,
            selectionEnd > maro_removed ? selectionEnd - maro_removed : 0);
        SendMessageW(maro_control, EM_LINESCROLL, 0,
            firstLine - SendMessageW(maro_control, EM_GETFIRSTVISIBLELINE, 0, 0));
    }
}

void maro_DiagnosticWindow::maro_ClearOutput()
{
    maro_output_.clear();
    if (maro_summary_ != nullptr && maro_outputMode_)
    {
        SetWindowTextW(maro_summary_, L"");
    }
    if (maro_expandedOutput_ != nullptr) SetWindowTextW(maro_expandedOutput_, L"");
}

void maro_DiagnosticWindow::maro_Refresh()
{
    if (maro_window_ == nullptr)
    {
        return;
    }
    std::wstring details;
    if (!maro_details_.empty())
    {
        details = maro_details_;
    }
    else
    {
        details = maro_status_;
    }
    if (!maro_path_.empty())
    {
        details += L"\r\n\r\n" + maro_path_;
    }
    if (!maro_notice_.empty())
    {
        details += L"\r\n\r\n" + maro_notice_;
    }
    if (maro_displayedCounts_ != maro_counts_)
    {
        maro_displayedCounts_ = maro_counts_;
        SetWindowTextW(maro_issuesLabel_, maro_counts_.c_str());
    }
    if (maro_displayedDetails_ != details)
    {
        maro_displayedDetails_ = details;
        SetWindowTextW(maro_issues_, details.c_str());
    }
}

bool maro_DiagnosticWindow::maro_CreateControls()
{
    const auto create = [this](const wchar_t* type, const wchar_t* caption, DWORD style, int id) {
        return CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, maro_window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            ATL::_AtlBaseModule.GetModuleInstance(), nullptr);
    };
    maro_background_ = CreateSolidBrush(RGB(24, 24, 24));
    maro_summaryLabel_ = create(L"STATIC", maro_outputMode_ ? L"실시간 출력" : L"디버그", SS_LEFT, 101);
    maro_summary_ = create(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 102);
    if (!maro_outputMode_)
    {
        maro_issuesLabel_ = create(L"STATIC", L"실시간 진단", SS_LEFT, 103);
        maro_issues_ = create(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 104);
    }
    if (!maro_CreateOutputControls(maro_window_, false)) return false;
    if (!maro_background_ || !maro_summaryLabel_ || !maro_summary_ || (!maro_outputMode_ && (!maro_issuesLabel_ || !maro_issues_)))
    {
        return false;
    }
    SendMessageW(maro_summary_, EM_SETLIMITTEXT, 64 * 1024, 0);
    SendMessageW(maro_issues_, EM_SETLIMITTEXT, 1024 * 1024, 0);
    if (!SetWindowSubclass(maro_summary_, maro_OutputScroll, 1, 0) ||
        (maro_issues_ && !SetWindowSubclass(maro_issues_, maro_OutputScroll, 1, 0)))
    {
        return false;
    }
    for (const HWND maro_child : {maro_summary_, maro_issues_})
        if (maro_child) SetWindowSubclass(maro_child, maro_InputProc, 2, reinterpret_cast<DWORD_PTR>(this));
    if (maro_outputMode_) SetWindowTextW(maro_summary_, maro_output_.c_str());
    maro_UpdateFont();
    maro_displayedDetails_.clear();
    maro_displayedCounts_.clear();
    maro_Refresh();
    maro_UpdateVisualization();
    maro_Layout();
    return true;
}

void maro_DiagnosticWindow::maro_UpdateFont()
{
    HFONT font = CreateFontW(-MulDiv(10, static_cast<int>(GetDpiForWindow(maro_window_)), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
    if (font == nullptr)
    {
        return;
    }
    for (const auto child : {maro_summaryLabel_, maro_summary_, maro_issuesLabel_, maro_issues_})
    {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (maro_font_ != nullptr)
    {
        DeleteObject(maro_font_);
    }
    maro_font_ = font;
}

void maro_DiagnosticWindow::maro_Layout()
{
    if (maro_window_ == nullptr || maro_summary_ == nullptr)
    {
        return;
    }
    RECT bounds{};
    GetClientRect(maro_window_, &bounds);
    const int pad = MulDiv(8, GetDpiForWindow(maro_window_), 96);
    const int label = MulDiv(24, GetDpiForWindow(maro_window_), 96);
    const int width = (std::max)(1L, bounds.right - 2 * pad);
    const int height = (std::max)(1L, bounds.bottom);
    maro_LayoutOutput(maro_window_, height);
    if (!maro_outputMode_)
    {
        maro_splitY_ = (std::max)(label * 3, height * maro_split_ / 100);
        const int maro_top = label * 2 + pad * 2;
        MoveWindow(maro_summary_, pad, maro_top, width, (std::max)(1, maro_splitY_ - maro_top - pad), TRUE);
        MoveWindow(maro_issuesLabel_, pad, maro_splitY_ + pad, width, label, TRUE);
        MoveWindow(maro_issues_, pad, maro_splitY_ + pad + label, width,
            (std::max)(1, height - maro_splitY_ - label - 2 * pad), TRUE);
    }
    InvalidateRect(maro_window_, nullptr, TRUE);
}

LRESULT CALLBACK maro_DiagnosticWindow::maro_WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    auto* instance = reinterpret_cast<maro_DiagnosticWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        instance = static_cast<maro_DiagnosticWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        instance->AddRef();
        instance->maro_window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
    }
    if (instance == nullptr)
    {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == WM_NCDESTROY)
    {
        instance->maro_closing_ = true;
        instance->maro_CloseExpanded();
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        instance->maro_window_ = nullptr;
        instance->maro_summary_ = nullptr;
        instance->maro_issues_ = nullptr;
        instance->maro_summaryLabel_ = nullptr;
        instance->maro_issuesLabel_ = nullptr;
        if (instance->maro_font_ != nullptr)
        {
            DeleteObject(instance->maro_font_);
            instance->maro_font_ = nullptr;
        }
        if (instance->maro_background_ != nullptr)
        {
            DeleteObject(instance->maro_background_);
            instance->maro_background_ = nullptr;
        }
        instance->Release();
        return DefWindowProcW(window, message, wparam, lparam);
    }
    try
    {
        return instance->maro_HandleMessage(message, wparam, lparam);
    }
    catch (...)
    {
        return message == WM_CREATE ? -1 : DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT maro_DiagnosticWindow::maro_HandleMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (maro_HandleKey(maro_window_, wparam)) return 0;
        break;
    case WM_COMMAND:
        if (maro_HandleCommand(maro_window_, wparam)) return 0;
        break;
    case WM_DRAWITEM:
        if (reinterpret_cast<DRAWITEMSTRUCT*>(lparam)->CtlID == maro_visualizationId)
            maro_DrawVisualization(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
        else
            maro_DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
        return TRUE;
    case WM_CREATE:
        return maro_CreateControls() ? 0 : -1;
    case WM_SIZE:
        maro_Layout();
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        maro_UpdateFont();
        maro_Layout();
        return 0;
    case WM_SETFOCUS:
        SetFocus(maro_summary_);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wparam), RGB(224, 224, 224));
        SetBkColor(reinterpret_cast<HDC>(wparam), RGB(24, 24, 24));
        return reinterpret_cast<LRESULT>(maro_background_);
    case WM_ERASEBKGND:
        {
            RECT bounds{};
            GetClientRect(maro_window_, &bounds);
            FillRect(reinterpret_cast<HDC>(wparam), &bounds, maro_background_);
        }
        return 1;
    case WM_LBUTTONDOWN:
        if (!maro_outputMode_ && abs(GET_Y_LPARAM(lparam) - maro_splitY_) <= 8)
        {
            SetCapture(maro_window_);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == maro_window_)
        {
            RECT bounds{};
            GetClientRect(maro_window_, &bounds);
            maro_split_ = std::clamp(GET_Y_LPARAM(lparam) * 100 / (std::max)(1L, bounds.bottom), 20L, 80L);
            maro_Layout();
        }
        if (!maro_outputMode_ && (GetCapture() == maro_window_ || abs(GET_Y_LPARAM(lparam) - maro_splitY_) <= 8))
        {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == maro_window_)
        {
            ReleaseCapture();
        }
        return 0;
    case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(maro_window_, &paint);
            RECT bounds{};
            GetClientRect(maro_window_, &bounds);
            FillRect(dc, &bounds, maro_background_);
            EndPaint(maro_window_, &paint);
        }
        return 0;
    default:
        return DefWindowProcW(maro_window_, message, wparam, lparam);
    }
    return DefWindowProcW(maro_window_, message, wparam, lparam);
}

bool maro_DiagnosticWindow::maro_CreateOutputControls(HWND maro_parent, bool maro_expanded)
{
    const auto maro_instance = ATL::_AtlBaseModule.GetModuleInstance();
    for (const auto& maro_button : {std::pair{maro_expandId, maro_expanded ? L"복원" : L"확장"},
        std::pair{maro_stopId, L"끄기"}})
    {
        const HWND maro_created = CreateWindowExW(0, L"BUTTON", maro_button.second, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, maro_parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(maro_button.first)), maro_instance, nullptr);
        if (!maro_created) return false;
        SetWindowSubclass(maro_created, maro_InputProc, 2, reinterpret_cast<DWORD_PTR>(this));
    }
    if (maro_outputMode_ || maro_expanded)
    {
        const HWND maro_input = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 1, 1, maro_parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(maro_inputId)), maro_instance, nullptr);
        if (!maro_input) return false;
        SendMessageW(maro_input, EM_SETLIMITTEXT, 4096, 0);
        SendMessageW(maro_input, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"입력 후 Enter · 입력 종료 Ctrl+D"));
        SetWindowSubclass(maro_input, maro_InputProc, 2, reinterpret_cast<DWORD_PTR>(this));
    }
    if (!CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        0, 0, 1, 1, maro_parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(maro_hintId)), maro_instance, nullptr)) return false;
    maro_UpdateSessionControls();
    return true;
}

void maro_DiagnosticWindow::maro_LayoutOutput(HWND maro_parent, int maro_bottom)
{
    RECT maro_bounds{};
    GetClientRect(maro_parent, &maro_bounds);
    const int maro_pad = MulDiv(8, static_cast<int>(GetDpiForWindow(maro_parent)), 96);
    const int maro_width = (std::max)(1L, maro_bounds.right - 2 * maro_pad);
    const int maro_unit = MulDiv(28, static_cast<int>(GetDpiForWindow(maro_parent)), 96);
    const bool maro_expanded = maro_parent != maro_window_;
    const int maro_button = (std::max)(32, (std::min)(70, maro_width / 4));
    const HWND maro_output = maro_parent == maro_window_ ? maro_summary_ : maro_expandedOutput_;
    MoveWindow(GetDlgItem(maro_parent, maro_expandId), maro_bounds.right - maro_pad - 2 * maro_button,
        maro_pad, maro_button - 4, maro_unit, TRUE);
    MoveWindow(GetDlgItem(maro_parent, maro_stopId), maro_bounds.right - maro_pad - maro_button,
        maro_pad, maro_button, maro_unit, TRUE);
    MoveWindow(GetDlgItem(maro_parent, 101), maro_pad, maro_pad,
        (std::max)(1, maro_width - 2 * maro_button - 4), maro_unit, TRUE);
    const int maro_hintY = maro_pad + maro_unit + 2;
    const int maro_hintHeight = MulDiv(20, static_cast<int>(GetDpiForWindow(maro_parent)), 96);
    MoveWindow(GetDlgItem(maro_parent, maro_hintId), maro_pad, maro_hintY, maro_width, maro_hintHeight, TRUE);
    const int maro_outputY = maro_hintY + maro_hintHeight + 4;
    const int maro_available = (std::max)(1, maro_bottom - maro_outputY - maro_pad);
    const int maro_visualHeight = maro_expanded ? (std::max)(160, maro_available * 70 / 100) : 0;
    const int maro_inputY = (std::max)(maro_outputY + 1, maro_bottom - maro_visualHeight - maro_unit - maro_pad - 4);
    MoveWindow(maro_output, maro_pad, maro_outputY, maro_width,
        (std::max)(1, maro_inputY - maro_outputY - 4), TRUE);
    MoveWindow(GetDlgItem(maro_parent, maro_inputId), maro_pad, maro_inputY,
        maro_width, maro_unit, TRUE);
    if (maro_expanded)
        maro_LayoutVisualization(maro_inputY + maro_unit + 8, maro_bottom - maro_pad, maro_width);
    else if (maro_outputMode_)
    {
        ShowWindow(maro_output, maro_expandedWindow_ ? SW_HIDE : SW_SHOW);
        ShowWindow(GetDlgItem(maro_parent, maro_inputId), maro_expandedWindow_ ? SW_HIDE : SW_SHOW);
    }
    const HFONT maro_font = maro_parent == maro_window_ ? maro_font_ : maro_expandedFont_;
    for (int maro_id : {101, 102, maro_expandId, maro_inputId, maro_stopId, maro_sourceId,
        maro_explanationId, maro_visualizationId, maro_visualLabelId, maro_timelineId, maro_hintId})
        SendMessageW(GetDlgItem(maro_parent, maro_id), WM_SETFONT, reinterpret_cast<WPARAM>(maro_font), TRUE);
}

bool maro_DiagnosticWindow::maro_CreateVisualization(HWND maro_parent, bool maro_expanded)
{
    const auto maro_create = [maro_parent](const wchar_t* maro_type, const wchar_t* maro_caption, DWORD maro_style, int maro_id) {
        return CreateWindowExW(0, maro_type, maro_caption, WS_CHILD | WS_VISIBLE | maro_style, 0, 0, 1, 1,
            maro_parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(maro_id)), ATL::_AtlBaseModule.GetModuleInstance(), nullptr);
    };
    if (!maro_expanded) return true;
    if (!maro_create(L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL, maro_sourceId) ||
        !maro_create(L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL, maro_explanationId) ||
        !maro_create(L"STATIC", L"", SS_OWNERDRAW, maro_visualizationId) ||
        !maro_create(L"STATIC", L"정적 미리보기 · 실제 실행과 별개", SS_LEFT | SS_ENDELLIPSIS, maro_visualLabelId)) return false;
    INITCOMMONCONTROLSEX maro_controls{sizeof(maro_controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&maro_controls);
    if (!maro_create(TRACKBAR_CLASSW, L"실행 기록", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, maro_timelineId)) return false;
    SendDlgItemMessageW(maro_parent, maro_sourceId, EM_SETLIMITTEXT, 16384, 0);
    SendDlgItemMessageW(maro_parent, maro_explanationId, EM_SETLIMITTEXT, 4096, 0);
    SetWindowSubclass(GetDlgItem(maro_parent, maro_sourceId), maro_OutputScroll, 1, 0);
    SetWindowSubclass(GetDlgItem(maro_parent, maro_explanationId), maro_OutputScroll, 1, 0);
    for (const int maro_id : {maro_sourceId, maro_explanationId, maro_timelineId, maro_visualizationId})
        SetWindowSubclass(GetDlgItem(maro_parent, maro_id), maro_InputProc, 2, reinterpret_cast<DWORD_PTR>(this));
    return true;
}

void maro_DiagnosticWindow::maro_LayoutVisualization(int maro_top, int maro_bottom, int maro_width)
{
    const int maro_pad = 8;
    const int maro_unit = MulDiv(28, static_cast<int>(GetDpiForWindow(maro_expandedWindow_)), 96);
    const int maro_leftWidth = (std::max)(120, maro_width * 55 / 100 - maro_pad);
    const int maro_rightX = maro_pad + maro_leftWidth + maro_pad;
    const int maro_rightWidth = (std::max)(1, maro_width - maro_leftWidth - maro_pad);
    const int maro_controlsHeight = maro_unit + 8;
    const int maro_explanationHeight = (std::min)(64, (std::max)(32, (maro_bottom - maro_top) / 5));
    const int maro_controlsTop = (std::max)(maro_top + maro_unit + 1, maro_bottom - maro_controlsHeight);
    const int maro_explanationTop = (std::max)(maro_top + maro_unit + 1, maro_controlsTop - maro_explanationHeight - 4);
    MoveWindow(GetDlgItem(maro_expandedWindow_, maro_visualLabelId), maro_pad, maro_top, maro_leftWidth, maro_unit, TRUE);
    MoveWindow(GetDlgItem(maro_expandedWindow_, maro_sourceId), maro_pad, maro_top + maro_unit,
        maro_leftWidth, (std::max)(1, maro_explanationTop - maro_top - maro_unit - 4), TRUE);
    MoveWindow(GetDlgItem(maro_expandedWindow_, maro_explanationId), maro_pad, maro_explanationTop,
        maro_leftWidth, maro_explanationHeight, TRUE);
    MoveWindow(GetDlgItem(maro_expandedWindow_, maro_timelineId), maro_pad, maro_controlsTop,
        maro_leftWidth, maro_unit, TRUE);
    MoveWindow(GetDlgItem(maro_expandedWindow_, maro_visualizationId), maro_rightX, maro_top,
        maro_rightWidth, (std::max)(1, maro_bottom - maro_top), TRUE);
}

void maro_DiagnosticWindow::maro_UpdateVisualization()
{
    const bool maro_hasSource = !maro_source_.maro_lines.empty();
    if (maro_hasSource) maro_previewLine_ = (std::min)(maro_previewLine_, maro_source_.maro_lines.size() - 1);
    std::wstring maro_explanation = L"소스 문서를 열면 컴파일 전에도 정적 미리보기를 볼 수 있습니다.";
    std::wstring maro_preview;
    if (maro_hasSource)
    {
        const auto& maro_line = maro_source_.maro_lines[maro_previewLine_];
        maro_explanation = std::to_wstring(maro_line.maro_line) + L"행 · " + maro_line.maro_explanation;
        const auto maro_start = maro_previewLine_ > 3 ? maro_previewLine_ - 3 : 0;
        const auto maro_end = (std::min)(maro_source_.maro_lines.size(), maro_start + 9);
        for (std::size_t maro_index = maro_start; maro_index < maro_end; ++maro_index)
        {
            const auto& maro_current = maro_source_.maro_lines[maro_index];
            maro_preview += (maro_index == maro_previewLine_ ? L"▶ " : L"  ") + std::to_wstring(maro_current.maro_line) +
                L"  " + maro_current.maro_text.substr(0, 1000) + (maro_current.maro_text.size() > 1000 ? L"…" : L"") + L"\r\n";
        }
        if (maro_source_.maro_truncated) maro_explanation += L" · 큰 소스는 일부만 표시";
    }
    maro_RefreshDebug();
    if (!maro_expandedWindow_) return;
    const auto& maro_trace = maro_VisibleTrace();
    const bool maro_runtime = maro_trace.maro_line > 0 || !maro_trace.maro_message.empty();
    std::wstring maro_caption = L"정적 미리보기 · Alt+←/→ 줄 이동 · F10 실제 실행";
    if (maro_runtime)
    {
        maro_caption = maro_historyIndex_ + 1 < maro_history_.size() ? L"과거 관측값 · " :
            maro_trace_.maro_waiting ? L"실제 추적 일시정지 · " : L"마지막 관측값 · ";
        maro_caption += maro_trace.maro_function + L" · " + std::to_wstring(maro_trace.maro_line) + L"행";
        if (!maro_history_.empty()) maro_caption += L" · 기록 " + std::to_wstring(maro_historyIndex_ + 1) + L"/" + std::to_wstring(maro_history_.size());
        if (maro_trace.maro_line != maro_previewLine_ + 1)
            maro_caption += L" / 정적 미리보기 " + std::to_wstring(maro_previewLine_ + 1) + L"행";
        if (!maro_trace.maro_message.empty()) maro_explanation += L"\r\n" + maro_trace.maro_message.substr(0, 1000);
        if (!maro_trace.maro_file.empty() && _wcsicmp(maro_trace.maro_file.c_str(), maro_source_.maro_path.c_str()) != 0)
            maro_explanation += L"\r\n추적 위치: " + maro_trace.maro_file + L" (미리보기 문서와 다름)";
    }
    maro_SetTextChanged(GetDlgItem(maro_expandedWindow_, maro_sourceId), maro_preview);
    if (maro_trace_.maro_line == 0 && !maro_trace_.maro_message.empty())
        maro_explanation += L"\r\n" + maro_trace_.maro_message.substr(0, 1000);
    if (!maro_notice_.empty()) maro_explanation += L"\r\n" + maro_notice_.substr(0, 1000);
    maro_SetTextChanged(GetDlgItem(maro_expandedWindow_, maro_explanationId), maro_explanation);
    maro_SetTextChanged(GetDlgItem(maro_expandedWindow_, maro_visualLabelId), maro_caption);
    std::wstring maro_accessible;
    for (const auto& maro_variable : maro_trace.maro_variables)
    {
        if (maro_accessible.size() > 16384) break;
        maro_accessible += maro_variable.maro_name + L" (" + maro_variable.maro_type + L") = " +
            (maro_variable.maro_available ? maro_variable.maro_value : L"알 수 없음") + L"\r\n";
        for (const auto& maro_cell : maro_variable.maro_children)
            maro_accessible += maro_cell.maro_name + L" = " + (maro_cell.maro_available ? maro_cell.maro_value : L"?") + L"\r\n";
    }
    maro_SetTextChanged(GetDlgItem(maro_expandedWindow_, maro_visualizationId), maro_accessible);
    SendDlgItemMessageW(maro_expandedWindow_, maro_timelineId, TBM_SETRANGEMAX, TRUE,
        static_cast<LPARAM>(maro_history_.empty() ? 0 : maro_history_.size() - 1));
    SendDlgItemMessageW(maro_expandedWindow_, maro_timelineId, TBM_SETPOS, TRUE, static_cast<LPARAM>(maro_historyIndex_));
    EnableWindow(GetDlgItem(maro_expandedWindow_, maro_timelineId), maro_history_.size() > 1);
    InvalidateRect(GetDlgItem(maro_expandedWindow_, maro_visualizationId), nullptr, FALSE);
}

void maro_DiagnosticWindow::maro_RefreshDebug()
{
    if (maro_outputMode_ || !maro_summary_) return;
    const auto& maro_trace = maro_VisibleTrace();
    std::wstring maro_text;
    if (maro_trace.maro_line > 0)
    {
        maro_text = maro_trace.maro_function + L" · " + std::to_wstring(maro_trace.maro_line) + L"행\r\n";
        maro_text += maro_trace_.maro_waiting ? L"실행 전 일시정지 · F10 한 줄 / F9 계속\r\n" : L"마지막 관측값\r\n";
        for (const auto& maro_variable : maro_trace.maro_variables)
        {
            if (maro_text.size() > 16000) break;
            maro_text += L"\r\n" + maro_variable.maro_name + L" (" + maro_variable.maro_type + L") = " +
                (maro_variable.maro_available ? maro_variable.maro_value : L"알 수 없음");
        }
        if (!maro_trace.maro_message.empty()) maro_text += L"\r\n" + maro_trace.maro_message;
    }
    else
    {
        maro_text = L"정적 미리보기 · 실행값 아님\r\n";
        if (!maro_source_.maro_lines.empty())
        {
            const auto& maro_line = maro_source_.maro_lines[(std::min)(maro_previewLine_, maro_source_.maro_lines.size() - 1)];
            maro_text += std::to_wstring(maro_line.maro_line) + L"행 · " + maro_line.maro_text + L"\r\n" + maro_line.maro_explanation;
        }
        else maro_text += L"C/C++ 문서를 열어 주세요.";
        maro_text += L"\r\n\r\n확장하면 변수와 배열의 실행 과정을 볼 수 있습니다.";
    }
    maro_SetTextChanged(maro_summary_, maro_text);
}

void maro_DiagnosticWindow::maro_DrawVisualization(const DRAWITEMSTRUCT& maro_item)
{
    const HDC maro_dc = maro_item.hDC;
    FillRect(maro_dc, &maro_item.rcItem, maro_background_);
    SetBkMode(maro_dc, TRANSPARENT);
    const auto maro_oldFont = SelectObject(maro_dc, maro_expandedFont_ ? maro_expandedFont_ : maro_font_);
    const int maro_width = maro_item.rcItem.right - maro_item.rcItem.left;
    const int maro_height = maro_item.rcItem.bottom - maro_item.rcItem.top;
    const int maro_unit = MulDiv(20, static_cast<int>(GetDpiForWindow(maro_item.hwndItem)), 96);
    int maro_y = 4;
    const auto maro_text = [&](std::wstring_view maro_value, COLORREF maro_color, int maro_lines = 1) {
        RECT maro_rect{8, maro_y, maro_width - 8, (std::min)(maro_height, maro_y + maro_unit * maro_lines)};
        SetTextColor(maro_dc, maro_color);
        DrawTextW(maro_dc, maro_value.data(), static_cast<int>(maro_value.size()), &maro_rect,
            DT_LEFT | DT_NOPREFIX | (maro_lines > 1 ? DT_WORDBREAK : DT_SINGLELINE | DT_END_ELLIPSIS));
        maro_y += maro_unit * maro_lines;
    };
    const auto& maro_trace = maro_VisibleTrace();
    const bool maro_runtime = maro_trace.maro_line > 0;
    maro_text(maro_runtime ? L"실행 상태 · 변경값 강조 · 휠로 변수 이동" : L"선언 구조 · 아직 실행하지 않음", RGB(140, 200, 255));
    const int maro_stackReserve = maro_trace.maro_frames.empty() ? maro_unit : (std::min)(maro_unit * 5, maro_height / 3);
    const int maro_cardsBottom = maro_height - maro_stackReserve - maro_unit;
    int maro_cards = 0;
    const auto maro_previous = [&](const maro_TraceVariable& maro_variable) -> const maro_TraceVariable* {
        if (maro_historyIndex_ == 0 || maro_historyIndex_ >= maro_history_.size()) return nullptr;
        for (const auto& maro_value : maro_history_[maro_historyIndex_ - 1].maro_variables)
            if (maro_value.maro_name == maro_variable.maro_name && maro_value.maro_address == maro_variable.maro_address) return &maro_value;
        return nullptr;
    };
    const auto maro_box = [&](const RECT& maro_bounds, bool maro_changed) {
        SetDCBrushColor(maro_dc, maro_changed ? RGB(55, 66, 47) : RGB(34, 40, 48));
        FillRect(maro_dc, &maro_bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(maro_dc, maro_changed ? RGB(188, 203, 93) : RGB(87, 104, 124));
        FrameRect(maro_dc, &maro_bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    };
    const auto maro_card = [&](std::wstring maro_name, std::wstring maro_type, std::wstring maro_value, bool maro_available, bool maro_changed) {
        const RECT maro_bounds{4, maro_y, maro_width - 4, maro_y + maro_unit * 2 + 2};
        maro_box(maro_bounds, maro_changed);
        maro_text(maro_name + L" (" + maro_type + L")", RGB(235, 235, 235));
        maro_text(maro_available ? maro_value : L"값 알 수 없음", maro_available ? RGB(145, 225, 160) : RGB(185, 185, 185));
        maro_y += 6;
        ++maro_cards;
    };
    if (maro_runtime)
    {
        if (maro_visualOffset_ >= maro_trace.maro_variables.size()) maro_visualOffset_ = 0;
        for (std::size_t maro_index = maro_visualOffset_; maro_index < maro_trace.maro_variables.size(); ++maro_index)
        {
            if (maro_y + maro_unit * 2 + 6 > maro_cardsBottom) break;
            const auto& maro_variable = maro_trace.maro_variables[maro_index];
            const auto* maro_old = maro_previous(maro_variable);
            if (maro_variable.maro_children.empty())
            {
                maro_card(maro_variable.maro_name, maro_variable.maro_type, maro_variable.maro_value, maro_variable.maro_available,
                    maro_old && (maro_old->maro_value != maro_variable.maro_value || maro_old->maro_available != maro_variable.maro_available));
                continue;
            }
            maro_text(maro_variable.maro_name + L" (" + maro_variable.maro_type + L")", RGB(235, 235, 235));
            const std::size_t maro_columns = (std::max)(std::size_t{1}, maro_variable.maro_dimensions.size() == 2 ?
                maro_variable.maro_dimensions.back() : (std::min)(std::size_t{6}, maro_variable.maro_children.size()));
            const auto maro_visibleColumns = (std::min)(maro_columns, static_cast<std::size_t>((std::max)(1, (maro_width - 16) / 66)));
            const int maro_cellWidth = (std::max)(1, (maro_width - 16) / static_cast<int>((std::max)(std::size_t{1}, maro_visibleColumns)));
            const std::size_t maro_rows = (maro_variable.maro_children.size() + maro_columns - 1) / maro_columns;
            std::size_t maro_shown = 0;
            for (std::size_t maro_row = 0; maro_row < maro_rows && maro_y + maro_unit * 2 + 4 <= maro_cardsBottom; ++maro_row)
            {
                for (std::size_t maro_column = 0; maro_column < maro_visibleColumns; ++maro_column)
                {
                    const auto maro_cell = maro_row * maro_columns + maro_column;
                    if (maro_cell >= maro_variable.maro_children.size()) break;
                    const auto& maro_value = maro_variable.maro_children[maro_cell];
                    const bool maro_changed = maro_old && maro_cell < maro_old->maro_children.size() &&
                        (maro_old->maro_children[maro_cell].maro_value != maro_value.maro_value ||
                            maro_old->maro_children[maro_cell].maro_available != maro_value.maro_available);
                    const int maro_left = 8 + static_cast<int>(maro_column) * maro_cellWidth;
                    RECT maro_bounds{maro_left, maro_y, maro_left + maro_cellWidth - 4, maro_y + maro_unit * 2};
                    maro_box(maro_bounds, maro_changed);
                    RECT maro_label{maro_bounds.left + 3, maro_y, maro_bounds.right - 3, maro_y + maro_unit};
                    SetTextColor(maro_dc, RGB(150, 175, 190));
                    DrawTextW(maro_dc, maro_value.maro_name.c_str(), -1, &maro_label, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    maro_label.top += maro_unit;
                    maro_label.bottom += maro_unit;
                    SetTextColor(maro_dc, maro_value.maro_available ? RGB(145, 225, 160) : RGB(185, 185, 185));
                    DrawTextW(maro_dc, maro_value.maro_available ? maro_value.maro_value.c_str() : L"?", -1, &maro_label,
                        DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    ++maro_shown;
                }
                maro_y += maro_unit * 2 + 4;
            }
            if (maro_variable.maro_truncated || maro_shown < maro_variable.maro_children.size())
                maro_text(L"일부 셀 표시 · 관측 최대 64개", RGB(185, 185, 185));
            maro_y += 6;
            ++maro_cards;
        }
    }
    else
    {
        for (const auto& maro_variable : maro_source_.maro_items)
        {
            if (maro_y + maro_unit * 2 + 6 > maro_cardsBottom) break;
            if (maro_variable.maro_kind != maro_SourceItemKind::maro_Variable) continue;
            if (maro_variable.maro_line > maro_previewLine_ + 1) continue;
            maro_card((maro_variable.maro_line == maro_previewLine_ + 1 ? L"▶ " : L"") + maro_variable.maro_name,
                maro_variable.maro_detail, L"", false, false);
        }
    }
    if (maro_cards == 0) maro_text(maro_runtime ? L"이 위치에서 확인할 수 있는 지역 변수가 없습니다." : L"실제 추적을 시작하면 관측된 값을 표시합니다.", RGB(180, 180, 180), 2);
    if (maro_y + maro_unit < maro_height)
    {
        maro_text(L"호출 스택", RGB(140, 200, 255));
        if (maro_trace.maro_frames.empty()) maro_text(L"실행 전 · 알 수 없음", RGB(180, 180, 180));
        int maro_count = 0;
        for (const auto& maro_frame : maro_trace.maro_frames)
        {
            if (maro_y + maro_unit > maro_height || maro_count++ == 5) break;
            maro_text(L"↳ " + maro_frame.maro_function + L" · " + std::to_wstring(maro_frame.maro_line) + L"행", RGB(220, 220, 220));
        }
    }
    SelectObject(maro_dc, maro_oldFont);
}

void maro_DiagnosticWindow::maro_DrawButton(const DRAWITEMSTRUCT& maro_item)
{
    const bool maro_disabled = (maro_item.itemState & ODS_DISABLED) != 0;
    SetDCBrushColor(maro_item.hDC, (maro_item.itemState & ODS_SELECTED) ? RGB(62, 62, 62) : RGB(40, 40, 40));
    FillRect(maro_item.hDC, &maro_item.rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetTextColor(maro_item.hDC, maro_disabled ? RGB(100, 100, 100) : RGB(230, 230, 230));
    SetBkMode(maro_item.hDC, TRANSPARENT);
    wchar_t maro_text[32]{};
    GetWindowTextW(maro_item.hwndItem, maro_text, 32);
    RECT maro_rect = maro_item.rcItem;
    DrawTextW(maro_item.hDC, maro_text, -1, &maro_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if ((maro_item.itemState & ODS_FOCUS) != 0) DrawFocusRect(maro_item.hDC, &maro_rect);
}

void maro_DiagnosticWindow::maro_ExpandOutput()
{
    if (maro_expandedWindow_ || !maro_window_) return;
    WNDCLASSEXW maro_type{sizeof(maro_type)};
    maro_type.lpfnWndProc = maro_ExpandedProc;
    maro_type.hInstance = ATL::_AtlBaseModule.GetModuleInstance();
    maro_type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    maro_type.lpszClassName = L"maro_CLive_Output";
    if (!RegisterClassExW(&maro_type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    MONITORINFO maro_monitor{sizeof(maro_monitor)};
    GetMonitorInfoW(MonitorFromWindow(maro_window_, MONITOR_DEFAULTTONEAREST), &maro_monitor);
    const RECT maro_area = maro_monitor.rcWork;
    const HWND maro_created = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_APPWINDOW, maro_type.lpszClassName,
        L"CLive_Maro 실시간 출력", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        maro_area.left + 20, maro_area.top + 20, (std::max)(640L, maro_area.right - maro_area.left - 40),
        (std::max)(400L, maro_area.bottom - maro_area.top - 40), GetAncestor(maro_window_, GA_ROOT),
        nullptr, maro_type.hInstance, this);
    if (!maro_created) return;
    SetWindowTextW(GetDlgItem(maro_created, maro_inputId), maro_ReadInput(maro_window_).c_str());
    ShowWindow(maro_summary_, SW_HIDE);
    for (int maro_id : {maro_inputId, maro_stopId})
        ShowWindow(GetDlgItem(maro_window_, maro_id), SW_HIDE);
    SetWindowTextW(GetDlgItem(maro_window_, maro_expandId), L"복원");
    ShowWindow(maro_created, SW_SHOW);
    SetForegroundWindow(maro_created);
    SetFocus(GetDlgItem(maro_created, maro_sourceId));
    if (!maro_projectMode_ && maro_history_.empty() && maro_trace_.maro_line == 0 &&
        !maro_source_.maro_lines.empty() && maro_startTrace_) maro_startTrace_();
}

void maro_DiagnosticWindow::maro_CloseExpanded()
{
    if (maro_expandedWindow_) DestroyWindow(maro_expandedWindow_);
}

LRESULT CALLBACK maro_DiagnosticWindow::maro_ExpandedProc(HWND maro_window, UINT maro_message,
    WPARAM maro_wparam, LPARAM maro_lparam) noexcept
{
    auto* maro_self = reinterpret_cast<maro_DiagnosticWindow*>(GetWindowLongPtrW(maro_window, GWLP_USERDATA));
    if (maro_message == WM_NCCREATE)
    {
        maro_self = static_cast<maro_DiagnosticWindow*>(reinterpret_cast<CREATESTRUCTW*>(maro_lparam)->lpCreateParams);
        maro_self->AddRef();
        maro_self->maro_expandedWindow_ = maro_window;
        SetWindowLongPtrW(maro_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(maro_self));
    }
    if (!maro_self) return DefWindowProcW(maro_window, maro_message, maro_wparam, maro_lparam);
    if (maro_message == WM_NCDESTROY)
    {
        maro_self->maro_expandedWindow_ = nullptr;
        maro_self->maro_expandedOutput_ = nullptr;
        if (maro_self->maro_expandedFont_) DeleteObject(maro_self->maro_expandedFont_);
        maro_self->maro_expandedFont_ = nullptr;
        if (!maro_self->maro_closing_ && maro_self->maro_window_)
        {
            SetWindowTextW(maro_self->maro_summary_, maro_self->maro_output_.c_str());
            ShowWindow(maro_self->maro_summary_, SW_SHOW);
            for (int maro_id : {maro_inputId, maro_stopId})
                ShowWindow(GetDlgItem(maro_self->maro_window_, maro_id), SW_SHOW);
            SetWindowTextW(GetDlgItem(maro_self->maro_window_, maro_expandId), L"확장");
            maro_self->maro_Layout();
        }
        SetWindowLongPtrW(maro_window, GWLP_USERDATA, 0);
        maro_self->Release();
        return DefWindowProcW(maro_window, maro_message, maro_wparam, maro_lparam);
    }
    try { return maro_self->maro_HandleExpandedMessage(maro_message, maro_wparam, maro_lparam); }
    catch (...) { return maro_message == WM_CREATE ? -1 : DefWindowProcW(maro_window, maro_message, maro_wparam, maro_lparam); }
}

LRESULT maro_DiagnosticWindow::maro_HandleExpandedMessage(UINT maro_message, WPARAM maro_wparam, LPARAM maro_lparam)
{
    switch (maro_message)
    {
    case WM_CREATE:
        maro_expandedOutput_ = CreateWindowExW(0, L"EDIT", maro_output_.c_str(), WS_CHILD | WS_VISIBLE |
            WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0, 1, 1,
            maro_expandedWindow_, reinterpret_cast<HMENU>(102), ATL::_AtlBaseModule.GetModuleInstance(), nullptr);
        if (!maro_expandedOutput_ || !maro_CreateOutputControls(maro_expandedWindow_, true) ||
            !maro_CreateVisualization(maro_expandedWindow_, true)) return -1;
        SendMessageW(maro_expandedOutput_, EM_SETLIMITTEXT, maro_outputLimit, 0);
        SetWindowSubclass(maro_expandedOutput_, maro_OutputScroll, 1, 0);
        SetWindowSubclass(maro_expandedOutput_, maro_InputProc, 2, reinterpret_cast<DWORD_PTR>(this));
        maro_expandedFont_ = CreateFontW(-MulDiv(11, static_cast<int>(GetDpiForWindow(maro_expandedWindow_)), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
        maro_UpdateVisualization();
        maro_UpdateSessionControls();
        return 0;
    case WM_SIZE:
        maro_LayoutOutput(maro_expandedWindow_, HIWORD(maro_lparam));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (maro_HandleKey(maro_expandedWindow_, maro_wparam)) return 0;
        break;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(maro_lparam) == GetDlgItem(maro_expandedWindow_, maro_timelineId))
        {
            maro_SelectHistory(static_cast<std::size_t>(SendDlgItemMessageW(maro_expandedWindow_, maro_timelineId, TBM_GETPOS, 0, 0)));
            return 0;
        }
        break;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(maro_lparam)->ptMinTrackSize = {660, 520};
        return 0;
    case WM_COMMAND:
        if (maro_HandleCommand(maro_expandedWindow_, maro_wparam)) return 0;
        break;
    case WM_CLOSE:
        maro_CloseExpanded();
        return 0;
    case WM_DRAWITEM:
        if (reinterpret_cast<DRAWITEMSTRUCT*>(maro_lparam)->CtlID == maro_visualizationId)
            maro_DrawVisualization(*reinterpret_cast<DRAWITEMSTRUCT*>(maro_lparam));
        else
            maro_DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(maro_lparam));
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(maro_wparam), RGB(224, 224, 224));
        SetBkColor(reinterpret_cast<HDC>(maro_wparam), RGB(24, 24, 24));
        return reinterpret_cast<LRESULT>(maro_background_);
    case WM_ERASEBKGND:
        {
            RECT maro_bounds{};
            GetClientRect(maro_expandedWindow_, &maro_bounds);
            FillRect(reinterpret_cast<HDC>(maro_wparam), &maro_bounds, maro_background_);
        }
        return 1;
    }
    return DefWindowProcW(maro_expandedWindow_, maro_message, maro_wparam, maro_lparam);
}
