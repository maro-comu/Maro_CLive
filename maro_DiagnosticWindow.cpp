#include "maro_DiagnosticWindow.hpp"

#include <algorithm>
#include <commctrl.h>
#include <sstream>
#include <windowsx.h>

namespace
{
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
    *size = {340, 700};
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
    if (message != nullptr && message->message == WM_KEYDOWN && message->wParam == 'A' &&
        GetKeyState(VK_CONTROL) < 0 && (message->hwnd == maro_summary_ || message->hwnd == maro_issues_))
    {
        SendMessageW(message->hwnd, EM_SETSEL, 0, -1);
        return S_OK;
    }
    return S_FALSE;
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
        }
        if (diagnostic.range.generated && !diagnostic.sourcePath.empty())
        {
            details << L"\r\n" << diagnostic.sourcePath;
        }
        details << L"\r\n" << diagnostic.friendlyMessage << L"\r\n\r\n";
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
    if (text.empty())
    {
        return;
    }
    constexpr std::size_t limit = 64 * 1024;
    std::wstring appended;
    appended.reserve((std::min)(text.size(), limit));
    wchar_t previous = maro_output_.empty() ? L'\0' : maro_output_.back();
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
    if (maro_output_.size() > limit)
    {
        removed = maro_output_.size() - limit;
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
    if (maro_summary_ == nullptr)
    {
        return;
    }
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    SendMessageW(maro_summary_, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
    const bool follow = selectionStart == oldLength && selectionEnd == oldLength;
    const LRESULT firstLine = SendMessageW(maro_summary_, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (removed != 0)
    {
        SetWindowTextW(maro_summary_, maro_output_.c_str());
    }
    else
    {
        SendMessageW(maro_summary_, EM_SETSEL, oldLength, oldLength);
        SendMessageW(maro_summary_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(appended.c_str()));
    }
    if (follow)
    {
        SendMessageW(maro_summary_, EM_SETSEL, maro_output_.size(), maro_output_.size());
        SendMessageW(maro_summary_, EM_SCROLLCARET, 0, 0);
    }
    else
    {
        SendMessageW(maro_summary_, EM_SETSEL,
            selectionStart > removed ? selectionStart - removed : 0,
            selectionEnd > removed ? selectionEnd - removed : 0);
        SendMessageW(maro_summary_, EM_LINESCROLL, 0,
            firstLine - SendMessageW(maro_summary_, EM_GETFIRSTVISIBLELINE, 0, 0));
    }
}

void maro_DiagnosticWindow::maro_ClearOutput()
{
    maro_output_.clear();
    if (maro_summary_ != nullptr)
    {
        SetWindowTextW(maro_summary_, L"");
    }
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
    SetWindowTextW(maro_issuesLabel_, maro_counts_.c_str());
    SetWindowTextW(maro_issues_, details.c_str());
}

bool maro_DiagnosticWindow::maro_CreateControls()
{
    const auto create = [this](const wchar_t* type, const wchar_t* caption, DWORD style, int id) {
        return CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, maro_window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            ATL::_AtlBaseModule.GetModuleInstance(), nullptr);
    };
    maro_background_ = CreateSolidBrush(RGB(24, 24, 24));
    maro_summaryLabel_ = create(L"STATIC", L"실시간 출력", SS_LEFT, 101);
    maro_summary_ = create(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 102);
    maro_issuesLabel_ = create(L"STATIC", L"실시간 진단", SS_LEFT, 103);
    maro_issues_ = create(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 104);
    if (!maro_background_ || !maro_summaryLabel_ || !maro_summary_ || !maro_issuesLabel_ || !maro_issues_)
    {
        return false;
    }
    SendMessageW(maro_summary_, EM_SETLIMITTEXT, 64 * 1024, 0);
    SendMessageW(maro_issues_, EM_SETLIMITTEXT, 1024 * 1024, 0);
    if (!SetWindowSubclass(maro_summary_, maro_OutputScroll, 1, 0) ||
        !SetWindowSubclass(maro_issues_, maro_OutputScroll, 1, 0))
    {
        return false;
    }
    SetWindowTextW(maro_summary_, maro_output_.c_str());
    maro_UpdateFont();
    maro_Refresh();
    maro_Layout();
    return true;
}

void maro_DiagnosticWindow::maro_UpdateFont()
{
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, GetDpiForWindow(maro_window_)))
    {
        return;
    }
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
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
    maro_splitY_ = height * maro_split_ / 100;
    MoveWindow(maro_summaryLabel_, pad, pad, width, label, TRUE);
    MoveWindow(maro_summary_, pad, pad + label, width, (std::max)(1, maro_splitY_ - label - 2 * pad), TRUE);
    MoveWindow(maro_issuesLabel_, pad, maro_splitY_ + pad, width, label, TRUE);
    MoveWindow(maro_issues_, pad, maro_splitY_ + pad + label, width,
        (std::max)(1, height - maro_splitY_ - label - 2 * pad), TRUE);
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
        if (abs(GET_Y_LPARAM(lparam) - maro_splitY_) <= 8)
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
        if (GetCapture() == maro_window_ || abs(GET_Y_LPARAM(lparam) - maro_splitY_) <= 8)
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
}
