#include "maro_SourceWindow.hpp"
#include <commctrl.h>
#include <algorithm>

STDMETHODIMP maro_SourceWindow::CreatePaneWindow(HWND maro_parent,int maro_x,int maro_y,int maro_width,int maro_height,HWND* maro_window)
{
    if(!maro_window) return E_POINTER;
    if(maro_window_) { *maro_window=maro_window_; return S_OK; }
    WNDCLASSEXW maro_type{sizeof(maro_type)};
    maro_type.lpfnWndProc=maro_Proc;
    maro_type.hInstance=ATL::_AtlBaseModule.GetModuleInstance();
    maro_type.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    maro_type.lpszClassName=L"maro_SourceExplorer";
    if(!RegisterClassExW(&maro_type) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return E_FAIL;
    *maro_window=CreateWindowExW(0,maro_type.lpszClassName,L"CLive_Maro 코드 목록",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,
        maro_x,maro_y,maro_width,maro_height,maro_parent,nullptr,maro_type.hInstance,this);
    return *maro_window ? S_OK : E_FAIL;
}

STDMETHODIMP maro_SourceWindow::ClosePane()
{
    if(maro_window_) DestroyWindow(maro_window_);
    return S_OK;
}

void maro_SourceWindow::maro_SetSource(maro_SourceInsight maro_insight)
{
    maro_insight_=std::move(maro_insight);
    maro_Rebuild();
}

void maro_SourceWindow::maro_Rebuild()
{
    if(!maro_tree_) return;
    maro_updating_=true;
    SendMessageW(maro_tree_,WM_SETREDRAW,FALSE,0);
    TreeView_DeleteAllItems(maro_tree_);
    const wchar_t* maro_names[]={L"라이브러리",L"헤더파일",L"함수",L"변수"};
    for(int maro_group=0;maro_group<4;++maro_group)
    {
        TVINSERTSTRUCTW maro_root{};
        maro_root.hParent=TVI_ROOT;
        maro_root.hInsertAfter=TVI_LAST;
        maro_root.item.mask=TVIF_TEXT|TVIF_PARAM;
        maro_root.item.pszText=const_cast<wchar_t*>(maro_names[maro_group]);
        const auto maro_node=reinterpret_cast<HTREEITEM>(SendMessageW(maro_tree_,TVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&maro_root)));
        for(std::size_t maro_index=0;maro_index<maro_insight_.maro_items.size();++maro_index)
        {
            const auto& maro_item=maro_insight_.maro_items[maro_index];
            if(static_cast<int>(maro_item.maro_kind)!=maro_group) continue;
            auto maro_label=maro_item.maro_name+L"  · "+std::to_wstring(maro_item.maro_line)+L"행";
            TVINSERTSTRUCTW maro_insert{};
            maro_insert.hParent=maro_node;
            maro_insert.hInsertAfter=TVI_LAST;
            maro_insert.item.mask=TVIF_TEXT|TVIF_PARAM;
            maro_insert.item.pszText=maro_label.data();
            maro_insert.item.lParam=static_cast<LPARAM>(maro_index+1);
            SendMessageW(maro_tree_,TVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&maro_insert));
        }
        TreeView_Expand(maro_tree_,maro_node,TVE_EXPAND);
    }
    SendMessageW(maro_tree_,WM_SETREDRAW,TRUE,0);
    InvalidateRect(maro_tree_,nullptr,TRUE);
    maro_updating_=false;
}

LRESULT CALLBACK maro_SourceWindow::maro_Proc(HWND maro_window,UINT maro_message,WPARAM maro_wparam,LPARAM maro_lparam) noexcept
{
    auto* maro_self=reinterpret_cast<maro_SourceWindow*>(GetWindowLongPtrW(maro_window,GWLP_USERDATA));
    if(maro_message==WM_NCCREATE)
    {
        maro_self=static_cast<maro_SourceWindow*>(reinterpret_cast<CREATESTRUCTW*>(maro_lparam)->lpCreateParams);
        maro_self->AddRef();
        maro_self->maro_window_=maro_window;
        SetWindowLongPtrW(maro_window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(maro_self));
    }
    if(!maro_self) return DefWindowProcW(maro_window,maro_message,maro_wparam,maro_lparam);
    if(maro_message==WM_NCDESTROY)
    {
        maro_self->maro_window_=nullptr;
        maro_self->maro_tree_=nullptr;
        if(maro_self->maro_font_) DeleteObject(maro_self->maro_font_);
        maro_self->maro_font_=nullptr;
        SetWindowLongPtrW(maro_window,GWLP_USERDATA,0);
        maro_self->Release();
        return DefWindowProcW(maro_window,maro_message,maro_wparam,maro_lparam);
    }
    try
    {
        switch(maro_message)
        {
        case WM_CREATE:
            {
                INITCOMMONCONTROLSEX maro_controls{sizeof(maro_controls),ICC_TREEVIEW_CLASSES};
                InitCommonControlsEx(&maro_controls);
                maro_self->maro_tree_=CreateWindowExW(0,WC_TREEVIEWW,L"라이브러리 · 헤더파일 · 함수 · 변수",
                    WS_CHILD|WS_VISIBLE|WS_TABSTOP|TVS_HASBUTTONS|TVS_LINESATROOT|TVS_SHOWSELALWAYS|TVS_INFOTIP,
                    0,0,1,1,maro_window,reinterpret_cast<HMENU>(201),ATL::_AtlBaseModule.GetModuleInstance(),nullptr);
                if(!maro_self->maro_tree_) return -1;
                TreeView_SetBkColor(maro_self->maro_tree_,RGB(24,24,24));
                TreeView_SetTextColor(maro_self->maro_tree_,RGB(224,224,224));
                maro_self->maro_font_=CreateFontW(-MulDiv(10,static_cast<int>(GetDpiForWindow(maro_window)),72),0,0,0,
                    FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                    DEFAULT_PITCH,L"Malgun Gothic");
                SendMessageW(maro_self->maro_tree_,WM_SETFONT,reinterpret_cast<WPARAM>(maro_self->maro_font_),TRUE);
                maro_self->maro_Rebuild();
            }
            return 0;
        case WM_SIZE:
            MoveWindow(maro_self->maro_tree_,0,0,LOWORD(maro_lparam),HIWORD(maro_lparam),TRUE);
            return 0;
        case WM_NOTIFY:
            if(!maro_self->maro_updating_ && reinterpret_cast<NMHDR*>(maro_lparam)->code==TVN_SELCHANGEDW)
            {
                const auto maro_id=reinterpret_cast<NMTREEVIEWW*>(maro_lparam)->itemNew.lParam;
                if(maro_id>0 && static_cast<std::size_t>(maro_id)<=maro_self->maro_insight_.maro_items.size() && maro_self->maro_navigate_)
                    maro_self->maro_navigate_(maro_self->maro_insight_.maro_items[maro_id-1]);
            }
            return 0;
        }
    }
    catch(...) { return maro_message==WM_CREATE ? -1 : 0; }
    return DefWindowProcW(maro_window,maro_message,maro_wparam,maro_lparam);
}
