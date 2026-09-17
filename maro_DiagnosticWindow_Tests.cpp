#include "maro_DiagnosticWindow.hpp"

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
                passed = summary != nullptr && issues != nullptr && top.bottom < bottom.top &&
                    (GetWindowLongPtrW(summary, GWL_STYLE) & ES_READONLY) != 0 &&
                    (GetWindowLongPtrW(issues, GWL_STYLE) & ES_READONLY) != 0;
                pane->maro_SetPending(L"maro_sample.cpp", L"검사 중...");
                passed = passed && maro_ControlText(summary).find(L"maro_sample.cpp") != std::wstring::npos &&
                    maro_ControlText(issues).empty();
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
                passed = passed && maro_ControlText(summary).find(L"검사 완료") != std::wstring::npos &&
                    maro_ControlText(GetDlgItem(window, 103)) == L"오류 1 · 경고 1" &&
                    maro_ControlText(issues).find(L"7행") != std::wstring::npos &&
                    maro_ControlText(issues).find(L"초기화가 필요합니다.") != std::wstring::npos;
                pane->maro_SetNotice(L"최신 버전입니다.");
                passed = passed && maro_ControlText(summary).find(L"최신 버전입니다.") != std::wstring::npos;
                result.status = Maro_Status::Success;
                result.diagnostics.clear();
                pane->maro_SetResult(result);
                passed = passed && maro_ControlText(issues) == L"문제 없음.";
                MoveWindow(window, 0, 0, 220, 300, FALSE);
                GetWindowRect(summary, &top);
                GetWindowRect(issues, &bottom);
                passed = passed && top.bottom < bottom.top && bottom.bottom > bottom.top;
                ATL::CComPtr<IStream> state;
                passed = passed && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &state)) &&
                    SUCCEEDED(pane->SaveViewState(state));
                LARGE_INTEGER zero{};
                state->Seek(zero, STREAM_SEEK_SET, nullptr);
                passed = passed && SUCCEEDED(pane->LoadViewState(state));
                pane->ClosePane();
                passed = passed && !IsWindow(window);
                window = nullptr;
                passed = passed && SUCCEEDED(pane->CreatePaneWindow(parent, 0, 0, 340, 700, &window)) &&
                    maro_ControlText(GetDlgItem(window, 104)) == L"문제 없음.";
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
