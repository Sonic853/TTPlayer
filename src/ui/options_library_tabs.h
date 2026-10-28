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
inline constexpr int kLibraryOsuStableExecutableBrowse=0xe976;
inline constexpr int kLibraryOsuLazerExecutableBrowse=0xe977;
inline constexpr int kLibraryOsuStableSongsBrowse=0xe978;
inline constexpr int kLibraryOsuLazerFilesBrowse=0xe979;
inline constexpr int kLibraryOsuStableStatus=0xe97a;
inline constexpr int kLibraryOsuLazerStatus=0xe97b;
inline constexpr int kLibraryOsuStableExecutableLabel=0xe97c;
inline constexpr int kLibraryOsuLazerExecutableLabel=0xe97d;
inline constexpr int kLibraryOsuStableSongsLabel=0xe97e;
inline constexpr int kLibraryOsuLazerFilesLabel=0xe97f;
inline constexpr int kLibraryOsuLazerNotice=0xe980;
inline bool IsOsuLibraryPathControl(UINT id) {
    return id>=kLibraryOsuStableExecutable && id<=kLibraryOsuLazerFiles;
}
inline bool IsOsuLibraryBrowseControl(UINT id) {
    return id>=kLibraryOsuStableExecutableBrowse && id<=kLibraryOsuLazerFilesBrowse;
}
inline UINT OsuLibraryPathControlForBrowse(UINT id) {
    return IsOsuLibraryBrowseControl(id)?
        kLibraryOsuStableExecutable+(id-kLibraryOsuStableExecutableBrowse):0;
}

// Keep dialog 255 and its original library controls/notifications intact.
// Only their position and visibility change when selecting the nested tab.
class LibraryOptionsTabs {
    enum class Anchor { Fixed, Stretch, Right };
    struct Control { HWND window; RECT bounds; int group; bool visible; Anchor anchor{Anchor::Fixed}; };
    HWND page_{},tab_{};
    bool osu_enabled_{};
    std::vector<Control> controls_;
    int reference_width_{};
    bool laying_out_{};
    static constexpr UINT_PTR kSubclass=0x54544c54;

    void Layout() {
        if(laying_out_)return;
        laying_out_=true;
        RECT client{},inset{0,0,4,4};GetClientRect(page_,&client);i18n::MapDialogRect(page_,&inset);
        SetWindowPos(tab_,HWND_BOTTOM,0,0,client.right,client.bottom,SWP_NOACTIVATE);
        RECT content{};GetClientRect(tab_,&content);TabCtrl_AdjustRect(tab_,FALSE,&content);
        MapWindowPoints(tab_,page_,reinterpret_cast<POINT*>(&content),2);
        const int left=content.left+inset.right;
        const int right=content.right-inset.right;
        for(const auto& control:controls_) {
            const auto& r=control.bounds;
            // All bounds remain relative to the original design, not the last
            // WM_SIZE result. Labels keep their width, edits stretch, and the
            // browse buttons stay at the right edge without stretching.
            const int x=control.anchor==Anchor::Right?
                std::max<LONG>(left,right-(reference_width_-r.left)):left+r.left;
            const int width=control.anchor==Anchor::Stretch?
                right-(reference_width_-r.right)-x:
                std::min<LONG>(r.right-r.left,right-x);
            SetWindowPos(control.window,nullptr,x,content.top+inset.bottom+r.top,
                std::max(1,width),r.bottom-r.top,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOREDRAW);
        }
        RedrawWindow(page_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
        laying_out_=false;
    }
    void Select(int group) {
        if(group<0 || group>(osu_enabled_?2:0))return;
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
        RECT origin{7,7,0,0},compact{0,48,0,97};i18n::MapDialogRect(page,&origin);i18n::MapDialogRect(page,&compact);
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
        const wchar_t* tabs[]{i18n::Literal(L"媒体库"),L"osu!lazer",L"osu!"};
        for(int i=0;i<3;++i) {
            auto text=i18n::Text(tabs[i]);TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=text.data();
            TabCtrl_InsertItem(self->tab_,i,&item);
        }
        RECT reference{0,0,259,0};i18n::MapDialogRect(page,&reference);
        self->reference_width_=reference.right;
        const auto add=[&](LPCWSTR klass,LPCWSTR text,int id,DWORD style,RECT r,
                          int group,Anchor anchor=Anchor::Fixed,DWORD ex=0) {
            const auto caption=i18n::Text(text);
            HWND control=CreateWindowExW(ex,klass,caption.c_str(),WS_CHILD|style,0,0,0,0,page,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
            if(!control)return;
            SendMessageW(control,WM_SETFONT,font,FALSE);i18n::MapDialogRect(page,&r);
            self->controls_.push_back({control,r,group,true,anchor});
            if(IsOsuLibraryPathControl(id))SendMessageW(control,EM_SETLIMITTEXT,32767,0);
        };
        struct PathRow { int edit; int browse; int label; };
        const PathRow rows[2][2]{
            {{kLibraryOsuLazerExecutable,kLibraryOsuLazerExecutableBrowse,kLibraryOsuLazerExecutableLabel},
             {kLibraryOsuLazerFiles,kLibraryOsuLazerFilesBrowse,kLibraryOsuLazerFilesLabel}},
            {{kLibraryOsuStableExecutable,kLibraryOsuStableExecutableBrowse,kLibraryOsuStableExecutableLabel},
             {kLibraryOsuStableSongs,kLibraryOsuStableSongsBrowse,kLibraryOsuStableSongsLabel}}};
        for(int page_index=0;page_index<2;++page_index) {
            const int group=page_index+1;
            for(int row=0;row<2;++row) {
                const LONG y=3+row*23;
                const auto& ids=rows[page_index][row];
                add(WC_STATICW,row?i18n::Literal(L"歌曲目录："):i18n::Literal(L"程序文件："),ids.label,SS_LEFT,
                    {3,y+2,60,y+14},group);
                add(WC_EDITW,L"",ids.edit,WS_TABSTOP|ES_AUTOHSCROLL,
                    {62,y,237,y+14},group,Anchor::Stretch,WS_EX_CLIENTEDGE);
                add(WC_BUTTONW,L"",ids.browse,WS_TABSTOP|BS_PUSHBUTTON,
                    {241,y,259,y+14},group,Anchor::Right);
            }
            add(WC_STATICW,L"",page_index?kLibraryOsuStableStatus:kLibraryOsuLazerStatus,
                SS_LEFT,{3,52,259,78},group,Anchor::Stretch);
            add(WC_STATICW,page_index?
                i18n::Literal(L"歌曲目录通常为 osu! 程序目录下的 Songs 文件夹。\r\n路径可使用 %USERPROFILE% 等环境变量。"):
                i18n::Literal(L"歌曲目录为 lazer 存储位置下的 files 文件夹。\r\n路径可使用 %USERPROFILE% 等环境变量。"),
                page_index?kLibraryOsuNotice:kLibraryOsuLazerNotice,
                SS_LEFT,{3,86,259,128},group,Anchor::Stretch);
        }
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
