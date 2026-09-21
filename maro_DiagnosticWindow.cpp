#include "maro_DiagnosticWindow.hpp"
#include "maro_DiagnosticLinks.hpp"

#include <algorithm>
#include <commctrl.h>
#include <sstream>
#include <windowsx.h>
#include <richedit.h>

namespace
{
constexpr int maro_inputId = 106;
constexpr int maro_sendId = 107;
constexpr int maro_eofId = 108;
constexpr int maro_stopId = 109;
constexpr int maro_projectId = 110;
constexpr int maro_automaticId = 111;
constexpr int maro_encodingId = 112;
constexpr int maro_hintId = 124;
constexpr std::size_t maro_outputLimit = 64 * 1024;

std::wstring maro_WithoutDirectories(std::wstring maro_text)
{
    for (std::size_t maro_index = 0; maro_index + 2 < maro_text.size(); ++maro_index)
    {
        const bool maro_drive = ((maro_text[maro_index] >= L'A' && maro_text[maro_index] <= L'Z') ||
            (maro_text[maro_index] >= L'a' && maro_text[maro_index] <= L'z')) && maro_text[maro_index + 1] == L':' &&
            (maro_text[maro_index + 2] == L'\\' || maro_text[maro_index + 2] == L'/');
        const bool maro_unc = maro_text[maro_index] == L'\\' && maro_text[maro_index + 1] == L'\\';
        if (!maro_drive && !maro_unc) continue;
        const wchar_t maro_quote = maro_index > 0 && (maro_text[maro_index - 1] == L'\'' || maro_text[maro_index - 1] == L'"')
            ? maro_text[maro_index - 1] : 0;
        auto maro_end = maro_index + 2;
        while (maro_end < maro_text.size() && maro_text[maro_end] != L'\r' && maro_text[maro_end] != L'\n' &&
            (maro_quote ? maro_text[maro_end] != maro_quote : maro_text[maro_end] != L' ' && maro_text[maro_end] != L'\t')) ++maro_end;
        const auto maro_slash = maro_text.find_last_of(L"\\/", maro_end - 1);
        if (maro_slash != std::wstring::npos && maro_slash >= maro_index)
            maro_text.erase(maro_index, maro_slash + 1 - maro_index);
    }
    return maro_text;
}

std::wstring maro_DiagnosticText(std::wstring maro_text)
{
    maro_text = maro_WithoutDirectories(std::move(maro_text));
    std::size_t maro_written = 0;
    for (std::size_t maro_index = 0; maro_index < maro_text.size(); ++maro_index)
    {
        const auto maro_character = maro_text[maro_index];
        if (maro_character == L'\r' && maro_index + 1 < maro_text.size() && maro_text[maro_index + 1] == L'\n') ++maro_index;
        maro_text[maro_written++] = maro_character == L'\n' ? L'\r' : maro_character;
    }
    maro_text.resize(maro_written);
    return maro_text;
}

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
        message->hwnd == GetDlgItem(maro_window_, maro_inputId))
    {
        maro_SubmitInput(GetParent(message->hwnd));
        return S_OK;
    }
    if (message != nullptr && message->message == WM_KEYDOWN && message->wParam == 'A' &&
        GetKeyState(VK_CONTROL) < 0 && (message->hwnd == maro_summary_ || message->hwnd == maro_issues_))
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

void maro_DiagnosticWindow::maro_SetRunCallback(std::function<void()> maro_run)
{
    maro_runCallback_ = std::move(maro_run);
}

void maro_DiagnosticWindow::maro_SetFixCallback(std::function<void(const Maro_Diagnostic&)> maro_fix)
{
    maro_applyFix_ = std::move(maro_fix);
}

void maro_DiagnosticWindow::maro_SetNavigateCallback(std::function<void(const Maro_Diagnostic&)> maro_navigate)
{
    maro_navigate_ = std::move(maro_navigate);
}

void maro_DiagnosticWindow::maro_SetDocumentationCallback(std::function<void(const Maro_Diagnostic&)> maro_documentation)
{
    maro_documentation_ = std::move(maro_documentation);
}

