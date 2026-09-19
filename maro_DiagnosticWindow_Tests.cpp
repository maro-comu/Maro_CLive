#include "maro_DiagnosticWindow.hpp"

#include <commctrl.h>
#include <richedit.h>
#include <iostream>

namespace
{
class maro_TestModule : public ATL::CAtlExeModuleT<maro_TestModule> {};
maro_TestModule maro_module;

HWND maro_TestExpandedWindow()
{
    HWND maro_result = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND maro_window, LPARAM maro_data) -> BOOL {
        wchar_t maro_name[64]{};
        GetClassNameW(maro_window, maro_name, 64);
        if (std::wstring_view(maro_name) != L"maro_CLive_Output") return TRUE;
        *reinterpret_cast<HWND*>(maro_data) = maro_window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&maro_result));
    return maro_result;
}

std::wstring maro_ControlText(HWND maro_control)
{
    std::wstring maro_text(static_cast<std::size_t>(GetWindowTextLengthW(maro_control)) + 1, L'\0');
    maro_text.resize(GetWindowTextW(maro_control, maro_text.data(), static_cast<int>(maro_text.size())));
    return maro_text;
}

bool maro_TwoButtons(HWND maro_window)
{
    unsigned maro_count = 0;
    for (HWND maro_child = GetWindow(maro_window, GW_CHILD); maro_child; maro_child = GetWindow(maro_child, GW_HWNDNEXT))
    {
        wchar_t maro_class[64]{};
        GetClassNameW(maro_child, maro_class, 64);
        if (_wcsicmp(maro_class, L"BUTTON") == 0) ++maro_count;
    }
    return maro_count == 2 && GetDlgItem(maro_window, 105) && GetDlgItem(maro_window, 109);
}

bool maro_ControlVisible(HWND maro_window, int maro_id)
{
    return (GetWindowLongPtrW(GetDlgItem(maro_window, maro_id), GWL_STYLE) & WS_VISIBLE) != 0;
}

void maro_Key(HWND maro_control, UINT maro_key, BYTE maro_modifier = 0)
{
    BYTE maro_saved[256]{}, maro_keys[256]{};
    GetKeyboardState(maro_saved);
    if (maro_modifier) maro_keys[maro_modifier] = 0x80;
    SetKeyboardState(maro_keys);
    SendMessageW(maro_control, maro_modifier == VK_MENU ? WM_SYSKEYDOWN : WM_KEYDOWN, maro_key, 0);
    SetKeyboardState(maro_saved);
}
}

