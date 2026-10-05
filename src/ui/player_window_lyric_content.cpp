#include "ttplayer/ui/player_window.h"
#include "ttplayer/i18n/i18n.h"
#include "player_window_internal.h"
#include "../app/resource_ids.h"
#include <algorithm>

namespace ttplayer::ui {
using namespace detail;

bool PlayerWindow::NativeLyricContentActive() const noexcept {
    return fullscreen_mode_ == 0 && !fullscreen_lyric_detached_ &&
        settings_.lyric.window_content_mode != TTP_SKIN_CONTENT_LYRICS &&
        !(external_skin_ && external_skin_->Handles(lyric_window_));
}

TtpSkinContent PlayerWindow::NativeLyricContentState(HWND surface) const {
    TtpSkinContent content{sizeof(content),surface};
    content.mode=std::clamp(settings_.lyric.window_content_mode,1,3);
    content.visual_type=std::clamp(settings_.lyric.window_visual_type,0,6);
    if(surface==lyric_control_) GetClientRect(surface,&content.bounds);
    else {
        content.bounds=LyricTextBounds();
        if(ActiveLyricTransparent() && settings_.lyric.transparent_skin)
            GetClientRect(lyric_window_,&content.bounds);
    }
    return content;
}

void PlayerWindow::PaintNativeLyricContent(HDC dc) {
    const auto content=NativeLyricContentState(lyric_control_);
    const auto& r=content.bounds;
    if(!dc || IsRectEmpty(&r))return;
    // Compose visual + lyrics offscreen, as the skin providers do, so a
    // repaint never exposes the intermediate black erase or half a frame.
    const HDC canvas=CreateCompatibleDC(dc);
    const HBITMAP bitmap=CreateCompatibleBitmap(dc,r.right,r.bottom);
    if(!canvas || !bitmap) {
        if(canvas)DeleteDC(canvas);if(bitmap)DeleteObject(bitmap);return;
    }
    const auto old=SelectObject(canvas,bitmap);
    if(PaintSkinPluginContent(this,canvas,&r,content.mode,content.visual_type))
        BitBlt(dc,0,0,r.right,r.bottom,canvas,0,0,SRCCOPY);
    SelectObject(canvas,old);DeleteObject(bitmap);DeleteDC(canvas);
}

void PlayerWindow::SetNativeLyricContent(int mode,int visual_type) {
    if(file_info_write_in_progress_ || fullscreen_mode_ ||
       (external_skin_ && external_skin_->Handles(lyric_window_)))return;
    if(lyric_control_)SendMessageW(lyric_control_,WM_CANCELMODE,0,0);
    settings_.lyric.window_content_mode=std::clamp(mode,1,3);
    if(visual_type>=0)settings_.lyric.window_visual_type=std::clamp(visual_type,0,6);
    plugin_content_visual_enabled_.store(false,std::memory_order_release);
    plugin_content_visual_type_=-1;
    LayoutLyricControls();RebuildLyricFont(false);
    ApplySkinWindowAlpha(lyric_window_,rendered_skin_window_alpha_);
    if(lyric_window_)RedrawWindow(lyric_window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}

bool PlayerWindow::HandleNativeLyricContentCommand(UINT command) {
    if(command>=kCmdLyricContentFirst && command<=kCmdLyricContentLast) {
        SetNativeLyricContent(int(command-kCmdLyricContentFirst)+1);return true;
    }
    if(command>=kCmdLyricContentEffectFirst && command<=kCmdLyricContentEffectLast) {
        SetNativeLyricContent(settings_.lyric.window_content_mode,int(command-kCmdLyricContentEffectFirst));return true;
    }
    if(command==kCmdLyricContentFullscreen) {
        if(!file_info_write_in_progress_ && !fullscreen_mode_ &&
           audio_->State()==audio::PlaybackState::playing) {
            plugin_content_fullscreen_saved_type_=settings_.visual.type;
            SetFullScreenMode(settings_.lyric.window_content_mode,lyric_window_,settings_.lyric.window_visual_type);
        }
        return true;
    }
    return false;
}

HMENU PlayerWindow::CreateNativeLyricContentMenu() const {
    HMENU menu=CreatePopupMenu(),effects=CreatePopupMenu();
    if(!menu || !effects) {if(menu)DestroyMenu(menu);if(effects)DestroyMenu(effects);return nullptr;}
    const wchar_t* modes[]={L"歌词",L"视觉效果",L"歌词与视觉同屏"};
    for(UINT i=0;i<3;++i)
        AppendMenuW(menu,MF_STRING|(settings_.lyric.window_content_mode==int(i)+1?MF_CHECKED:0),
            kCmdLyricContentFirst+i,i18n::Literal(modes[i]));
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    for(UINT i=0;i<7;++i) {
        const auto label=i==0?std::wstring(i18n::Literal(L"无")):i<4?ResourceListItem(ResourceModule(),2232,i-1):
            LoadResourceText(GetModuleHandleW(nullptr),i==4?IDS_FULLSCREEN_ALBUM:i==5?IDS_VISUAL_PULSE:IDS_VISUAL_RIPPLE);
        AppendMenuW(effects,MF_STRING|(settings_.lyric.window_visual_type==int(i)?MF_CHECKED:0),
            kCmdLyricContentEffectFirst+i,label.c_str());
    }
    AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(effects),i18n::Literal(L"视觉效果类型"));
    AppendMenuW(menu,MF_STRING|(audio_->State()==audio::PlaybackState::playing?0:MF_GRAYED),
        kCmdLyricContentFullscreen,i18n::Literal(L"全屏显示当前内容"));
    return menu;
}

void PlayerWindow::MoveContentOptionsToBottom(HMENU menu) {
    if(!menu)return;
    for(int i=0;i<GetMenuItemCount(menu);++i) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask=MIIM_ID|MIIM_FTYPE|MIIM_STATE|MIIM_DATA|MIIM_SUBMENU|MIIM_BITMAP|MIIM_CHECKMARKS|MIIM_STRING;
        if(!GetMenuItemInfoW(menu,i,TRUE,&item) ||
           (item.wID!=kCmdLyricOptions && item.wID!=kCmdVisualOptions))continue;
        std::wstring text(item.cch+1,L'\0');item.dwTypeData=text.data();item.cch=static_cast<UINT>(text.size());
        if(!GetMenuItemInfoW(menu,i,TRUE,&item))return;
        RemoveMenu(menu,i,MF_BYPOSITION);
        // Moving the options section can expose consecutive/trailing separators.
        bool separator=true;
        for(int position=0;position<GetMenuItemCount(menu);) {
            MENUITEMINFOW next{sizeof(next)};next.fMask=MIIM_FTYPE;
            GetMenuItemInfoW(menu,position,TRUE,&next);
            const bool current=(next.fType&MFT_SEPARATOR)!=0;
            if(current && separator)DeleteMenu(menu,position,MF_BYPOSITION);
            else {separator=current;++position;}
        }
        if(!separator && GetMenuItemCount(menu)>0)AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        InsertMenuItemW(menu,GetMenuItemCount(menu),TRUE,&item);
        return;
    }
}
}