std::optional<std::size_t> maro_DiagnosticWindow::maro_HitLink(POINT maro_point) const
{
    if (!maro_issues_) return std::nullopt;
    RECT maro_bounds{};
    GetClientRect(maro_issues_, &maro_bounds);
    if (!PtInRect(&maro_bounds, maro_point)) return std::nullopt;
    POINTL maro_at{maro_point.x, maro_point.y};
    const auto maro_nearest = static_cast<long>(SendMessageW(maro_issues_, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&maro_at)));
    for (const auto maro_character : {maro_nearest, maro_nearest - 1})
    {
      for (std::size_t maro_index = 0; maro_index < maro_links_.size(); ++maro_index)
      {
        const auto& maro_link = maro_links_[maro_index];
        if (maro_character < maro_link.maro_start || maro_character >= maro_link.maro_end) continue;
        POINTL maro_origin{};
        SendMessageW(maro_issues_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_origin), maro_character);
        wchar_t maro_text[3]{};
        TEXTRANGEW maro_range{{maro_character, maro_character + 1}, maro_text};
        SendMessageW(maro_issues_, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&maro_range));
        HDC maro_dc = GetDC(maro_issues_);
        const auto maro_old = SelectObject(maro_dc, maro_font_);
        SIZE maro_size{};
        GetTextExtentPoint32W(maro_dc, maro_text, 1, &maro_size);
        SelectObject(maro_dc, maro_old);
        ReleaseDC(maro_issues_, maro_dc);
        POINTL maro_next{};
        SendMessageW(maro_issues_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_next), maro_character + 1);
        const auto maro_right = maro_next.y == maro_origin.y && maro_next.x > maro_origin.x ?
            maro_next.x : maro_origin.x + (std::max)(1L, maro_size.cx);
        if (maro_point.x >= maro_origin.x && maro_point.x < maro_right &&
            maro_point.y >= maro_origin.y && maro_point.y < maro_origin.y + (std::max)(1L, maro_size.cy)) return maro_index;
      }
    }
    return std::nullopt;
}

void maro_DiagnosticWindow::maro_InvokeLink(std::size_t maro_index)
{
    if (maro_index >= maro_links_.size()) return;
    const auto maro_link = maro_links_[maro_index];
    const auto maro_callback = maro_link.maro_kind == maro_LinkKind::maro_Fix ? maro_applyFix_ :
        maro_link.maro_kind == maro_LinkKind::maro_Navigate ? maro_navigate_ : maro_documentation_;
    if (!maro_callback) return;
    if (maro_link.maro_kind == maro_LinkKind::maro_Fix) maro_links_.clear();
    maro_pressedLink_.reset();
    if (maro_issues_ && GetCapture() == maro_issues_) ReleaseCapture();
    maro_callback(maro_link.maro_diagnostic);
}

