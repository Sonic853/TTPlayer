#pragma once
#include "ttplayer/i18n/i18n.h"
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <vector>
#include <functional>
#include <algorithm>

namespace ttplayer::ui::detail {
class NetworkOptionsTabs {
    struct Control {HWND window;RECT bounds;bool visible;};
    HWND page_{},outer_{},inner_{},gpu_{};std::array<HWND,3> music_{};
    std::vector<Control> connection_;std::function<HRESULT()> commit_;
    bool has_music_{},layout_{};int selected_{};
    static constexpr UINT_PTR subclass=0x54544e54;
    static constexpr wchar_t property[]=L"TTPlayer.NetworkTabs";
    void Layout(){if(layout_)return;layout_=true;RECT r{};GetClientRect(page_,&r);SetWindowPos(outer_,HWND_BOTTOM,0,0,r.right,r.bottom,SWP_NOACTIVATE);
        TabCtrl_AdjustRect(outer_,FALSE,&r);RECT gap{0,0,3,3},origin{0,68,0,0};i18n::MapDialogRect(page_,&gap);i18n::MapDialogRect(page_,&origin);
        for(const auto& c:connection_)SetWindowPos(c.window,nullptr,c.bounds.left,c.bounds.top-origin.top+r.top+gap.bottom,c.bounds.right-c.bounds.left,c.bounds.bottom-c.bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);
        // The inner tab occupies the music pane, with the existing resource
        // controls retaining their own parents and notification handlers.
        RECT gpu{0,0,0,18};i18n::MapDialogRect(page_,&gpu);
        MoveWindow(gpu_,r.left+gap.right,r.top,r.right-r.left-gap.right*2,gpu.bottom-gap.bottom,TRUE);
        MoveWindow(inner_,r.left,r.top+gpu.bottom,r.right-r.left,r.bottom-r.top-gpu.bottom,TRUE);RECT body{};GetClientRect(inner_,&body);TabCtrl_AdjustRect(inner_,FALSE,&body);MapWindowPoints(inner_,page_,reinterpret_cast<POINT*>(&body),2);
        for(size_t i=0;i<music_.size();++i)if(music_[i]){RECT old{};GetWindowRect(music_[i],&old);SetWindowPos(music_[i],nullptr,body.left,body.top,body.right-body.left,i==2?body.bottom-body.top:old.bottom-old.top,SWP_NOZORDER|SWP_NOACTIVATE);}
        layout_=false;
    }
    void Visibility(){for(const auto& c:connection_)ShowWindow(c.window,selected_==0&&c.visible?SW_SHOW:SW_HIDE);ShowWindow(inner_,selected_==1?SW_SHOW:SW_HIDE);
        ShowWindow(gpu_,selected_==1?SW_SHOW:SW_HIDE);
        const int current=TabCtrl_GetCurSel(inner_);for(int i=0;i<3;++i)if(music_[i])ShowWindow(music_[i],selected_==1&&current==i?SW_SHOW:SW_HIDE);
        RedrawWindow(page_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }
    void Select(int n){if(n==1&&!has_music_)n=0;if(selected_==1&&n!=1&&commit_&&FAILED(commit_()))n=1;selected_=n;TabCtrl_SetCurSel(outer_,n);Visibility();}
    static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){auto* s=reinterpret_cast<NetworkOptionsTabs*>(data);
        if(m==WM_NOTIFY){auto* n=reinterpret_cast<NMHDR*>(l);if(n&&n->hwndFrom==s->outer_&&n->code==TCN_SELCHANGE){s->Select(TabCtrl_GetCurSel(s->outer_));return 0;}}
        if(m==WM_SIZE)s->Layout();
        if(m==WM_NCDESTROY){RemovePropW(h,property);RemoveWindowSubclass(h,Proc,id);delete s;}
        return DefSubclassProc(h,m,w,l);
    }
public:
    static constexpr int TabId=0xe9b0;
    static constexpr int DisableGpuId=0xe9b1;
    static void Attach(HWND page,const std::array<HWND,3>& music,bool available,std::function<HRESULT()> commit){
        if(GetPropW(page,property))return;auto* s=new NetworkOptionsTabs;s->page_=page;s->inner_=GetDlgItem(page,1181);s->music_=music;s->has_music_=available;s->commit_=std::move(commit);
        for(HWND h=GetWindow(page,GW_CHILD);h;h=GetWindow(h,GW_HWNDNEXT)){if(h==s->inner_||std::find(music.begin(),music.end(),h)!=music.end())continue;RECT r{};GetWindowRect(h,&r);MapWindowPoints(nullptr,page,reinterpret_cast<POINT*>(&r),2);s->connection_.push_back({h,r,(GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)!=0});}
        if(available){s->gpu_=CreateWindowExW(0,L"BUTTON",i18n::Literal(L"关闭 GPU 加速"),WS_CHILD|WS_TABSTOP|BS_AUTOCHECKBOX,0,0,0,0,page,reinterpret_cast<HMENU>(DisableGpuId),GetModuleHandleW(nullptr),nullptr);SendMessageW(s->gpu_,WM_SETFONT,SendMessageW(page,WM_GETFONT,0,0),TRUE);}
        s->outer_=CreateWindowExW(0,WC_TABCONTROLW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS,0,0,0,0,page,reinterpret_cast<HMENU>(TabId),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(s->outer_,WM_SETFONT,SendMessageW(page,WM_GETFONT,0,0),TRUE);TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<LPWSTR>(i18n::Literal(L"连接设置"));TabCtrl_InsertItem(s->outer_,0,&item);
        if(available){item.pszText=const_cast<LPWSTR>(i18n::Literal(L"音乐窗"));TabCtrl_InsertItem(s->outer_,1,&item);}
        SetPropW(page,property,s);SetWindowSubclass(page,Proc,subclass,reinterpret_cast<DWORD_PTR>(s));s->Layout();s->Select(0);
    }
    static void SelectMusic(HWND page){if(auto* s=static_cast<NetworkOptionsTabs*>(GetPropW(page,property)))s->Select(1);}
};
}
