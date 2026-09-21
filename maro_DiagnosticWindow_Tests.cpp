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

bool maro_OnlyStopButton(HWND maro_window)
{
    unsigned maro_count = 0;
    for (HWND maro_child = GetWindow(maro_window, GW_CHILD); maro_child; maro_child = GetWindow(maro_child, GW_HWNDNEXT))
    {
        wchar_t maro_class[64]{};
        GetClassNameW(maro_child, maro_class, 64);
        if (_wcsicmp(maro_class, L"BUTTON") == 0) ++maro_count;
    }
    return maro_count == 1 && !GetDlgItem(maro_window, 105) && GetDlgItem(maro_window, 109);
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
        maro_check(maro_OnlyStopButton(maro_outputWindow) && !GetDlgItem(maro_outputWindow, 104) &&
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
        int maro_fixes = 0, maro_navigations = 0, maro_documentations = 0;
        Maro_Diagnostic maro_clicked;
        maro_debug->maro_SetFixCallback([&](const Maro_Diagnostic& maro_diagnostic) {
            ++maro_fixes;
            maro_clicked = maro_diagnostic;
        });
        maro_debug->maro_SetNavigateCallback([&](const Maro_Diagnostic& maro_diagnostic) {
            ++maro_navigations;
            maro_clicked = maro_diagnostic;
        });
        maro_debug->maro_SetDocumentationCallback([&](const Maro_Diagnostic& maro_diagnostic) {
            ++maro_documentations;
            maro_clicked = maro_diagnostic;
        });
        maro_result.diagnostics.push_back(maro_error);
        maro_debug->maro_SetResult(maro_result);
        maro_check(maro_ControlText(maro_issues).starts_with(L"C2143 · 오류 · 3행 31열") &&
            maro_ControlText(maro_issues).find(L"연관 진단 2개 접음") != std::wstring::npos &&
            maro_ControlText(maro_issues).find(L"문장 끝에 ';'가 필요합니다.") == std::wstring::npos &&
            maro_ControlText(maro_issues).find(L"세미콜론 추가") != std::wstring::npos &&
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
        maro_link.chrg.cpMax = maro_link.chrg.cpMin + static_cast<LONG>(maro_error.fix->description.size());
        SendMessageW(maro_issues, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&maro_link.chrg));
        CHARFORMAT2W maro_linkFormat{sizeof(maro_linkFormat)};
        SendMessageW(maro_issues, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&maro_linkFormat));
        maro_check((maro_linkFormat.dwEffects & (CFE_LINK | CFE_UNDERLINE)) == (CFE_LINK | CFE_UNDERLINE),
            "safe fix solution has a visible link style");
        SendMessageW(maro_debugWindow, WM_NOTIFY, 104, reinterpret_cast<LPARAM>(&maro_link));
        SendMessageW(maro_debugWindow, WM_NOTIFY, 104, reinterpret_cast<LPARAM>(&maro_link));
        maro_check(maro_fixes == 0 && maro_navigations == 0 && maro_documentations == 0,
            "link notifications without a matching mouse gesture cannot invoke callbacks");
        maro_debug->maro_SetResult(maro_result);
        POINTL maro_linkPoint{};
        SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_linkPoint), maro_link.chrg.cpMin + 2);
        const auto maro_clickAt = [&](int maro_x, int maro_y) {
            SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(maro_x, maro_y));
            SendMessageW(maro_issues, WM_LBUTTONUP, 0, MAKELPARAM(maro_x, maro_y));
        };
        maro_clickAt(320, maro_linkPoint.y + 2);
        POINTL maro_relatedPoint{}, maro_severityPoint{};
        SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_relatedPoint), maro_link.chrg.cpMax + 1);
        SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_severityPoint), 8);
        maro_clickAt(maro_relatedPoint.x + 2, maro_relatedPoint.y + 2);
        maro_clickAt(maro_severityPoint.x + 2, maro_severityPoint.y + 2);
        maro_check(maro_fixes == 0 && maro_navigations == 0 && maro_documentations == 0,
            "blank area related count and severity are not actions");
        POINTL maro_codePoint{}, maro_locationPoint{};
        SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_codePoint), 1);
        SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_locationPoint),
            static_cast<LPARAM>(std::wstring_view(L"C2143 · 오류 · ").size()));
        maro_clickAt(maro_codePoint.x + 2, maro_codePoint.y + 2);
        maro_check(maro_documentations == 1 && maro_navigations == 0 && maro_fixes == 0 && maro_clicked.code == L"C2143",
            "only the error code opens its documentation");
        maro_clickAt(maro_locationPoint.x + 2, maro_locationPoint.y + 2);
        maro_check(maro_navigations == 1 && maro_documentations == 1 && maro_fixes == 0 &&
            maro_clicked.range.start.line == 3 && maro_clicked.range.start.column == 31 && maro_clicked.sourcePath == maro_error.sourcePath,
            "line and column link carries the exact source location without fixing it");
        maro_clickAt(maro_locationPoint.x + 2, maro_locationPoint.y + 2);
        maro_check(maro_navigations == 2, "location links remain usable after navigation");
        maro_clickAt(maro_linkPoint.x + 2, maro_linkPoint.y + 2);
        maro_check(maro_fixes == 1 && maro_clicked.fix && maro_clicked.fix->edits[0].replacement == L";" &&
            maro_clicked.range.start.column == 31, "native solution click carries the exact fix");
        maro_clickAt(maro_linkPoint.x + 2, maro_linkPoint.y + 2);
        maro_check(maro_fixes == 1, "applied solution links cannot be reused before fresh diagnostics");
        maro_debug->maro_SetResult(maro_result);
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
        const auto maro_codePosition = MAKELPARAM(maro_codePoint.x + 2, maro_codePoint.y + 2);
        const auto maro_locationPosition = MAKELPARAM(maro_locationPoint.x + 2, maro_locationPoint.y + 2);
        SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, maro_codePosition);
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, maro_locationPosition);
        maro_check(maro_navigations == 2 && maro_documentations == 1 && GetCapture() != maro_issues,
            "pressing the code and releasing on the location invokes neither action");
        SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, maro_locationPosition);
        maro_debug->maro_SetResult(maro_result);
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, maro_locationPosition);
        SendMessageW(maro_issues, WM_LBUTTONDOWN, MK_LBUTTON, maro_codePosition);
        maro_debug->maro_SetPending(maro_error.sourcePath, L"검사 중...");
        maro_debug->maro_SetResult(maro_result);
        SendMessageW(maro_issues, WM_LBUTTONUP, 0, maro_codePosition);
        maro_check(maro_navigations == 2 && maro_documentations == 1 && GetCapture() != maro_issues,
            "new or pending results invalidate navigation and documentation presses");
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
            maro_ControlText(maro_issues).find(L"직접 수정: 종료 조건과 반복 변수를 확인하세요.") != std::wstring::npos &&
            maro_ControlText(maro_issues).find(L"C2143") == std::wstring::npos, "runtime guidance has no invented compiler code");
        const auto maro_previousFixes = maro_fixes;
        maro_clickAt(5, 3);
        POINTL maro_guidancePoint{};
        SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_guidancePoint),
            static_cast<LPARAM>(std::wstring_view(L"무한반복 의심 · 3행 31열\r").size()));
        maro_clickAt(maro_guidancePoint.x + 2, maro_guidancePoint.y + 2);
        maro_check(maro_fixes == maro_previousFixes && maro_documentations == 1 && maro_navigations == 2,
            "runtime title and manual guidance are not actionable fixes or documentation");
        maro_result.diagnostics[0].range = {};
        maro_debug->maro_SetResult(maro_result);
        maro_clickAt(5, 3);
        maro_check(maro_navigations == 2 && maro_ControlText(maro_issues).find(L"행") == std::wstring::npos,
            "runtime diagnostics without a known location do not offer navigation");
        maro_result.diagnostics[0] = maro_error;
        maro_result.diagnostics[0].fix.reset();
        maro_result.diagnostics[0].range.generated = true;
        maro_debug->maro_SetResult(maro_result);
        maro_clickAt(maro_locationPoint.x + 2, maro_locationPoint.y + 2);
        maro_check(maro_navigations == 2, "generated source coordinates are not navigation links");
        maro_result.diagnostics[0] = maro_error;
        maro_result.diagnostics[0].fix->description = L"선언과 사용을 함께 수정합니다.";
        maro_result.diagnostics[0].fix->edits.push_back({17, 50, 1, L"=", L"+"});
        maro_debug->maro_SetResult(maro_result);
        maro_clickAt(maro_linkPoint.x + 2, maro_linkPoint.y + 2);
        maro_check(maro_fixes == maro_previousFixes + 1 && maro_clicked.fix && maro_clicked.fix->edits.size() == 2,
            "existing solution forwards all guarded edits without adding a second action");
        maro_check(maro_ControlText(maro_issues).find(L"선언과 사용을 함께 수정합니다.") != std::wstring::npos &&
            maro_ControlText(maro_issues).find(maro_error.friendlyMessage) == std::wstring::npos,
            "short fix description replaces the longer explanation without duplication");
        maro_result.diagnostics[0] = maro_error;
        maro_result.diagnostics[0].friendlyMessage = maro_error.fix->description;
        maro_debug->maro_SetResult(maro_result);
        const auto maro_sameDescription = maro_ControlText(maro_issues);
        const auto maro_firstDescription = maro_sameDescription.find(maro_error.fix->description);
        maro_check(maro_firstDescription != std::wstring::npos &&
            maro_sameDescription.find(maro_error.fix->description, maro_firstDescription + 1) == std::wstring::npos,
            "identical explanation and solution are displayed only once");
        const auto maro_clickCharacter = [&](long maro_character) {
            POINTL maro_point{};
            SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_point), maro_character);
            maro_clickAt(maro_point.x + 2, maro_point.y + 2);
        };
        const std::wstring maro_solution = L"비교가 목적이라면 '='를 '=='로 바꿉니다.";
        const std::wstring maro_caution = L"대입이 목적이면 수정하지 마세요.";
        for (const std::wstring_view maro_newline : {L"\n", L"\r\n", L"\r"})
        {
            maro_result.diagnostics[0] = maro_error;
            maro_result.diagnostics[0].friendlyMessage = L"조건식에서 값을 대입하고 있습니다. 긴 설명입니다.";
            maro_result.diagnostics[0].fix->description = maro_solution + std::wstring(maro_newline) + maro_caution;
            maro_debug->maro_SetResult(maro_result);
            const auto maro_displayed = maro_ControlText(maro_issues);
            const auto maro_firstSolution = maro_displayed.find(maro_solution);
            maro_check(maro_firstSolution != std::wstring::npos &&
                maro_displayed.find(maro_solution, maro_firstSolution + 1) == std::wstring::npos &&
                maro_displayed.find(maro_caution) != std::wstring::npos &&
                maro_displayed.find(L"긴 설명") == std::wstring::npos,
                "one short remedy preserves intent without repeating the original explanation");
            CHARRANGE maro_solutionRange{maro_link.chrg.cpMin,
                maro_link.chrg.cpMin + static_cast<long>(maro_solution.size() + 1 + maro_caution.size())};
            SendMessageW(maro_issues, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&maro_solutionRange));
            maro_linkFormat = {sizeof(maro_linkFormat)};
            SendMessageW(maro_issues, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&maro_linkFormat));
            maro_check((maro_linkFormat.dwEffects & (CFE_LINK | CFE_UNDERLINE)) == (CFE_LINK | CFE_UNDERLINE),
                "every remedy paragraph has the same actionable link style");
            const auto maro_before = maro_fixes;
            maro_clickCharacter(maro_solutionRange.cpMin + 1);
            maro_clickCharacter(maro_solutionRange.cpMin + 1);
            maro_check(maro_fixes == maro_before + 1, "first remedy sentence applies exactly one fix");
            maro_debug->maro_SetResult(maro_result);
            maro_clickCharacter(maro_link.chrg.cpMin + static_cast<long>(maro_solution.size()) + 2);
            maro_clickCharacter(maro_link.chrg.cpMin + static_cast<long>(maro_solution.size()) + 2);
            maro_check(maro_fixes == maro_before + 2 && maro_navigations == 2 && maro_documentations == 1,
                "later remedy sentence applies exactly one fix with normalized newline offsets");
        }
        maro_result.diagnostics[0] = maro_error;
        maro_result.diagnostics[0].friendlyMessage.clear();
        maro_debug->maro_SetResult(maro_result);
        const auto maro_beforeFallback = maro_fixes;
        const auto maro_fallbackText = maro_ControlText(maro_issues);
        const auto maro_fallbackPosition = maro_fallbackText.find(maro_error.fix->description);
        maro_clickCharacter(maro_link.chrg.cpMin + 1);
        maro_check(maro_fixes == maro_beforeFallback + 1 && maro_fallbackPosition != std::wstring::npos &&
            maro_fallbackText.find(maro_error.fix->description, maro_fallbackPosition + 1) == std::wstring::npos,
            "fix description remains visible once when friendly guidance is absent");
        maro_result.diagnostics[0] = maro_error;
        maro_result.diagnostics[0].fix->description.clear();
        maro_debug->maro_SetResult(maro_result);
        maro_clickCharacter(maro_link.chrg.cpMin + 1);
        maro_check(maro_fixes == maro_beforeFallback + 2 &&
            maro_ControlText(maro_issues).find(maro_error.friendlyMessage) != std::wstring::npos,
            "existing solution remains actionable when optional fix description is empty");
        maro_result.diagnostics[0].fix->edits.clear();
        maro_debug->maro_SetResult(maro_result);
        maro_clickCharacter(maro_link.chrg.cpMin + 1);
        maro_check(maro_fixes == maro_beforeFallback + 2 &&
            maro_ControlText(maro_issues).find(L"직접 수정: " + maro_error.friendlyMessage) != std::wstring::npos,
            "a solution without guarded edits is clearly labelled manual guidance");
        maro_result.diagnostics[0].code.clear();
        maro_result.diagnostics[0].friendlyMessage = L"실행 오류\r\n첫 설명\r\n다음 설명";
        maro_result.diagnostics[0].fix.reset();
        maro_result.diagnostics.push_back(maro_error);
        maro_debug->maro_SetResult(maro_result);
        const std::wstring maro_firstDiagnostic = L"실행 오류 · 3행 31열\r직접 수정: 첫 설명\r다음 설명\r연관 진단 2개 접음\r\r";
        POINTL maro_secondCode{};
        SendMessageW(maro_issues, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&maro_secondCode),
            static_cast<LPARAM>(maro_firstDiagnostic.size() + 1));
        maro_clickAt(maro_secondCode.x + 2, maro_secondCode.y + 2);
        maro_check(maro_documentations == 2 && maro_clicked.code == L"C2143",
            "multiline runtime guidance retains exact following link offsets");
        maro_result.diagnostics.resize(1);
        maro_result.diagnostics[0] = maro_error;
        maro_result.diagnostics[0].code = L"unsupported-code";
        maro_result.diagnostics[0].friendlyMessage = L"잘못된 형식입니다.\n인수의 형식과 서식 문자열을 맞추세요.";
        maro_result.diagnostics[0].fix.reset();
        maro_debug->maro_SetResult(maro_result);
        maro_clickAt(maro_codePoint.x + 2, maro_codePoint.y + 2);
        CHARRANGE maro_unsupportedRange{0, 16};
        SendMessageW(maro_issues, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&maro_unsupportedRange));
        maro_linkFormat = {sizeof(maro_linkFormat)};
        SendMessageW(maro_issues, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&maro_linkFormat));
        maro_check(maro_documentations == 2 && (maro_linkFormat.dwEffects & CFE_LINK) == 0 &&
            maro_ControlText(maro_issues).find(L"직접 수정: 잘못된 형식입니다.") != std::wstring::npos &&
            maro_ControlText(maro_issues).find(L"인수의 형식과 서식 문자열을 맞추세요.") != std::wstring::npos,
            "unsupported codes stay plain while multiline manual solutions remain visible");
        maro_result.diagnostics[0] = maro_error;
        maro_result.diagnostics[0].code.clear();
        maro_result.diagnostics[0].friendlyMessage = L"실행 오류\n긴 설명입니다.";
        maro_result.diagnostics[0].fix->description = L"검증된 수정을 적용합니다.";
        maro_debug->maro_SetResult(maro_result);
        const auto maro_runtimeFixes = maro_fixes;
        const auto maro_runtimeText = maro_ControlText(maro_issues);
        const auto maro_runtimeRemedy = maro_runtimeText.find(maro_result.diagnostics[0].fix->description);
        maro_check(maro_runtimeText.starts_with(L"실행 오류 · 3행 31열") &&
            maro_runtimeRemedy != std::wstring::npos && maro_runtimeText.find(L"긴 설명") == std::wstring::npos,
            "code-free runtime title stays separate from a short guarded remedy");
        if (maro_runtimeRemedy != std::wstring::npos) maro_clickCharacter(static_cast<long>(maro_runtimeRemedy + 1));
        maro_check(maro_fixes == maro_runtimeFixes + 1, "code-free guarded remedy remains clickable");
        maro_output->maro_ClearOutput();
        maro_output->maro_AppendOutput(std::wstring(1024 * 1024 + 10, L'X'));
        maro_output->maro_AppendOutput(L"tail");
        maro_check(maro_ControlText(maro_summary).size() == 64 * 1024 && maro_ControlText(maro_summary).ends_with(L"tail"), "bounded output");
        maro_output->maro_ClearOutput();
        maro_output->maro_AppendOutput(maro_expected);
        std::wstring maro_received;
        int maro_eof = 0, maro_auto = 0, maro_project = 0, maro_run = 0;
        unsigned maro_encoding = 0;
        maro_output->maro_SetSessionCallbacks([&](std::wstring maro_text) { maro_received = std::move(maro_text); return true; },
            [&] { ++maro_eof; }, [] {});
        maro_output->maro_SetAutomaticCallback([&] { ++maro_auto; });
        maro_output->maro_SetProjectCallback([&] { ++maro_project; });
        maro_output->maro_SetEncodingCallback([&](unsigned maro_value) { maro_encoding = maro_value; });
        maro_output->maro_SetRunCallback([&] { ++maro_run; });
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
        for (const auto maro_key : {VK_F9, VK_F10})
        {
            MSG maro_message{maro_summary, WM_KEYDOWN, static_cast<WPARAM>(maro_key)};
            maro_check(maro_output->TranslateAccelerator(&maro_message) == S_FALSE, "removed trace shortcut reaches Visual Studio");
            maro_Key(maro_summary, maro_key);
        }
        maro_Key(maro_summary, VK_LEFT, VK_MENU);
        maro_Key(maro_summary, VK_RIGHT, VK_MENU);
        maro_check(!maro_TestExpandedWindow() && maro_OnlyStopButton(maro_outputWindow) &&
            maro_ControlText(maro_summary) == maro_expected && maro_ControlVisible(maro_outputWindow, 102) &&
            maro_ControlText(GetDlgItem(maro_outputWindow, 124)).find(L"F10") == std::wstring::npos &&
            maro_ControlText(GetDlgItem(maro_outputWindow, 124)).find(L"기록") == std::wstring::npos,
            "removed expansion command and shortcuts never create a popup or hide output");
        maro_output->maro_AppendOutput(L"docked\n");
        maro_check(maro_ControlText(maro_summary).ends_with(L"docked\r\n"), "docked output continues streaming");
        maro_output->maro_SetSource(maro_repeatedInput);
        SetWindowTextW(GetDlgItem(maro_outputWindow, 106), L"11");
        maro_Key(GetDlgItem(maro_outputWindow, 106), VK_RETURN);
        maro_check(maro_received == L"11" && maro_ControlVisible(maro_outputWindow, 106),
            "docked repeated input remains available");
        maro_Key(GetDlgItem(maro_outputWindow, 106), 'D', VK_CONTROL);
        maro_check(maro_eof == 1 && !IsWindowEnabled(GetDlgItem(maro_outputWindow, 106)), "EOF keyboard");
        SendMessageW(maro_outputWindow, WM_COMMAND, MAKEWPARAM(109, BN_CLICKED), 0);
        maro_check(maro_auto == 1, "off toggles automatic");
        maro_output->maro_SetAutomatic(false);
        maro_check(maro_ControlText(GetDlgItem(maro_outputWindow, 109)) == L"켜기", "off becomes on");
        maro_output->ClosePane();
        maro_check(!maro_TestExpandedWindow(), "closing output leaves no popup");
        maro_debug->ClosePane();
        maro_outputWindow = nullptr;
        maro_check(SUCCEEDED(maro_output->CreatePaneWindow(maro_parent, 340, 0, 650, 260, &maro_outputWindow)) &&
            maro_ControlText(GetDlgItem(maro_outputWindow, 102)).ends_with(L"docked\r\n") &&
            !GetDlgItem(maro_outputWindow, 104), "reopen preserves mode and output");
        maro_output->ClosePane();
    }
    if (maro_parent) DestroyWindow(maro_parent);
    if (SUCCEEDED(maro_apartment)) CoUninitialize();
    return maro_passed;
}