void maro_DiagnosticWindow::maro_SetSessionState(bool maro_running, bool maro_inputOpen)
{
    if (maro_running_ == maro_running && maro_inputOpen_ == (maro_running && maro_inputOpen)) return;
    if (maro_running && !maro_running_) maro_inputSubmitted_ = false;
    maro_running_ = maro_running;
    maro_inputOpen_ = maro_running && maro_inputOpen;
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
    std::size_t maro_hash = maro_source.maro_lines.size();
    maro_HashText(maro_hash, maro_source.maro_path);
    for (const auto& maro_line : maro_source.maro_lines)
        maro_HashText(maro_hash, maro_line.maro_text);
    if (maro_sourceHash_ == maro_hash && maro_source_.maro_path == maro_source.maro_path &&
        maro_source_.maro_lines.size() == maro_source.maro_lines.size()) return;
    maro_sourceHash_ = maro_hash;
    maro_source_ = std::move(maro_source);
    maro_inputSubmitted_ = false;
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
    if (!maro_outputMode_ || !maro_window_) return;
    const bool maro_canInput = maro_CanInput();
    EnableWindow(GetDlgItem(maro_window_, maro_inputId), maro_canInput);
    ShowWindow(GetDlgItem(maro_window_, maro_inputId), maro_canInput ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(maro_window_, maro_stopId), static_cast<bool>(maro_toggleAutomatic_) || static_cast<bool>(maro_stopRun_));
    maro_SetTextChanged(GetDlgItem(maro_window_, maro_stopId), maro_automatic_ ? L"끄기" : L"켜기");
    const std::wstring maro_encoding = maro_encoding_ == 65001 ? L"UTF-8" : maro_encoding_ == 949 ? L"CP949" :
        maro_encoding_ == 1252 ? L"CP1252" : L"자동";
    const std::wstring maro_hint = (maro_projectMode_ ? L"프로젝트" : L"현재 파일") + std::wstring(L" · ") + maro_encoding +
        L" · F5 실행 · Ctrl+P 모드 · Ctrl+E 인코딩";
    maro_SetTextChanged(GetDlgItem(maro_window_, maro_hintId), maro_hint);
    RECT maro_bounds{};
    GetClientRect(maro_window_, &maro_bounds);
    maro_LayoutOutput(maro_window_, maro_bounds.bottom);
}

bool maro_DiagnosticWindow::maro_CanInput() const
{
    if (!maro_outputMode_ || !maro_running_ || !maro_inputOpen_ || !maro_submitInput_) return false;
    if (maro_projectMode_) return true;
    if (maro_source_.maro_inputPolicy == maro_InputPolicy::maro_None) return false;
    return maro_source_.maro_inputPolicy != maro_InputPolicy::maro_SingleLine || !maro_inputSubmitted_;
}

