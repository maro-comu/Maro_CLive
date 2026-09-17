#include "maro_DiagnosticWindow.hpp"

#include <iostream>

namespace
{
class maro_TestModule : public ATL::CAtlExeModuleT<maro_TestModule> {};
maro_TestModule maro_module;

std::wstring maro_ControlText(HWND control)
{
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    const int count = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    text.resize(count);
    return text;
}
}

bool maro_TestDiagnosticWindow()
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool passed = false;
    HWND parent = CreateWindowExW(0, L"STATIC", L"maro_test", WS_OVERLAPPEDWINDOW,
        0, 0, 400, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        ATL::CComObject<maro_DiagnosticWindow>* raw = nullptr;
        if (parent != nullptr && SUCCEEDED(ATL::CComObject<maro_DiagnosticWindow>::CreateInstance(&raw)))
        {
            ATL::CComPtr<maro_DiagnosticWindow> pane = raw;
            HWND window = nullptr;
            if (SUCCEEDED(pane->CreatePaneWindow(parent, 0, 0, 340, 700, &window)))
            {
                const HWND summary = GetDlgItem(window, 102);
                const HWND issues = GetDlgItem(window, 104);
                RECT top{}, bottom{};
                GetWindowRect(summary, &top);
                GetWindowRect(issues, &bottom);
                passed = true;
                const auto maro_check = [&passed](bool condition, const char* name) {
                    if (!condition) std::cerr << "[FAIL] pane: " << name << '\n';
                    passed = condition && passed;
                };
                maro_check(summary != nullptr && issues != nullptr && top.bottom < bottom.top &&
                    (GetWindowLongPtrW(summary, GWL_STYLE) & ES_READONLY) != 0 &&
                    (GetWindowLongPtrW(issues, GWL_STYLE) & ES_READONLY) != 0 &&
                    (GetWindowLongPtrW(summary, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL | WS_BORDER)) == 0 &&
                    (GetWindowLongPtrW(issues, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL | WS_BORDER)) == 0 &&
                    maro_ControlText(GetDlgItem(window, 101)) == L"실시간 출력", "control styles");
                HDC colors = CreateCompatibleDC(nullptr);
                const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(window, WM_CTLCOLORSTATIC,
                    reinterpret_cast<WPARAM>(colors), reinterpret_cast<LPARAM>(summary)));
                LOGBRUSH background{};
                maro_check(GetObjectW(brush, sizeof(background), &background) != 0 &&
                    background.lbColor == RGB(24, 24, 24) && GetBkColor(colors) == RGB(24, 24, 24) &&
                    GetTextColor(colors) == RGB(224, 224, 224), "dark colors");
                DeleteDC(colors);
                pane->maro_AppendOutput(L"maro_first\n");
                pane->maro_AppendOutput(L"maro_second\r");
                pane->maro_AppendOutput(L"\n");
                const std::wstring expectedOutput = L"maro_first\r\nmaro_second\r\n";
                maro_check(maro_ControlText(summary) == expectedOutput, "streamed output");
                pane->maro_SetPending(L"maro_sample.cpp", L"검사 중...");
                maro_check(maro_ControlText(summary) == expectedOutput &&
                    maro_ControlText(issues).find(L"maro_sample.cpp") != std::wstring::npos &&
                    maro_ControlText(issues).find(L"검사 중...") != std::wstring::npos, "pending preserves output");
                Maro_ResultEnvelope result;
                result.status = Maro_Status::CompileFailed;
                result.statusText = L"검사 완료";
                Maro_Diagnostic error;
                error.severity = Maro_Severity::Error;
                error.range.start.line = 7;
                error.code = L"E001";
                error.friendlyMessage = L"세미콜론을 확인하세요.";
                result.diagnostics.push_back(error);
                error.severity = Maro_Severity::Warning;
                error.friendlyMessage = L"초기화가 필요합니다.";
                result.diagnostics.push_back(error);
                pane->maro_SetResult(result);
                maro_check(maro_ControlText(summary) == expectedOutput &&
                    maro_ControlText(GetDlgItem(window, 103)) == L"실시간 진단 · 오류 1 · 경고 1" &&
                    maro_ControlText(issues).starts_with(L"E001 · 오류 · 7행") &&
                    maro_ControlText(issues).find(L"7행") != std::wstring::npos &&
                    maro_ControlText(issues).find(L"초기화가 필요합니다.") != std::wstring::npos, "error codes and counts");
                pane->maro_SetNotice(L"최신 버전입니다.");
                maro_check(maro_ControlText(summary) == expectedOutput &&
                    maro_ControlText(issues).find(L"최신 버전입니다.") != std::wstring::npos, "notice preserves output");
                result.status = Maro_Status::Success;
                result.diagnostics.clear();
                pane->maro_SetResult(result);
                maro_check(maro_ControlText(summary) == expectedOutput &&
                    maro_ControlText(issues).starts_with(L"문제 없음."), "result preserves output");
                pane->maro_ClearOutput();
                maro_check(maro_ControlText(summary).empty() &&
                    maro_ControlText(issues).starts_with(L"문제 없음."), "clear output independently");
                pane->maro_AppendOutput(std::wstring(1024 * 1024 + 10, L'X'));
                pane->maro_AppendOutput(L"maro_tail");
                maro_check(maro_ControlText(summary).size() == 64 * 1024 &&
                    maro_ControlText(summary).ends_with(L"maro_tail"), "bounded output");
                pane->maro_ClearOutput();
                std::wstring lines;
                for (int line = 0; line < 200; ++line)
                {
                    lines += L"maro_line\n";
                }
                pane->maro_AppendOutput(lines);
                SendMessageW(summary, EM_SETSEL, 0, 0);
                SendMessageW(summary, EM_SCROLLCARET, 0, 0);
                SendMessageW(summary, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
                maro_check(SendMessageW(summary, EM_GETFIRSTVISIBLELINE, 0, 0) > 0, "wheel without scrollbar");
                pane->maro_ClearOutput();
                pane->maro_AppendOutput(expectedOutput);
                SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(8, 315));
                SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(8, 420));
                SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(8, 420));
                RECT resized{};
                GetWindowRect(summary, &resized);
                maro_check(resized.bottom > top.bottom, "invisible splitter drag");
                MoveWindow(window, 0, 0, 220, 300, FALSE);
                GetWindowRect(summary, &top);
                GetWindowRect(issues, &bottom);
                maro_check(top.bottom < bottom.top && bottom.bottom > bottom.top, "resize layout");
                ATL::CComPtr<IStream> state;
                maro_check(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &state)) &&
                    SUCCEEDED(pane->SaveViewState(state)), "save split");
                LARGE_INTEGER zero{};
                if (state) state->Seek(zero, STREAM_SEEK_SET, nullptr);
                maro_check(state && SUCCEEDED(pane->LoadViewState(state)), "restore split");
                pane->ClosePane();
                maro_check(!IsWindow(window), "close");
                window = nullptr;
                maro_check(SUCCEEDED(pane->CreatePaneWindow(parent, 0, 0, 340, 700, &window)) &&
                    maro_ControlText(GetDlgItem(window, 104)).starts_with(L"문제 없음.") &&
                    maro_ControlText(GetDlgItem(window, 102)) == expectedOutput, "reopen");
                pane->ClosePane();
            }
        }
    }
    if (parent != nullptr)
    {
        DestroyWindow(parent);
    }
    if (SUCCEEDED(apartment))
    {
        CoUninitialize();
    }
    return passed;
}
