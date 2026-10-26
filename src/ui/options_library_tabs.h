#pragma once

#include "ttplayer/i18n/i18n.h"
#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>
#include <windows.h>
#include <commctrl.h>

namespace ttplayer::ui::detail {
inline constexpr int kLibrarySettingsTab=0xe970;
inline constexpr int kLibraryOsuStableExecutable=0xe971;
inline constexpr int kLibraryOsuLazerExecutable=0xe972;
inline constexpr int kLibraryOsuStableSongs=0xe973;
inline constexpr int kLibraryOsuLazerFiles=0xe974;
inline constexpr int kLibraryOsuNotice=0xe975;
inline bool IsOsuLibraryPathControl(UINT id) {
    return id>=kLibraryOsuStableExecutable && id<=kLibraryOsuLazerFiles;
}

// Keep dialog 255 and its original library controls/notifications intact.
// Only their position and visibility change when selecting the nested tab.
class LibraryOptionsTabs {
    struct Control { HWND window; RECT bounds; int group; bool visible; };
    HWND page_{},tab_{};
    bool osu_enabled_{};
    std::vector<Control> controls_;
    bool laying_out_{};
    static constexpr UINT_PTR kSubclass=0x54544c54;

    void Layout() {
        if(laying_out_)return;
        laying_out_=true;
        RECT client{},inset{0,0,4,4};GetClientRect(page_,&client);MapDialogRect(page_,&inset);
        SetWindowPos(tab_,HWND_BOTTOM,0,0,client.right,client.bottom,SWP_NOACTIVATE);
        RECT content{};GetClientRect(tab_,&content);TabCtrl_AdjustRect(tab_,FALSE,&content);
        MapWindowPoints(tab_,page_,reinterpret_cast<POINT*>(&content),2);
        for(const auto& control:controls_) {
            const auto& r=control.bounds;
            const int x=content.left+inset.right+r.left;
            const int width=control.group==1?content.right-inset.right-x:
                std::min<LONG>(r.right-r.left,content.right-inset.right-x);
            SetWindowPos(control.window,nullptr,x,content.top+inset.bottom+r.top,
                std::max(1,width),r.bottom-r.top,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOREDRAW);
        }
        RedrawWindow(page_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
        laying_out_=false;
    }
    void Select(int group) {
        if(group<0 || group>(osu_enabled_?1:0))return;
        TabCtrl_SetCurSel(tab_,group);
        const HWND focus=GetFocus();
        for(const auto& control:controls_) {
            const bool show=control.group==group && control.visible;
            if(!show && (focus==control.window || IsChild(control.window,focus)))SetFocus(tab_);
            ShowWindow(control.window,show?SW_SHOW:SW_HIDE);
        }
        RedrawWindow(page_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }
    static LRESULT CALLBACK PageProc(HWND page,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR data) {
        auto* self=reinterpret_cast<LibraryOptionsTabs*>(data);
        if(message==WM_NOTIFY) {
            const auto* n=reinterpret_cast<const NMHDR*>(lparam);
            if(n && n->hwndFrom==self->tab_ && n->code==TCN_SELCHANGE) {
                self->Select(TabCtrl_GetCurSel(self->tab_));return 0;
            }
        } else if(message==WM_SIZE) {
            const auto result=DefSubclassProc(page,message,wparam,lparam);self->Layout();return result;
        } else if(message==WM_NCDESTROY) {
            RemoveWindowSubclass(page,PageProc,id);delete self;
        }
        return DefSubclassProc(page,message,wparam,lparam);
    }
public:
    static void Attach(HWND page,bool osu_enabled) {
        if(!osu_enabled)return;
        DWORD_PTR existing{};if(GetWindowSubclass(page,PageProc,kSubclass,&existing))return;
        auto* self=new LibraryOptionsTabs;self->page_=page;self->osu_enabled_=osu_enabled;
        RECT origin{7,7,0,0},compact{0,48,0,97};MapDialogRect(page,&origin);MapDialogRect(page,&compact);
        for(HWND control=GetWindow(page,GW_CHILD);control;control=GetWindow(control,GW_HWNDNEXT)) {
            RECT r{};GetWindowRect(control,&r);MapWindowPoints(nullptr,page,reinterpret_cast<POINT*>(&r),2);
            wchar_t klass[32]{};GetClassNameW(control,klass,32);
            const auto style=GetWindowLongPtrW(control,GWL_STYLE);
            const bool group=_wcsicmp(klass,WC_BUTTONW)==0 && (style&BS_TYPEMASK)==BS_GROUPBOX;
            // The original 80-DLU options group contains a single checkbox.
            // Compact its blank space to make room for the tab header.
            if(group) {
                r.bottom-=compact.top;
                SetWindowLongPtrW(control,GWL_STYLE,style|WS_CLIPSIBLINGS);
            } else if(r.top>=compact.bottom)OffsetRect(&r,0,-compact.top);
            OffsetRect(&r,-origin.left,-origin.top);
            self->controls_.push_back({control,r,0,(style&WS_VISIBLE)!=0});
        }
        const auto instance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(page,GWLP_HINSTANCE));
        self->tab_=CreateWindowExW(0,WC_TABCONTROLW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS,
            0,0,0,0,page,reinterpret_cast<HMENU>(kLibrarySettingsTab),instance,nullptr);
        if(!self->tab_ || !SetWindowSubclass(page,PageProc,kSubclass,reinterpret_cast<DWORD_PTR>(self))) {
            if(self->tab_)DestroyWindow(self->tab_);delete self;return;
        }
        const auto font=SendMessageW(page,WM_GETFONT,0,0);SendMessageW(self->tab_,WM_SETFONT,font,FALSE);
        for(int i=0;i<2;++i) {
            auto text=i18n::Text(i?L"osu!":L"媒体库");TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=text.data();
            TabCtrl_InsertItem(self->tab_,i,&item);
        }
        const auto add=[&](LPCWSTR klass,LPCWSTR text,int id,DWORD style,RECT r,DWORD ex=0) {
            const auto caption=i18n::Text(text);
            HWND control=CreateWindowExW(ex,klass,caption.c_str(),WS_CHILD|style,0,0,0,0,page,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
            if(!control)return;
            SendMessageW(control,WM_SETFONT,font,FALSE);MapDialogRect(page,&r);
            self->controls_.push_back({control,r,1,true});
            if(IsOsuLibraryPathControl(id))SendMessageW(control,EM_SETLIMITTEXT,32767,0);
        };
        const wchar_t* labels[]{L"osu!stable 路径",L"osulazer 路径",L"osu!stable 歌曲路径",L"osulazer 歌曲路径"};
        for(int row=0;row<4;++row) {
            const LONG y=row*35;
            add(WC_STATICW,labels[row],-1,SS_LEFT,{3,y+1,259,y+12});
            add(WC_EDITW,L"",kLibraryOsuStableExecutable+row,WS_TABSTOP|ES_AUTOHSCROLL,
                {3,y+14,259,y+28},WS_EX_CLIENTEDGE);
        }
        add(WC_STATICW,L"路径可保留 %USERPROFILE% 等环境变量。\r\nlazer 歌曲路径优先从 storage.ini 获取；歌曲索引位于 files 上级目录的 client.realm。",
            kLibraryOsuNotice,SS_LEFT,{3,146,259,180});
        self->Layout();self->Select(0);
    }
    static void ShowControl(HWND page,HWND control) {
        DWORD_PTR data{};if(!GetWindowSubclass(page,PageProc,kSubclass,&data))return;
        auto* self=reinterpret_cast<LibraryOptionsTabs*>(data);
        for(const auto& entry:self->controls_)if(entry.window==control || IsChild(entry.window,control)) {
            self->Select(entry.group);return;
        }
    }
};
} // namespace ttplayer::ui::detail