bool maro_TestDiagnosticWindow()
{
    const HRESULT maro_apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool maro_passed = true;
    const auto maro_check = [&](bool maro_condition, const char* maro_name) {
        if (!maro_condition) std::cerr << "[FAIL] pane: " << maro_name << '\n';
        maro_passed = maro_condition && maro_passed;
    };
    const HWND maro_parent = CreateWindowExW(0, L"STATIC", L"maro_test", WS_OVERLAPPEDWINDOW,
        0, 0, 1000, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ATL::CComObject<maro_DiagnosticWindow>* maro_debugRaw = nullptr;
    ATL::CComObject<maro_DiagnosticWindow>* maro_outputRaw = nullptr;
    maro_check(maro_parent && SUCCEEDED(ATL::CComObject<maro_DiagnosticWindow>::CreateInstance(&maro_debugRaw)) &&
        SUCCEEDED(ATL::CComObject<maro_DiagnosticWindow>::CreateInstance(&maro_outputRaw)), "create instances");
    if (maro_debugRaw && maro_outputRaw)
    {
        ATL::CComPtr<maro_DiagnosticWindow> maro_debug = maro_debugRaw;
        ATL::CComPtr<maro_DiagnosticWindow> maro_output = maro_outputRaw;
        maro_output->maro_SetOutputMode(true);
        HWND maro_debugWindow = nullptr, maro_outputWindow = nullptr;
        maro_check(SUCCEEDED(maro_debug->CreatePaneWindow(maro_parent, 0, 0, 340, 700, &maro_debugWindow)) &&
            SUCCEEDED(maro_output->CreatePaneWindow(maro_parent, 340, 0, 650, 260, &maro_outputWindow)), "create panes");
        const HWND maro_summary = GetDlgItem(maro_outputWindow, 102);
        const HWND maro_issues = GetDlgItem(maro_debugWindow, 104);
        maro_check(maro_TwoButtons(maro_outputWindow) && !GetDlgItem(maro_outputWindow, 104) &&
            !GetDlgItem(maro_debugWindow, 101) && !GetDlgItem(maro_debugWindow, 102) &&
            !GetDlgItem(maro_debugWindow, 105) && !GetDlgItem(maro_debugWindow, 106) &&
            !GetDlgItem(maro_debugWindow, 109) && !GetDlgItem(maro_debugWindow, 124) &&
            GetDlgItem(maro_debugWindow, 103) && maro_issues, "diagnostics only with no debug controls");
        RECT maro_labelBounds{}, maro_issueBounds{};
        GetWindowRect(GetDlgItem(maro_debugWindow, 103), &maro_labelBounds);
        GetWindowRect(maro_issues, &maro_issueBounds);
        maro_check(maro_labelBounds.top < maro_issueBounds.top &&
            maro_issueBounds.top - maro_labelBounds.bottom <= 12, "diagnostic heading is at the top");
        maro_check((GetWindowLongPtrW(maro_summary, GWL_STYLE) & ES_READONLY) != 0 &&
            (GetWindowLongPtrW(maro_summary, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL | WS_BORDER)) == 0,
            "borderless readonly output");
        const HDC maro_colors = CreateCompatibleDC(nullptr);
        const auto maro_brush = reinterpret_cast<HBRUSH>(SendMessageW(maro_debugWindow, WM_CTLCOLORSTATIC,
            reinterpret_cast<WPARAM>(maro_colors), reinterpret_cast<LPARAM>(maro_issues)));
        LOGBRUSH maro_background{};
        maro_check(GetObjectW(maro_brush, sizeof(maro_background), &maro_background) &&
            maro_background.lbColor == RGB(24, 24, 24) && GetTextColor(maro_colors) == RGB(224, 224, 224), "dark theme");
        DeleteDC(maro_colors);
        maro_output->maro_AppendOutput(L"first\nsecond\r");
        maro_output->maro_AppendOutput(L"\n");
        const std::wstring maro_expected = L"first\r\nsecond\r\n";
        maro_debug->maro_AppendOutput(L"must not be debug output");
        maro_check(maro_ControlText(maro_summary) == maro_expected &&
            maro_ControlText(GetDlgItem(maro_debugWindow, 102)).find(L"must not") == std::wstring::npos, "stdout isolated");
        Maro_ResultEnvelope maro_result;
        maro_result.status = Maro_Status::CompileFailed;
        Maro_Diagnostic maro_error;
        maro_error.severity = Maro_Severity::Error;
        maro_error.range.start.line = 3;
        maro_error.range.start.column = 31;
        maro_error.code = L"C2143";
        maro_error.sourcePath = L"C:\\private\\long_directory\\maro_test.c";
        maro_error.originalDiagnostic = L"raw compiler diagnostic C:\\private\\long_directory\\maro_test.c(4)";
        maro_error.friendlyMessage = L"문장 끝에 ';'가 필요합니다.";
        maro_error.maro_relatedCount = 2;
        maro_error.fix = Maro_FixSuggestion{L"세미콜론 추가", {{17, 30, 0, L";", L""}}};
        int maro_fixes = 0;
        Maro_Diagnostic maro_clicked;
        maro_debug->maro_SetFixCallback([&](const Maro_Diagnostic& maro_diagnostic) {
            ++maro_fixes;
            maro_clicked = maro_diagnostic;
        });
        maro_result.diagnostics.push_back(maro_error);
        maro_debug->maro_SetResult(maro_result);
        maro_check(maro_ControlText(maro_issues).starts_with(L"C2143 · 오류 · 3행 31열") &&
            maro_ControlText(maro_issues).find(L"연관 진단 2개 접음") != std::wstring::npos &&
            maro_ControlText(maro_issues).find(L"문장 끝에 ';'가 필요합니다.") != std::wstring::npos &&
            maro_ControlText(maro_issues).find(L"private") == std::wstring::npos &&
            maro_ControlText(maro_issues).find(L"raw compiler") == std::wstring::npos &&
            maro_ControlText(GetDlgItem(maro_debugWindow, 103)) == L"실시간 진단 · 오류 1 · 경고 0" &&
            maro_ControlText(maro_summary) == maro_expected, "root error precise line and independent output");
        ENLINK maro_link{};
        maro_link.nmhdr.hwndFrom = maro_issues;
        maro_link.nmhdr.idFrom = 104;
        maro_link.nmhdr.code = EN_LINK;
        maro_link.msg = WM_LBUTTONUP;
        maro_link.chrg = {0, 5};
        SendMessageW(maro_debugWindow, WM_NOTIFY, 104, reinterpret_cast<LPARAM>(&maro_link));
        maro_check(maro_fixes == 0, "only an exact solution link can request a fix");
        maro_link.chrg.cpMin = static_cast<LONG>(std::wstring_view(L"C2143 · 오류 · 3행 31열\r").size());
        maro_link.chrg.cpMax = maro_link.chrg.cpMin + static_cast<LONG>(maro_error.friendlyMessage.size());
        SendMessageW(maro_issues, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&maro_link.chrg));
        CHARFORMAT2W maro_linkFormat{sizeof(maro_linkFormat)};
        SendMessageW(maro_issues, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&maro_linkFormat));
        maro_check((maro_linkFormat.dwEffects & (CFE_LINK | CFE_UNDERLINE)) == (CFE_LINK | CFE_UNDERLINE),
            "safe fix solution has a visible link style");
        SendMessageW(maro_debugWindow, WM_NOTIFY, 104, reinterpret_cast<LPARAM>(&maro_link));
        SendMessageW(maro_debugWindow, WM_NOTIFY, 104, reinterpret_cast<LPARAM>(&maro_link));
        maro_check(maro_fixes == 1 && maro_clicked.fix && maro_clicked.fix->edits[0].replacement == L";" &&
            maro_clicked.range.start.column == 31, "solution click carries exact fix once");
        maro_debug->maro_SetResult(maro_result);
        POINTL maro_linkPoint{};
        SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_linkPoint), maro_link.chrg.cpMin + 2);
        const auto maro_clickAt = [&](int maro_x, int maro_y) {
            SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(maro_x, maro_y));
            SendMessageW(maro_issues, WM_LBUTTONUP, 0, MAKELPARAM(maro_x, maro_y));
        };
        maro_clickAt(320, maro_linkPoint.y + 2);
        maro_clickAt(4, 2);
        maro_check(maro_fixes == 1, "blank area and diagnostic title cannot apply fixes");
        maro_clickAt(maro_linkPoint.x + 2, maro_linkPoint.y + 2);
        maro_check(maro_fixes == 2, "native mouse clicks on solution glyphs apply the fix");
        maro_debug->maro_SetResult(maro_result);
        const auto maro_solutionPoint = MAKELPARAM(maro_linkPoint.x + 2, maro_linkPoint.y + 2);
        SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, maro_solutionPoint);
        maro_check(GetCapture() == maro_issues, "solution press captures its matching release");
        maro_debug->maro_SetResult(maro_result);
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, maro_solutionPoint);
        maro_check(maro_fixes == 2 && GetCapture() != maro_issues, "updated diagnostics cancel the previous solution press");
        SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, maro_solutionPoint);
        maro_debug->maro_SetPending(maro_error.sourcePath, L"검사 중...");
        maro_check(GetCapture() != maro_issues, "pending diagnostics release solution capture");
        maro_debug->maro_SetResult(maro_result);
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, maro_solutionPoint);
        maro_check(maro_fixes == 2, "pending and replacement diagnostics cannot reuse a press");
        SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, maro_solutionPoint);
        SetCapture(maro_parent);
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, maro_solutionPoint);
        maro_check(maro_fixes == 2 && GetCapture() == maro_parent, "lost capture cancels without releasing another window's capture");
        ReleaseCapture();
        SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, maro_solutionPoint);
        SendMessageW(maro_issues, WM_CANCELMODE, 0, 0);
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, maro_solutionPoint);
        maro_check(maro_fixes == 2 && GetCapture() != maro_issues, "cancelled mouse interaction cannot apply a fix");
        SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, maro_solutionPoint);
        SendMessageW(maro_issues, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-10, -10));
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, maro_solutionPoint);
        maro_check(maro_fixes == 2 && GetCapture() != maro_issues, "dragging away cancels even when released back on the solution");
        SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, maro_solutionPoint);
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, MAKELPARAM(-10, -10));
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, maro_solutionPoint);
        maro_check(maro_fixes == 2 && GetCapture() != maro_issues, "outside release cannot leave a stale solution press");
        maro_clickAt(maro_linkPoint.x + 2, maro_linkPoint.y + 2);
        maro_check(maro_fixes == 3, "a new complete solution click still works after cancellation");
        for (long maro_character = maro_link.chrg.cpMin; maro_character < maro_link.chrg.cpMax - 1; ++maro_character)
        {
            maro_debug->maro_SetResult(maro_result);
            POINTL maro_left{}, maro_right{};
            SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_left), maro_character);
            SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_right), maro_character + 1);
            if (maro_left.y != maro_right.y || maro_left.x >= maro_right.x) continue;
            const auto maro_before = maro_fixes;
            maro_clickAt(maro_right.x - 1, maro_left.y + 4);
            maro_check(maro_fixes == maro_before + 1, "right half of each solution glyph remains clickable");
        }
        maro_result.diagnostics[0].code.clear();
        maro_result.diagnostics[0].friendlyMessage = L"무한반복 의심\n종료 조건과 반복 변수를 확인하세요.";
        maro_result.diagnostics[0].fix.reset();
        maro_debug->maro_SetResult(maro_result);
        maro_check(maro_ControlText(maro_issues).starts_with(L"무한반복 의심 · 3행 31열") &&
            maro_ControlText(maro_issues).find(L"종료 조건과 반복 변수를 확인하세요.") != std::wstring::npos &&
            maro_ControlText(maro_issues).find(L"C2143") == std::wstring::npos, "runtime guidance has no invented compiler code");
        maro_output->maro_ClearOutput();
        maro_output->maro_AppendOutput(std::wstring(1024 * 1024 + 10, L'X'));
        maro_output->maro_AppendOutput(L"tail");
        maro_check(maro_ControlText(maro_summary).size() == 64 * 1024 && maro_ControlText(maro_summary).ends_with(L"tail"), "bounded output");
        maro_output->maro_ClearOutput();
        maro_output->maro_AppendOutput(maro_expected);
        std::wstring maro_received;
        int maro_eof = 0, maro_auto = 0, maro_project = 0, maro_run = 0, maro_start = 0, maro_step = 0, maro_continue = 0;
        unsigned maro_encoding = 0;
        maro_output->maro_SetSessionCallbacks([&](std::wstring maro_text) { maro_received = std::move(maro_text); return true; },
            [&] { ++maro_eof; }, [] {});
        maro_output->maro_SetAutomaticCallback([&] { ++maro_auto; });
        maro_output->maro_SetProjectCallback([&] { ++maro_project; });
        maro_output->maro_SetEncodingCallback([&](unsigned maro_value) { maro_encoding = maro_value; });
        maro_output->maro_SetRunCallback([&] { ++maro_run; });
        maro_output->maro_SetTraceCallbacks([&] { ++maro_start; }, [&] { ++maro_step; }, [&] { ++maro_continue; });
        const auto maro_repeatedInput = maro_InspectSource(L"int main(){ int n; while(scanf(\"%d\",&n)==1){} }", L"maro_input.c");
        maro_output->maro_SetSource(maro_repeatedInput);
        maro_output->maro_SetSessionState(true, true);
        SetWindowTextW(GetDlgItem(maro_outputWindow, 106), L"입력 42");
        maro_Key(GetDlgItem(maro_outputWindow, 106), VK_RETURN);
        maro_check(maro_received == L"입력 42" && maro_ControlText(GetDlgItem(maro_outputWindow, 106)).empty(), "Enter input");
        maro_Key(GetDlgItem(maro_outputWindow, 106), VK_RETURN);
        maro_check(maro_received.empty() && maro_ControlVisible(maro_outputWindow, 106), "repeated input and empty submission retain field");
        const auto maro_singleInput = maro_InspectSource(L"int main(){ int n; scanf(\"%d\",&n); }", L"maro_input.c");
        maro_output->maro_SetSource(maro_singleInput);
        SetWindowTextW(GetDlgItem(maro_outputWindow, 106), L" \t\v\f");
        maro_Key(GetDlgItem(maro_outputWindow, 106), VK_RETURN);
        maro_check(maro_ControlVisible(maro_outputWindow, 106), "blank numeric input does not hide field");
        SetWindowTextW(GetDlgItem(maro_outputWindow, 106), L"7");
        maro_Key(GetDlgItem(maro_outputWindow, 106), VK_RETURN);
        maro_check(maro_received == L"7" && !maro_ControlVisible(maro_outputWindow, 106), "one-shot input hides after a nonblank line");
        maro_output->maro_ClearOutput();
        maro_output->maro_SetSessionState(true, true);
        maro_check(maro_ControlVisible(maro_outputWindow, 106), "rerun resets input even without an observed stopped state");
        SetWindowTextW(GetDlgItem(maro_outputWindow, 106), L"8");
        maro_Key(GetDlgItem(maro_outputWindow, 106), VK_RETURN);
        maro_output->maro_SetSessionState(false, false);
        maro_output->maro_SetSessionState(true, true);
        maro_check(maro_ControlVisible(maro_outputWindow, 106), "stopped to running resets one-shot input");
        maro_output->maro_SetSource(maro_InspectSource(L"int main(){ char s[8]; fgets(s,8,stdin); }", L"maro_input.c"));
        SetWindowTextW(GetDlgItem(maro_outputWindow, 106), L"");
        maro_Key(GetDlgItem(maro_outputWindow, 106), VK_RETURN);
        maro_check(!maro_ControlVisible(maro_outputWindow, 106), "one-shot line reader accepts and hides after empty input");
        maro_Key(maro_summary, 'P', VK_CONTROL);
        maro_Key(maro_summary, 'E', VK_CONTROL);
        maro_Key(maro_summary, VK_F5);
        maro_check(maro_project == 1 && maro_encoding == 65001 && maro_run == 1, "advanced keyboard controls");
        const auto maro_source = maro_InspectSource(L"int main(void) {\n int maro_value = 3;\n maro_value += 4;\n return 0;\n}", L"maro_test.c");
        maro_debug->maro_SetSource(maro_source);
        maro_output->maro_SetSource(maro_source);
        maro_check(!maro_ControlVisible(maro_outputWindow, 106), "output-only source has no input field");
        maro_output->maro_SetProjectMode(true);
        maro_check(maro_ControlVisible(maro_outputWindow, 106), "project input remains available when other files may read it");
        maro_output->maro_SetProjectMode(false);
        maro_output->maro_AppendOutput(maro_expected);
        SendMessageW(maro_outputWindow, WM_COMMAND, MAKEWPARAM(105, BN_CLICKED), 0);
        const HWND maro_expanded = maro_TestExpandedWindow();
        maro_check(maro_expanded && maro_TwoButtons(maro_expanded) && !GetDlgItem(maro_expanded, 104) && maro_start == 1 &&
            maro_ControlText(GetDlgItem(maro_expanded, 102)) == maro_expected, "expand routes output and starts trace");
        maro_check(maro_ControlText(GetDlgItem(maro_expanded, 114)).find(L"int main") != std::wstring::npos &&
            maro_ControlText(GetDlgItem(maro_expanded, 116)).empty(), "static preview does not invent values");
        maro_Key(GetDlgItem(maro_expanded, 114), VK_RIGHT, VK_MENU);
        maro_check(maro_ControlText(GetDlgItem(maro_expanded, 115)).starts_with(L"2행"), "static line explanation");
        maro_TraceSnapshot maro_trace;
        maro_trace.maro_file = L"maro_test.c";
        maro_trace.maro_function = L"main";
        maro_trace.maro_line = 3;
        maro_trace.maro_waiting = true;
        maro_trace.maro_variables.push_back({L"maro_value", L"3", L"int", 42, true});
        maro_output->maro_SetTrace(maro_trace);
        maro_trace.maro_variables[0].maro_value = L"7";
        maro_output->maro_SetTrace(maro_trace);
        maro_check(maro_ControlText(GetDlgItem(maro_expanded, 116)).find(L"= 7") != std::wstring::npos &&
            SendDlgItemMessageW(maro_expanded, 123, TBM_GETRANGEMAX, 0, 0) == 1, "observed history");
        maro_Key(GetDlgItem(maro_expanded, 114), VK_LEFT, VK_MENU);
        maro_check(maro_ControlText(GetDlgItem(maro_expanded, 116)).find(L"= 3") != std::wstring::npos &&
            maro_ControlText(GetDlgItem(maro_expanded, 122)).starts_with(L"과거 관측값"), "history renders past values");
        SendDlgItemMessageW(maro_expanded, 123, TBM_SETPOS, TRUE, 1);
        SendMessageW(maro_expanded, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(GetDlgItem(maro_expanded, 123)));
        maro_check(maro_ControlText(GetDlgItem(maro_expanded, 116)).find(L"= 7") != std::wstring::npos, "history slider");
        maro_Key(GetDlgItem(maro_expanded, 102), VK_F10);
        maro_check(maro_step == 1, "output focus step shortcut");
        maro_output->maro_SetTrace(maro_trace);
        maro_Key(GetDlgItem(maro_expanded, 106), VK_F9);
        maro_check(maro_continue == 1, "input focus continue shortcut");
        maro_TraceVariable maro_array{L"maro_grid", L"6 cells", L"int[2][3]", 100, true};
        maro_array.maro_dimensions = {2, 3};
        for (unsigned maro_cell = 0; maro_cell < 6; ++maro_cell)
            maro_array.maro_children.push_back({L"[" + std::to_wstring(maro_cell / 3) + L"][" + std::to_wstring(maro_cell % 3) + L"]",
                std::to_wstring(maro_cell), L"int", 100 + maro_cell * 4, true});
        maro_trace.maro_variables = {maro_array};
        maro_output->maro_SetTrace(maro_trace);
        maro_check(maro_ControlText(GetDlgItem(maro_expanded, 116)).find(L"[1][2] = 5") != std::wstring::npos, "two dimensional cells");
        maro_trace.maro_variables[0].maro_children[5].maro_value = L"91";
        maro_output->maro_SetTrace(maro_trace);
        maro_check(maro_ControlText(GetDlgItem(maro_expanded, 116)).find(L"[1][2] = 91") != std::wstring::npos, "array-only mutation refreshes");
        for (unsigned maro_value = 0; maro_value < 260; ++maro_value)
        {
            maro_trace.maro_variables[0].maro_children[0].maro_value = std::to_wstring(maro_value);
            maro_output->maro_SetTrace(maro_trace);
        }
        maro_check(SendDlgItemMessageW(maro_expanded, 123, TBM_GETRANGEMAX, 0, 0) == 255, "history bounded 256");
        maro_output->maro_AppendOutput(L"expanded\n");
        maro_check(maro_ControlText(GetDlgItem(maro_expanded, 102)).ends_with(L"expanded\r\n"), "expanded streams");
        maro_output->maro_SetSource(maro_repeatedInput);
        SetWindowTextW(GetDlgItem(maro_expanded, 106), L"11");
        maro_Key(GetDlgItem(maro_expanded, 106), VK_RETURN);
        maro_check(maro_received == L"11" && maro_ControlVisible(maro_expanded, 106) &&
            !maro_ControlVisible(maro_outputWindow, 106), "expanded repeated input remains and docked field stays hidden");
        maro_Key(GetDlgItem(maro_expanded, 106), 'D', VK_CONTROL);
        maro_check(maro_eof == 1 && !IsWindowEnabled(GetDlgItem(maro_expanded, 106)), "EOF keyboard");
        SendMessageW(maro_expanded, WM_COMMAND, MAKEWPARAM(109, BN_CLICKED), 0);
        maro_check(maro_auto == 1, "off toggles automatic");
        maro_output->maro_SetAutomatic(false);
        maro_check(maro_ControlText(GetDlgItem(maro_expanded, 109)) == L"켜기", "off becomes on");
        SendMessageW(maro_expanded, WM_CLOSE, 0, 0);
        maro_check(!IsWindow(maro_expanded) && maro_ControlText(maro_summary).ends_with(L"expanded\r\n") &&
            !maro_ControlVisible(maro_outputWindow, 106), "restore retains output without revealing closed input");
        maro_output->maro_SetSource(maro_source);
        maro_output->maro_SetTrace({});
        maro_output->maro_ToggleExpanded();
        const HWND maro_second = maro_TestExpandedWindow();
        maro_check(SendDlgItemMessageW(maro_second, 123, TBM_GETRANGEMAX, 0, 0) == 0 &&
            maro_ControlText(GetDlgItem(maro_second, 116)).empty(), "new session clears history");
        maro_output->ClosePane();
        maro_check(!IsWindow(maro_second), "close destroys expansion");
        maro_debug->ClosePane();
        maro_outputWindow = nullptr;
        maro_check(SUCCEEDED(maro_output->CreatePaneWindow(maro_parent, 340, 0, 650, 260, &maro_outputWindow)) &&
            maro_ControlText(GetDlgItem(maro_outputWindow, 102)).ends_with(L"expanded\r\n") &&
            !GetDlgItem(maro_outputWindow, 104), "reopen preserves mode and output");
        maro_output->ClosePane();
    }
    if (maro_parent) DestroyWindow(maro_parent);
    if (SUCCEEDED(maro_apartment)) CoUninitialize();
    return maro_passed;
}
