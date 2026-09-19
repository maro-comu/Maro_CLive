#include "maro_SourceWindow.hpp"
#include <commctrl.h>

bool maro_TestSourceWindow()
{
    const auto maro_apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool maro_ok = false;
    HWND maro_parent = CreateWindowExW(0, L"STATIC", L"maro_source_test", WS_OVERLAPPED,
        0, 0, 320, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (maro_parent)
    {
        ATL::CComObject<maro_SourceWindow>* maro_raw = nullptr;
        if (SUCCEEDED(ATL::CComObject<maro_SourceWindow>::CreateInstance(&maro_raw)))
        {
            ATL::CComPtr<maro_SourceWindow> maro_pane = maro_raw;
            HWND maro_window = nullptr;
            maro_SourceItem maro_selected;
            unsigned maro_clicks = 0;
            maro_pane->maro_SetNavigate([&](const maro_SourceItem& maro_item) { maro_selected = maro_item; ++maro_clicks; });
            maro_pane->maro_SetSource(maro_InspectSource(L"#pragma comment(lib, \"user32.lib\")\n#include <stdio.h>\nint maro_value;\nint main(void){ return 0; }", L"maro_test.c"));
            maro_ok = SUCCEEDED(maro_pane->CreatePaneWindow(maro_parent, 0, 0, 300, 600, &maro_window));
            const HWND maro_tree = GetDlgItem(maro_window, 201);
            const auto maro_click = [maro_tree](long maro_x, long maro_y) {
                const auto maro_position = MAKELPARAM(maro_x, maro_y);
                SendMessageW(maro_tree, WM_LBUTTONDOWN, MK_LBUTTON, maro_position);
                SendMessageW(maro_tree, WM_LBUTTONUP, 0, maro_position);
            };
            auto maro_root = TreeView_GetRoot(maro_tree);
            const wchar_t* maro_groups[] = {L"라이브러리", L"헤더파일", L"함수", L"변수"};
            for (int maro_index = 0; maro_index < 4; ++maro_index)
            {
                wchar_t maro_text[64]{};
                TVITEMW maro_item{};
                maro_item.mask = TVIF_TEXT;
                maro_item.hItem = maro_root;
                maro_item.pszText = maro_text;
                maro_item.cchTextMax = 64;
                maro_ok = maro_root && TreeView_GetItem(maro_tree, &maro_item) &&
                    std::wstring(maro_text) == maro_groups[maro_index] && maro_ok;
                RECT maro_groupBounds{};
                TreeView_GetItemRect(maro_tree, maro_root, &maro_groupBounds, TRUE);
                maro_click(maro_groupBounds.left + 2, maro_groupBounds.top + 2);
                maro_ok = maro_clicks == 0 && maro_ok;
                const auto maro_child = TreeView_GetChild(maro_tree, maro_root);
                if (maro_index == 3)
                {
                    TreeView_SelectItem(maro_tree, maro_child);
                    maro_ok = maro_clicks == 0 && maro_ok;
                    RECT maro_bounds{};
                    TreeView_GetItemRect(maro_tree, maro_child, &maro_bounds, TRUE);
                    SendMessageW(maro_tree, WM_LBUTTONUP, 0, MAKELPARAM(maro_bounds.left + 2, maro_bounds.top + 2));
                    maro_ok = maro_clicks == 0 && maro_ok;
                    maro_click(maro_bounds.left + 2, maro_bounds.top + 2);
                    maro_ok = maro_clicks == 1 && maro_selected.maro_name == L"maro_value" &&
                        maro_selected.maro_line == 3 && maro_selected.maro_path == L"maro_test.c" && maro_ok;
                    maro_click(290, maro_bounds.top + 2);
                    maro_ok = maro_clicks == 1 && maro_ok;
                    maro_click(maro_bounds.left + 2, maro_bounds.top + 2);
                    maro_ok = maro_clicks == 2 && maro_ok;
                }
                maro_root = TreeView_GetNextSibling(maro_tree, maro_root);
            }
            maro_pane->maro_SetSource({});
            maro_ok = maro_clicks == 2 && maro_ok;
            maro_pane->ClosePane();
            maro_ok = !IsWindow(maro_window) && maro_ok;
        }
        DestroyWindow(maro_parent);
    }
    if (SUCCEEDED(maro_apartment)) CoUninitialize();
    return maro_ok;
}