bool maro_DiagnosticWindow::maro_HandleKey(HWND maro_parent, WPARAM maro_key)
{
    if (!maro_outputMode_) return false;
    if (maro_key == VK_F5 && maro_runCallback_)
    {
        maro_runCallback_();
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

void maro_DiagnosticWindow::maro_SubmitInput(HWND maro_parent)
{
    if (!maro_CanInput()) return;
    const std::wstring maro_text = maro_ReadInput(maro_parent);
    if (maro_submitInput_(maro_text))
    {
        if (maro_source_.maro_inputPolicy == maro_InputPolicy::maro_SingleLine &&
            (maro_source_.maro_inputAcceptsEmptyLine || maro_text.find_first_not_of(L" \t\r\n\v\f") != std::wstring::npos))
            maro_inputSubmitted_ = true;
        SetWindowTextW(GetDlgItem(maro_window_, maro_inputId), L"");
        maro_UpdateSessionControls();
    }
}

bool maro_DiagnosticWindow::maro_HandleCommand(HWND maro_parent, WPARAM maro_command)
{
    if (!maro_outputMode_) return false;
    const auto maro_id = LOWORD(maro_command);
    if (maro_id == maro_encodingId && HIWORD(maro_command) == CBN_SELCHANGE)
    {
        const auto maro_encoding = maro_EncodingAt(SendDlgItemMessageW(maro_parent, maro_encodingId, CB_GETCURSEL, 0, 0));
        maro_SetEncoding(maro_encoding);
        if (maro_changeEncoding_) maro_changeEncoding_(maro_encoding);
        return true;
    }
    if (HIWORD(maro_command) != BN_CLICKED) return false;
    switch (maro_id)
    {
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
        if (maro_window == maro_instance->maro_issues_)
        {
            if (maro_message == WM_LBUTTONDOWN)
            {
                maro_instance->maro_pressedLink_ = maro_instance->maro_HitLink({GET_X_LPARAM(maro_lparam), GET_Y_LPARAM(maro_lparam)});
                if (maro_instance->maro_pressedLink_) SetCapture(maro_window);
                else if (GetCapture() == maro_window) ReleaseCapture();
                return 0;
            }
            if (maro_message == WM_LBUTTONUP)
            {
                const auto maro_hit = maro_instance->maro_HitLink({GET_X_LPARAM(maro_lparam), GET_Y_LPARAM(maro_lparam)});
                const auto maro_pressed = maro_instance->maro_pressedLink_;
                const bool maro_captured = GetCapture() == maro_window;
                maro_instance->maro_pressedLink_.reset();
                if (maro_captured) ReleaseCapture();
                if (maro_captured && maro_hit && maro_pressed == maro_hit) maro_instance->maro_InvokeLink(*maro_hit);
                return 0;
            }
            if (maro_message == WM_CAPTURECHANGED || maro_message == WM_CANCELMODE ||
                (maro_message == WM_MOUSEMOVE && maro_instance->maro_pressedLink_ &&
                    (!(maro_wparam & MK_LBUTTON) || maro_instance->maro_pressedLink_ !=
                        maro_instance->maro_HitLink({GET_X_LPARAM(maro_lparam), GET_Y_LPARAM(maro_lparam)}))))
            {
                maro_instance->maro_pressedLink_.reset();
                if (GetCapture() == maro_window) ReleaseCapture();
                return 0;
            }
            if (maro_message == WM_LBUTTONDBLCLK) return 0;
            if (maro_message == WM_SETCURSOR)
            {
                POINT maro_point{};
                GetCursorPos(&maro_point);
                ScreenToClient(maro_window, &maro_point);
                SetCursor(LoadCursorW(nullptr, maro_instance->maro_HitLink(maro_point) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
        }
        if (maro_message == WM_GETDLGCODE && maro_wparam == VK_RETURN) return DLGC_WANTALLKEYS;
        if ((maro_message == WM_KEYDOWN || maro_message == WM_SYSKEYDOWN) &&
            maro_instance->maro_HandleKey(GetParent(maro_window), maro_wparam)) return 0;
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
    maro_pressedLink_.reset();
    if (maro_issues_ && GetCapture() == maro_issues_) ReleaseCapture();
    maro_path_ = std::move(path);
    maro_status_ = std::move(status);
    maro_details_.clear();
    maro_links_.clear();
    maro_notice_.clear();
    maro_counts_ = L"실시간 진단";
    maro_Refresh();
}

void maro_DiagnosticWindow::maro_SetResult(const Maro_ResultEnvelope& result)
{
    maro_pressedLink_.reset();
    if (maro_issues_ && GetCapture() == maro_issues_) ReleaseCapture();
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::wostringstream details;
    maro_links_.clear();
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
        std::wstring maro_message = maro_DiagnosticText(diagnostic.friendlyMessage);
        const auto maro_break = maro_message.find_first_of(L"\r\n");
        if (diagnostic.code.empty())
        {
            details << maro_message.substr(0, maro_break);
            maro_message = maro_break == std::wstring::npos ? L"" : maro_message.substr(maro_break);
            while (!maro_message.empty() && (maro_message.front() == L'\r' || maro_message.front() == L'\n'))
                maro_message.erase(0, 1);
        }
        else
        {
            const auto maro_start = static_cast<long>(details.tellp());
            details << diagnostic.code;
            if (maro_documentation_ && !maro_DiagnosticDocumentation(diagnostic.code).empty())
                maro_links_.push_back({maro_start, static_cast<long>(details.tellp()), maro_LinkKind::maro_Documentation, diagnostic});
            details << L" · " << (error ? L"오류" : L"경고");
        }
        if (diagnostic.range.start.line != 0)
        {
            details << L" · ";
            const auto maro_start = static_cast<long>(details.tellp());
            details << diagnostic.range.start.line << L"행";
            if (diagnostic.range.start.column != 0) details << L" " << diagnostic.range.start.column << L"열";
            if (maro_navigate_ && !diagnostic.range.generated && !diagnostic.sourcePath.empty())
                maro_links_.push_back({maro_start, static_cast<long>(details.tellp()), maro_LinkKind::maro_Navigate, diagnostic});
        }
        details << L"\r";
        const bool maro_hasFix = diagnostic.fix && !diagnostic.fix->edits.empty();
        if (maro_hasFix && !diagnostic.fix->description.empty())
            maro_message = maro_DiagnosticText(diagnostic.fix->description);
        if (!maro_hasFix && !maro_message.empty()) details << L"직접 수정: ";
        const auto maro_start = static_cast<long>(details.tellp());
        details << maro_message;
        if (maro_hasFix && maro_applyFix_ && !maro_message.empty())
            maro_links_.push_back({maro_start, static_cast<long>(details.tellp()), maro_LinkKind::maro_Fix, diagnostic});
        if (diagnostic.maro_relatedCount != 0)
            details << L"\r연관 진단 " << diagnostic.maro_relatedCount << L"개 접음";
        details << L"\r\r";
    }
    maro_status_ = result.statusText;
    maro_counts_ = L"실시간 진단 · 오류 " + std::to_wstring(errors) + L" · 경고 " + std::to_wstring(warnings);
    maro_details_ = details.str();
    if (maro_details_.empty())
    {
        maro_details_ = result.status == Maro_Status::Success ? L"문제 없음." : result.statusText;
    }
    maro_Refresh();
}

void maro_DiagnosticWindow::maro_SetNotice(std::wstring text)
{
    maro_notice_ = std::move(text);
    maro_Refresh();
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
    maro_UpdateOutput(maro_summary_, appended, oldLength, removed);
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
    maro_inputSubmitted_ = false;
    maro_output_.clear();
    if (maro_summary_ != nullptr && maro_outputMode_)
    {
        SetWindowTextW(maro_summary_, L"");
    }
    maro_UpdateSessionControls();
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
        details = maro_WithoutDirectories(maro_status_);
    }
    if (!maro_notice_.empty())
    {
        details += L"\r\r" + maro_WithoutDirectories(maro_notice_);
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
    if (maro_issues_)
    {
        CHARRANGE maro_all{0, -1};
        SendMessageW(maro_issues_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&maro_all));
        CHARFORMAT2W maro_format{sizeof(maro_format)};
        maro_format.dwMask = CFM_COLOR | CFM_LINK | CFM_UNDERLINE;
        maro_format.crTextColor = RGB(224, 224, 224);
        SendMessageW(maro_issues_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&maro_format));
        maro_format.dwEffects = CFE_LINK | CFE_UNDERLINE;
        maro_format.crTextColor = RGB(125, 210, 255);
        for (const auto& maro_link : maro_links_)
        {
            CHARRANGE maro_range{maro_link.maro_start, maro_link.maro_end};
            SendMessageW(maro_issues_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&maro_range));
            SendMessageW(maro_issues_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&maro_format));
        }
        CHARRANGE maro_empty{0, 0};
        SendMessageW(maro_issues_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&maro_empty));
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
    if (maro_outputMode_)
    {
        maro_summaryLabel_ = create(L"STATIC", L"실시간 출력", SS_LEFT, 101);
        maro_summary_ = create(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 102);
        if (!maro_summaryLabel_ || !maro_summary_ || !maro_CreateOutputControls(maro_window_)) return false;
    }
    else
    {
        static const HMODULE maro_richEdit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!maro_richEdit) return false;
        maro_issuesLabel_ = create(L"STATIC", L"실시간 진단", SS_LEFT, 103);
        maro_issues_ = create(MSFTEDIT_CLASS, L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 104);
        SendMessageW(maro_issues_, EM_SETBKGNDCOLOR, 0, RGB(24, 24, 24));
        SendMessageW(maro_issues_, EM_SETEVENTMASK, 0, ENM_LINK);
    }
    if (!maro_background_ || (!maro_outputMode_ && (!maro_issuesLabel_ || !maro_issues_)))
    {
        return false;
    }
    SendMessageW(maro_summary_, EM_SETLIMITTEXT, 64 * 1024, 0);
    SendMessageW(maro_issues_, EM_SETLIMITTEXT, 1024 * 1024, 0);
    if ((maro_summary_ && !SetWindowSubclass(maro_summary_, maro_OutputScroll, 1, 0)) ||
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
    if (maro_window_ == nullptr)
    {
        return;
    }
    RECT bounds{};
    GetClientRect(maro_window_, &bounds);
    const int pad = MulDiv(8, GetDpiForWindow(maro_window_), 96);
    const int label = MulDiv(24, GetDpiForWindow(maro_window_), 96);
    const int width = (std::max)(1L, bounds.right - 2 * pad);
    const int height = (std::max)(1L, bounds.bottom);
    if (maro_outputMode_) maro_LayoutOutput(maro_window_, height);
    if (!maro_outputMode_)
    {
        MoveWindow(maro_issuesLabel_, pad, pad, width, label, TRUE);
        MoveWindow(maro_issues_, pad, pad + label, width, (std::max)(1, height - label - 2 * pad), TRUE);
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
    case WM_NOTIFY:
        if (const auto* maro_header = reinterpret_cast<NMHDR*>(lparam);
            maro_header && maro_header->hwndFrom == maro_issues_ && maro_header->code == EN_LINK)
            return 1;
        break;
    case WM_DRAWITEM:
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
        SetFocus(maro_outputMode_ ? maro_summary_ : maro_issues_);
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

bool maro_DiagnosticWindow::maro_CreateOutputControls(HWND maro_parent)
{
    const auto maro_instance = ATL::_AtlBaseModule.GetModuleInstance();
    const HWND maro_stop = CreateWindowExW(0, L"BUTTON", L"끄기", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 1, 1, maro_parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(maro_stopId)), maro_instance, nullptr);
    if (!maro_stop) return false;
    SetWindowSubclass(maro_stop, maro_InputProc, 2, reinterpret_cast<DWORD_PTR>(this));
    const HWND maro_input = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 1, 1, maro_parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(maro_inputId)), maro_instance, nullptr);
    if (!maro_input) return false;
    SendMessageW(maro_input, EM_SETLIMITTEXT, 4096, 0);
    SendMessageW(maro_input, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"입력 후 Enter · 입력 종료 Ctrl+D"));
    SetWindowSubclass(maro_input, maro_InputProc, 2, reinterpret_cast<DWORD_PTR>(this));
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
    const int maro_button = (std::max)(32, (std::min)(70, maro_width / 4));
    MoveWindow(GetDlgItem(maro_parent, maro_stopId), maro_bounds.right - maro_pad - maro_button,
        maro_pad, maro_button, maro_unit, TRUE);
    MoveWindow(GetDlgItem(maro_parent, 101), maro_pad, maro_pad,
        (std::max)(1, maro_width - maro_button - 4), maro_unit, TRUE);
    const int maro_hintY = maro_pad + maro_unit + 2;
    const int maro_hintHeight = MulDiv(20, static_cast<int>(GetDpiForWindow(maro_parent)), 96);
    MoveWindow(GetDlgItem(maro_parent, maro_hintId), maro_pad, maro_hintY, maro_width, maro_hintHeight, TRUE);
    const int maro_outputY = maro_hintY + maro_hintHeight + 4;
    const bool maro_inputVisible = maro_CanInput();
    const int maro_inputHeight = maro_inputVisible ? maro_unit + 4 : 0;
    const int maro_inputY = (std::max)(maro_outputY + 1, maro_bottom - maro_inputHeight - maro_pad);
    MoveWindow(maro_summary_, maro_pad, maro_outputY, maro_width,
        (std::max)(1, maro_inputY - maro_outputY - 4), TRUE);
    MoveWindow(GetDlgItem(maro_parent, maro_inputId), maro_pad, maro_inputY, maro_width, maro_unit, TRUE);
    ShowWindow(GetDlgItem(maro_parent, maro_inputId), maro_inputVisible ? SW_SHOW : SW_HIDE);
    for (int maro_id : {101, 102, maro_inputId, maro_stopId, maro_hintId})
        SendMessageW(GetDlgItem(maro_parent, maro_id), WM_SETFONT, reinterpret_cast<WPARAM>(maro_font_), TRUE);
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
