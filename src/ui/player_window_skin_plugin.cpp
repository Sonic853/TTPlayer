#include "ttplayer/ui/player_window.h"
#include "ttplayer/i18n/i18n.h"
#include "ttplayer/ui/wtl_menu.h"
#include "ttplayer/ui/window_drag.h"
#include "ttplayer/ui/player_runtime_policy.h"
#include "player_window_internal.h"
#include "ttplayer/skin/skin_paths.h"
#include <algorithm>
#include <limits>
#include <windowsx.h>
#include <richedit.h>

namespace ttplayer::ui {
using namespace detail;
void PlayerWindow::DiscoverSkinPlugins() {
    if(skin_plugins_discovered_) return;
    skin_plugins_discovered_=true;
    skin_plugins_=skin::SkinPluginModule::Discover(PlayerRuntimeDirectory()/L"AddIn");
}
bool PlayerWindow::IsPluginSkinPackage(const std::filesystem::path& path) {
    DiscoverSkinPlugins();
    return std::any_of(skin_plugins_.begin(),skin_plugins_.end(),[&](const auto& provider) {return provider->Supports(path);});
}
const std::wstring& PlayerWindow::ActiveSkinSelector() const {
    return external_skin_ ? settings_.plugin_skin_file : settings_.skin_file;
}
bool PlayerWindow::LoadPluginSkin(const std::filesystem::path& path, bool restore_profile) {
    DiscoverSkinPlugins();
    TtpSkinInfo info{};
    for(const auto& module:skin_plugins_) {
        if(!module->OwnsInstalledPackage(PlayerRuntimeDirectory()/L"Skin",path) || !module->Probe(path,info)) continue;
        const TtpSkinHost host{sizeof(TtpSkinHost),TTP_SKIN_ABI,this,
            QuerySkinPluginState,QuerySkinPluginTrack,PostSkinPluginCommand,HandleSkinPluginDrag,
            QuerySkinPluginSelection,PaintSkinPluginVisual,QuerySkinPluginTip,ResizeSkinPluginWindow,QuerySkinPluginSpectrum,PaintSkinPluginContent,HandleSkinPluginContentInput};
        auto next=skin::SkinPluginInstance::Create(module,path,&host);
        if(!next) return false;
        // The native fallback supplies lyrics and application dialogs. Format
        // parsing, classic drawing and input remain entirely in the provider.
        if(!skin_ || !skin_->Valid()) {
            const auto native = skin::ResolveSkinPackagePath(
                PlayerRuntimeDirectory()/L"Skin", settings_.skin_file);
            if ((native.empty() || !skin::IsNativeSkinPackage(native) ||
                 !LoadSkinPackage(native, restore_profile)) &&
                !LoadSkinResource(ResourceModule(),L"<Default_Skin>",restore_profile)) return false;
        }
        if(window_) {
            CompleteSkinWindowFadeForReplacement();
            if(close_after_skin_window_fade_) return false;
            if (restore_profile) SaveCurrentSkinProfile();
            if(IsIconic(window_)) ShowWindow(window_,SW_RESTORE);
            if(mini_mode_) {ToggleMiniMode();CompleteSkinWindowFadeForReplacement();}
        }
        const auto previous_player=settings_.player;
        const auto previous_playlist=settings_.playlist;
        const auto previous_lyric=settings_.lyric;
        const auto previous_visual=settings_.visual;
        // As with native skins, apply package defaults first and then overlay
        // only the attributes present in the target profile. The resulting
        // settings are shared by Lyrics Show, rendering and the editor.
        TtpSkinLyricColors colors{sizeof(colors)};
        if(restore_profile && next->LyricColors(nullptr,colors)) {
            settings_.lyric.text_color=colors.text;
            settings_.lyric.highlight_color=colors.highlight;
            settings_.lyric.background_color=colors.background;
        }
        auto profile=path;profile+=L".xml";
        std::wstring state;
        // Missing geometry in a partial target profile is a default layout,
        // not permission to inherit the outgoing provider's folded rectangles.
        auto target_player=settings_.player;
        SetRectEmpty(&target_player.player_window);
        SetRectEmpty(&target_player.playlist_window);
        SetRectEmpty(&target_player.equalizer_window);
        const bool profile_loaded=restore_profile && settings::LoadSkinVisualProfile(profile,
            target_player,settings_.playlist,settings_.lyric,settings_.visual,&state);
        if(profile_loaded) settings_.player=std::move(target_player);
        TtpSkinLayout layout{};layout.size=sizeof(layout);
        if(profile_loaded) {
            layout.windows[0]=settings_.player.player_window;
            layout.windows[1]=settings_.player.playlist_window;
            layout.windows[2]=settings_.player.equalizer_window;
            if(state.size()<std::size(layout.state)) wcscpy_s(layout.state,state.c_str());
        }
        if(!next->Layout(layout,true) && layout.state[0]) {
            // A future/corrupt provider payload must not discard usable saved
            // rectangles or prevent loading the skin itself.
            layout.state[0]=0;static_cast<void>(next->Layout(layout,true));
        }
        auto previous=std::move(external_skin_);
        if(previous) previous->Detach();
        if (window_) {
            static_cast<void>(CreatePlaylistWindow(true));
            static_cast<void>(CreateEqualizerWindow(true));
            if(profile_loaded) ApplySkinProfileWindowState();
        }
        if(window_ && !next->Attach(window_,playlist_window_,equalizer_window_,lyric_window_)) {
            settings_.player=previous_player;settings_.playlist=previous_playlist;
            settings_.lyric=previous_lyric;settings_.visual=previous_visual;
            ApplySkinProfileWindowState();
            if(previous) previous->Attach(window_,playlist_window_,equalizer_window_,lyric_window_);
            external_skin_=std::move(previous);RemovePluginSkinNativeTips();return false;
        }
        external_skin_=std::move(next);
        UpdateLyricEditorStyle();
        UpdateVisualWindowLayout();UpdateVisualFrame();
        RemovePluginSkinNativeTips();
        settings_.plugin_skin_file=skin::SkinPackageSelector(PlayerRuntimeDirectory()/L"Skin",path);
        settings_.player.mini_mode=false;
        if(window_) {
            ShowWindow(playlist_window_,settings_.player.playlist_visible?SW_SHOWNOACTIVATE:SW_HIDE);
            ShowWindow(equalizer_window_,settings_.player.equalizer_visible?SW_SHOWNOACTIVATE:SW_HIDE);
            RefreshPlaybackUi();InvalidateRect(window_,nullptr,FALSE);
        }
        return true;
    }
    return false;
}
bool PlayerWindow::InstallPluginSkin(const std::filesystem::path& path) {
    DiscoverSkinPlugins();
    TtpSkinInfo info{};
    const auto provider=std::find_if(skin_plugins_.begin(),skin_plugins_.end(),
        [&](const auto& module) {return module->Supports(path) && module->Probe(path,info);});
    if(provider==skin_plugins_.end()) return false;
    const auto directory=(*provider)->Directory(PlayerRuntimeDirectory()/L"Skin");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    const auto source = std::filesystem::absolute(path, error).lexically_normal();
    if (error) return false;
    const auto destination = directory / source.filename();
    if (_wcsicmp(source.c_str(), destination.c_str()) != 0 &&
        !CopyFileW(source.c_str(), destination.c_str(), FALSE)) return false;
    const bool loaded = LoadSkinPackage(destination);
    InvalidateSkinMenuCatalog();
    StartSkinMenuCatalogLoad();
    return loaded;
}
void PlayerWindow::RemovePluginSkinNativeTips() {
    if(!external_skin_) return;
    for(HWND tip:{tooltip_,playlist_tooltip_,playlist_item_tooltip_})
        if(IsWindow(tip)) SendMessageW(tip,TTM_POP,0,0);
    for(HWND owner:{window_,playlist_window_,equalizer_window_,playlist_track_control_})
        if(owner) RemoveToolTipTools(owner);
    if(external_skin_->Handles(lyric_window_)) RemoveToolTipTools(lyric_window_);
}
BOOL WINAPI PlayerWindow::QuerySkinPluginTip(void* context,uint32_t action,int32_t value,wchar_t* text,uint32_t count) {
    if(!context || !text || !count) return FALSE;
    text[0]=0;
    try {
        const auto& self=*static_cast<PlayerWindow*>(context);
        UINT command{};std::wstring label;
        switch(action) {
        case TTP_SKIN_PLAY: command=kCmdPlay;break;
        case TTP_SKIN_PAUSE: command=kCmdPause;break;
        case TTP_SKIN_STOP: command=kCmdStopPlayback;break;
        case TTP_SKIN_PREVIOUS: command=kCmdPrevious;break;
        case TTP_SKIN_NEXT: command=kCmdNext;break;
        case TTP_SKIN_OPEN: command=kCmdOpenFile;break;
        case TTP_SKIN_CLOSE: label=self.ResourceText(8);break;
        case TTP_SKIN_MINIMIZE: command=0x7dd3;break;
        case TTP_SKIN_PLAYLIST: command=kCmdShowPlaylist;break;
        case TTP_SKIN_EQUALIZER: command=kCmdShowEqualizer;break;
        case TTP_SKIN_OPTIONS: command=kCmdOptions;break;
        case TTP_SKIN_LYRICS: command=kCmdShowLyrics;break;
        case TTP_SKIN_PROPERTIES: command=kCmdFileProperties;break;
        case TTP_SKIN_ALWAYS_ON_TOP: command=kCmdAlwaysOnTop;break;
        case TTP_SKIN_EQ_ENABLE: command=kEqCommandEnable;break;
        case TTP_SKIN_EQ_PRESETS: label=self.EqualizerToolText(kEqControlProfile);break;
        case TTP_SKIN_EQ_VALUE:
            if(value>=0 && value<=10) label=self.EqualizerToolText(value?kEqSliderFirstBand+value-1:kEqSliderPreamp);
            break;
        case TTP_SKIN_LIST_TOOLBAR:
            if(value>=0 && value<7) label=self.ToolTipText(self.playlist_window_,kPlaylistToolFirst+value);
            break;
        default: return FALSE;
        }
        if(command) label=self.ToolTipWithHotKey(command,ResourceCommandLabel(self.ResourceModule(),command));
        if(label.empty()) return FALSE;
        wcsncpy_s(text,count,label.c_str(),_TRUNCATE);return TRUE;
    } catch(...) {return FALSE;}
}
BOOL WINAPI PlayerWindow::QuerySkinPluginState(void* context,TtpSkinState* state) {
    if(!context || !state || state->size<sizeof(*state)) return FALSE;
    try {
    const auto& self=*static_cast<PlayerWindow*>(context);
    state->playback=static_cast<int>(self.audio_->State());
    state->position_ms=self.audio_->Position().count();state->duration_ms=self.audio_->Duration().count();
    state->volume=self.settings_.player.mute?0:self.settings_.player.volume;
    state->balance=self.settings_.player.balance;state->mode=self.settings_.player.play_mode;
    const auto format=self.audio_->Format();
    state->channels=static_cast<int>(format.channels);state->sample_rate=static_cast<int>(format.sample_rate);
    state->bitrate=0;
    if(const auto* track=self.PlaybackTrackForUi()) state->bitrate=static_cast<int>(track->bitrate_bps);
    state->playlist_visible=IsWindowVisible(self.playlist_window_)!=FALSE;
    state->equalizer_visible=IsWindowVisible(self.equalizer_window_)!=FALSE;
    state->eq_enabled=self.settings_.equalizer.profile!=-2;
    state->elapsed=self.settings_.player.show_elapsed_time;
    std::copy(self.settings_.equalizer.current.begin(),self.settings_.equalizer.current.end(),state->eq);
    state->track_count=static_cast<uint32_t>(std::min<size_t>(self.VisiblePlaylistTrackCount(),INT_MAX));
    const auto row=self.VisiblePlaylistPlayingRow();
    state->playing_row=row?static_cast<int32_t>(*row):-1;
    auto title=self.display_title_;
    if(!self.playback_error_text_.empty()) title=self.playback_error_text_;
    else if(const auto* track=self.PlaybackTrackForUi()) title=self.PlaylistDisplayText(*track);
    if(title.empty()) title=self.DefaultPlayerTitle();
    wcsncpy_s(state->title,title.c_str(),_TRUNCATE);
    return TRUE;
    } catch(...) { return FALSE; }
}
BOOL WINAPI PlayerWindow::QuerySkinPluginTrack(void* context,uint32_t index,TtpSkinTrack* output) {
    if(!context || !output || output->size<sizeof(*output)) return FALSE;
    try {
    const auto& self=*static_cast<PlayerWindow*>(context);
    const auto* track=self.VisiblePlaylistTrack(index);if(!track) return FALSE;
    output->duration_ms=track->duration_ms;
    wcsncpy_s(output->title,self.PlaylistDisplayText(*track).c_str(),_TRUNCATE);
    return TRUE;
    } catch(...) { return FALSE; }
}
uint32_t WINAPI PlayerWindow::QuerySkinPluginSelection(void* context,uint32_t row) {
    if(!context) return 0;
    const auto& self=*static_cast<PlayerWindow*>(context);
    if(row>=self.VisiblePlaylistTrackCount()) return 0;
    return (self.playlist_selected_rows_.contains(row)?1u:0u) |
           (self.playlist_selection_ && *self.playlist_selection_==row?2u:0u);
}
void WINAPI PlayerWindow::PostSkinPluginCommand(void* context,uint32_t command,int32_t value) {
    const auto* self=static_cast<PlayerWindow*>(context);
    if(self && self->window_) PostMessageW(self->window_,RegisterWindowMessageW(TTP_SKIN_COMMAND_MESSAGE),command,value);
}
BOOL WINAPI PlayerWindow::HandleSkinPluginDrag(void* context,const TtpSkinDrag* event) {
    if (!context || !event || event->size < sizeof(*event)) return FALSE;
    auto& self = *static_cast<PlayerWindow*>(context);
    const HWND source = event->window;
    if (!source || (source != self.window_ && source != self.playlist_window_ &&
                    source != self.equalizer_window_ && source != self.lyric_window_)) return FALSE;
    if(source==self.lyric_window_ && event->phase!=TTP_SKIN_DRAG_END &&
       (!self.external_skin_ || !self.external_skin_->Handles(source))) return FALSE;
    try {
        // End must remain valid while LoadSkin/Detach has moved the instance
        // out of external_skin_. No callback here can unload or change a skin.
        if (event->phase == TTP_SKIN_DRAG_END) {
            if (self.skin_drag_window_ == source) self.EndSkinMouseCapture();
            return TRUE;
        }
        if (!self.external_skin_ || !IsWindow(source) ||
            GetWindowThreadProcessId(source, nullptr) != GetCurrentThreadId()) return FALSE;
        if (event->phase == TTP_SKIN_DRAG_BEGIN) {
            constexpr unsigned resize = kDragLeft | kDragRight | kDragTop | kDragBottom;
            const auto hit = event->edges;
            if (hit != kDragMove && (!hit || (hit & ~resize) ||
                event->minimum.cx <= 0 || event->minimum.cy <= 0)) return FALSE;
            self.BeginSkinBackgroundDrag(source, event->point, hit, event->minimum);
            return GetCapture() == source;
        }
        if (event->phase == TTP_SKIN_DRAG_MOVE && self.skin_drag_window_ == source) {
            self.ContinueSkinBackgroundDrag(source, event->point);
            return TRUE;
        }
    } catch (...) {
        if (self.skin_drag_window_ == source) self.EndSkinMouseCapture();
    }
    return FALSE;
}
BOOL WINAPI PlayerWindow::ResizeSkinPluginWindow(void* context,HWND source,SIZE size) {
    if(!context || !IsWindow(source) || IsIconic(source) ||
       size.cx<=0 || size.cy<=0 || size.cx>32767 || size.cy>32767) return FALSE;
    auto& self=*static_cast<PlayerWindow*>(context);
    if(!self.external_skin_ || (source!=self.window_ && source!=self.playlist_window_ &&
       source!=self.equalizer_window_ && !(source==self.lyric_window_ && self.external_skin_->Handles(source))) ||
       GetWindowThreadProcessId(source,nullptr)!=GetCurrentThreadId()) return FALSE;
    try {
        RECT before{},client{};
        if(!GetWindowRect(source,&before) || !GetClientRect(source,&client)) return FALSE;
        const int width=size.cx+(before.right-before.left)-(client.right-client.left);
        const int height=size.cy+(before.bottom-before.top)-(client.bottom-client.top);
        const int dy=height-(before.bottom-before.top);
        struct WindowBounds {HWND window;RECT bounds;};
        std::vector<WindowBounds> candidates,moving;
        if(dy && DecodePackedRuntimeOption(self.settings_.general.snap_windows,1,100).enabled) {
            for(const HWND window:self.RegisteredDragWindows()) {
                RECT bounds{};
                if(window!=source && !IsIconic(window) && GetWindowRect(window,&bounds) &&
                   bounds.top>=before.bottom && bounds.bottom>bounds.top && bounds.right>bounds.left)
                    candidates.push_back({window,bounds});
            }
            // Seed only actual contacts with the moving bottom edge. A nearby
            // window inside the magnetic distance has not yet been attached.
            // Then carry the connected group below that edge, including side
            // branches and hidden windows that will later be shown again.
            bool added=true;
            while(added) {
                added=false;
                for(auto it=candidates.begin();it!=candidates.end();) {
                    const auto& r=it->bounds;
                    bool connected=r.top==before.bottom && r.left<before.right && before.left<r.right;
                    for(const auto& member:moving)
                        if(AreDragWindowsAttached(member.bounds,r)) {connected=true;break;}
                    if(connected) {moving.push_back(*it);it=candidates.erase(it);added=true;}
                    else ++it;
                }
            }
        }
        constexpr UINT flags=SWP_NOZORDER|SWP_NOACTIVATE;
        HDWP batch=BeginDeferWindowPos(static_cast<int>(moving.size()+1));
        if(batch) batch=DeferWindowPos(batch,source,nullptr,before.left,before.top,width,height,flags);
        if(batch) for(const auto& member:moving) {
            batch=DeferWindowPos(batch,member.window,nullptr,member.bounds.left,member.bounds.top+dy,0,0,flags|SWP_NOSIZE);
            if(!batch) break;
        }
        if(batch && EndDeferWindowPos(batch)) return TRUE;
        // Use the original snapshot even if a deferred update partially ran;
        // applying a second relative translation would move followers twice.
        if(!SetWindowPos(source,nullptr,before.left,before.top,width,height,flags)) return FALSE;
        for(const auto& member:moving)
            SetWindowPos(member.window,nullptr,member.bounds.left,member.bounds.top+dy,0,0,flags|SWP_NOSIZE);
        return TRUE;
    } catch(...) {return FALSE;}
}
BOOL WINAPI PlayerWindow::HandleSkinPluginContentInput(void* context,const TtpSkinContent* content,
                                                       const MSG* event,LRESULT* result) {
    if(!context || !content || content->size<sizeof(*content) || !event || !result ||
       content->mode<1 || content->mode>3 || content->visual_type>4) return FALSE;
    auto& self=*static_cast<PlayerWindow*>(context);
    if(event->hwnd!=content->window || content->window!=self.lyric_window_) return FALSE;
    *result=0;
    const UINT message=event->message;
    const HWND window=content->window;
    RECT visual{},lyric{};bool overlay{};
    self.SkinPluginContentRects(content->bounds,content->mode,content->visual_type,visual,lyric,overlay);
    const auto cancel=[&] {
        if(self.plugin_content_drag_window_==window) {
            self.plugin_content_drag_window_=nullptr;
            self.HandleLyricControlMessage(window,WM_CANCELMODE,0,0,true);
        }
    };
    if(message==WM_SIZE) {
        cancel();
        if(self.lyric_editor_) {
            self.LayoutLyricEditor(&lyric);
            const int show=IsRectEmpty(&lyric)?SW_HIDE:SW_SHOWNOACTIVATE;
            ShowWindow(self.lyric_editor_,show);
            if(self.lyric_editor_toolbar_) ShowWindow(self.lyric_editor_toolbar_,show);
        }
        return FALSE;
    }
    if(message==WM_CAPTURECHANGED || message==WM_CANCELMODE ||
       (message==WM_SHOWWINDOW && !event->wParam) || (message==WM_ENABLE && !event->wParam)) {
        cancel();return FALSE;
    }
    if(message==WM_RBUTTONUP || message==WM_CONTEXTMENU) {
        cancel();
        POINT point{GET_X_LPARAM(event->lParam),GET_Y_LPARAM(event->lParam)};
        if(message==WM_RBUTTONUP) ClientToScreen(window,&point);
        self.plugin_content_menu_point_=point;
        self.plugin_content_menu_point_valid_=message!=WM_CONTEXTMENU || event->lParam!=LPARAM(-1);
        return FALSE; // The DLL queues its menu request; no modal loop on its stack.
    }
    if(self.fullscreen_mode_ || self.lyric_editor_) return FALSE;
    POINT point{GET_X_LPARAM(event->lParam),GET_Y_LPARAM(event->lParam)};
    if(message==WM_SETCURSOR) {GetCursorPos(&point);ScreenToClient(window,&point);}
    if(message==WM_MOUSEWHEEL) ScreenToClient(window,&point);
    const bool dragging=self.plugin_content_drag_window_==window && self.lyric_line_dragging_;
    const bool over_lyric=PtInRect(&lyric,point)!=FALSE;
    const bool over_content=PtInRect(&content->bounds,point)!=FALSE;
    switch(message) {
    case WM_LBUTTONDOWN:
        if(!over_content) return FALSE;
        SetFocus(window);
        if(!over_lyric) return TRUE;
        self.plugin_content_drag_window_=window;
        break;
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
        if(!dragging && !over_content) return FALSE;
        if(!dragging && !over_lyric) return TRUE;
        break;
    case WM_LBUTTONDBLCLK:
        return over_content; // Only menus/frame controls may change content.
    case WM_MOUSEWHEEL:
        if(!over_lyric) return FALSE;
        // A lyric wheel never falls through to the DLL's volume adjustment.
        if(!self.settings_.lyric.mouse_wheel_adjust) return TRUE;
        break;
    case WM_SETCURSOR:
        if((!dragging && !over_lyric) || reinterpret_cast<HWND>(event->wParam)!=window ||
           LOWORD(event->lParam)!=HTCLIENT) return FALSE;
        break;
    case WM_KEYDOWN:
        if(IsRectEmpty(&lyric) || (event->wParam!=VK_UP && event->wParam!=VK_DOWN &&
           event->wParam!=VK_LEFT && event->wParam!=VK_RIGHT && event->wParam!=VK_ESCAPE)) return FALSE;
        break;
    case WM_GETDLGCODE:
        if(IsRectEmpty(&lyric)) return FALSE;
        break;
    default:return FALSE;
    }
    *result=self.HandleLyricControlMessage(window,message,event->wParam,event->lParam,true);
    if(!self.lyric_line_dragging_) self.plugin_content_drag_window_=nullptr;
    InvalidateRect(window,nullptr,FALSE);
    return TRUE;
}

namespace {
constexpr UINT kContentProviderMenuFirst=0xf000;
void RemapContentMenu(HMENU menu,std::vector<UINT>& commands) {
    for(int i=0;i<GetMenuItemCount(menu);++i) {
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID|MIIM_SUBMENU|MIIM_FTYPE;
        if(!GetMenuItemInfoW(menu,i,TRUE,&item)) continue;
        if(item.hSubMenu) RemapContentMenu(item.hSubMenu,commands);
        else if(!(item.fType&MFT_SEPARATOR)) {
            commands.push_back(item.wID);item.fMask=MIIM_ID;
            item.wID=kContentProviderMenuFirst+static_cast<UINT>(commands.size())-1;
            SetMenuItemInfoW(menu,i,TRUE,&item);
        }
    }
}
}

HMENU PlayerWindow::CreateSkinPluginContentMenu(const TtpSkinContent& content,std::vector<UINT>& commands) {
    commands.clear();
    HMENU popup{};
    if(content.mode==TTP_SKIN_CONTENT_VISUAL) popup=CreateVisualContextMenu(false,0,content.visual_type);
    else {
        popup=DetachFirstPopup(i18n::LoadMenu(ResourceModule(),MAKEINTRESOURCEW(
            lyric_editor_?kMenuLyricEditor:kMenuLyricDisplay)));
        if(popup) {
            if(lyric_editor_) PrepareLyricEditorMenu(popup);else PrepareLyricMenu(popup);
            if(content.mode==TTP_SKIN_CONTENT_COMBINED) {
                if(const HMENU visual=CreateVisualContextMenu(false,0,content.visual_type))
                    AppendMenuW(popup,MF_POPUP,reinterpret_cast<UINT_PTR>(visual),i18n::Literal(L"视觉效果"));
            }
        }
    }
    if(!popup) return nullptr;
    if(const HMENU provider=external_skin_->Menu(content.window)) {
        RemapContentMenu(provider,commands);
        AppendMenuW(popup,MF_SEPARATOR,0,nullptr);
        AppendMenuW(popup,MF_POPUP,reinterpret_cast<UINT_PTR>(provider),i18n::Literal(L"显示内容"));
    }
    return popup;
}

void PlayerWindow::HandleSkinPluginContentMenuCommand(UINT command,TtpSkinContent content) {
    if(command>=kCmdVisualFirst && command<=kCmdVisualLast) {
        content.visual_type=command-kCmdVisualFirst;
        external_skin_->ContentState(content,true);
    } else if(command>=kCmdFullscreenLyrics && command<=kCmdFullscreenAll) {
        HandleSkinPluginCommand(TTP_SKIN_CONTENT_FULLSCREEN,
            (command-kCmdFullscreenLyrics+1)|(content.visual_type<<8));
    } else if(!HandleLyricCommand(command)) HandleContextCommand(command,content.window);
}

void PlayerWindow::ShowSkinPluginContentMenu(bool keyboard) {
    if(context_menu_open_ || !external_skin_ || !external_skin_->Handles(lyric_window_) || !IsWindowEnabled(window_)) return;
    auto* const instance=external_skin_.get();
    const auto* provider=external_skin_->Provider();
    TtpSkinContent content{sizeof(content),lyric_window_};
    const bool native=external_skin_->ContentState(content);
    std::vector<UINT> commands;
    const HMENU popup=native?CreateSkinPluginContentMenu(content,commands):external_skin_->Menu(lyric_window_);
    if(!popup) return;
    POINT point=plugin_content_menu_point_;
    if(!plugin_content_menu_point_valid_) GetCursorPos(&point);
    plugin_content_menu_point_valid_=false;
    if(keyboard) {point={content.bounds.left,content.bounds.top};ClientToScreen(lyric_window_,&point);}
    const HWND owner=lyric_window_;
    context_menu_open_=true;SetForegroundWindow(owner);BeginPopupMenuStyle(popup,true);
    const UINT selected=TrackPlayerPopupMenuEx(popup,TPM_RIGHTBUTTON|TPM_RETURNCMD|TPM_NONOTIFY,
        point.x,point.y,owner,nullptr);
    EndPopupMenuStyle();DestroyMenu(popup);context_menu_open_=false;
    if(selected && external_skin_.get()==instance && external_skin_->Provider()==provider) {
        if(!native) external_skin_->Menu(owner,selected);
        else if(selected>=kContentProviderMenuFirst && selected-kContentProviderMenuFirst<commands.size())
            external_skin_->Menu(owner,commands[selected-kContentProviderMenuFirst]);
        else HandleSkinPluginContentMenuCommand(selected,content);
    }
    PostMessageW(owner,WM_NULL,0,0);
}

void PlayerWindow::HandleSkinPluginCommand(uint32_t command,int32_t value) {
    if(!external_skin_ || close_after_skin_window_fade_) return;
    switch(command) {
    case TTP_SKIN_PLAY: InvokeSkinAction(L"play");break;
    case TTP_SKIN_PAUSE: InvokeSkinAction(L"pause");break;
    case TTP_SKIN_STOP: Stop();break;
    case TTP_SKIN_PREVIOUS: SelectRelative(false);break;
    case TTP_SKIN_NEXT: SelectRelative(true);break;
    case TTP_SKIN_OPEN: ChooseFiles();break;
    case TTP_SKIN_CLOSE: PostMessageW(window_,WM_CLOSE,0,0);break;
    case TTP_SKIN_MINIMIZE: ShowWindow(window_,SW_MINIMIZE);break;
    case TTP_SKIN_PLAYLIST: TogglePlaylistWindow();break;
    case TTP_SKIN_EQUALIZER: ToggleEqualizerWindow();break;
    case TTP_SKIN_LYRICS: ToggleLyricWindow();break;
    case TTP_SKIN_CONTENT_FULLSCREEN:
        if(external_skin_->Handles(lyric_window_) && (value&255)>=1 && (value&255)<=3 &&
           ((value>>8)&255)<=4 && audio_->State()==audio::PlaybackState::playing) {
            if(fullscreen_mode_==0) plugin_content_fullscreen_saved_type_=settings_.visual.type;
            SetFullScreenMode(value&255,lyric_window_,(value>>8)&255);
        }
        break;
    case TTP_SKIN_CONTENT_MENU: ShowSkinPluginContentMenu(value==1);break;
    case TTP_SKIN_MENU: {POINT p{};GetCursorPos(&p);ShowContextMenu(p);break;}
    case TTP_SKIN_OPTIONS: ShowOptions();break;
    case TTP_SKIN_VOLUME:
        settings_.player.volume=std::clamp(value,0,100);settings_.player.mute=false;
        audio_->SetVolume(float(settings_.player.volume)/100.0F);break;
    case TTP_SKIN_BALANCE:
        settings_.player.balance=std::clamp(value,-100,100);audio_->SetBalance(settings_.player.balance);break;
    case TTP_SKIN_SEEK:
        // The DLL previews while dragging and commits only on release, just
        // like the native progress control. Do not start a seek transition.
        if(audio_->Duration().count()>0) audio_->SeekWithoutFade(std::chrono::milliseconds(audio_->Duration().count()*std::clamp(value,0,10000)/10000));
        break;
    case TTP_SKIN_MODE: SetPlaybackMode(value);break;
    case TTP_SKIN_PLAY_ROW:
        if(value>=0 && static_cast<size_t>(value)<VisiblePlaylistTrackCount()) SelectNavigationTrack(static_cast<size_t>(value));
        break;
    case TTP_SKIN_REMOVE_ROW:
        if(value>=0 && static_cast<size_t>(value)<VisiblePlaylistTrackCount()) {
            playlist_selected_rows_={static_cast<size_t>(value)};DeleteSelectedPlaylistRows();
        }
        break;
    case TTP_SKIN_SELECT_ROW: case TTP_SKIN_TOGGLE_ROW:
    case TTP_SKIN_EXTEND_ROW: case TTP_SKIN_EXTEND_TOGGLE_ROW:
        if(value>=0 && static_cast<size_t>(value)<VisiblePlaylistTrackCount())
            SelectPlaylistRow(static_cast<size_t>(value),
                command==TTP_SKIN_TOGGLE_ROW || command==TTP_SKIN_EXTEND_TOGGLE_ROW,
                command==TTP_SKIN_EXTEND_ROW || command==TTP_SKIN_EXTEND_TOGGLE_ROW);
        break;
    case TTP_SKIN_MOVE_SELECTION: case TTP_SKIN_COPY_SELECTION:
        if(value>=0) ReorderSelectedPlaylistRows(static_cast<size_t>(value),command==TTP_SKIN_COPY_SELECTION);
        break;
    case TTP_SKIN_DELETE_SELECTED: DeleteSelectedPlaylistRows();break;
    case TTP_SKIN_SELECT_ALL: HandlePlaylistCommand(kPlaylistSelectAll);break;
    case TTP_SKIN_LIST_TOOLBAR: {
        POINT p{};GetCursorPos(&p);
        if(value>=0 && value<7) InvokePlaylistToolbar(static_cast<size_t>(value),p);
        break;
    }
    case TTP_SKIN_LIST_MENU: {
        // The plugin supplies the row hit. Native controls have a different
        // layout, so do not call its coordinate-based context-menu handler.
        if(value>=0 && static_cast<size_t>(value)<VisiblePlaylistTrackCount() &&
            !playlist_selected_rows_.contains(static_cast<size_t>(value)))
            SelectPlaylistRow(static_cast<size_t>(value),false,false,
                PlaylistSelectionTrigger::selection_changed);
        playlist_send_to_catalog_.Clear();
        HMENU menu = value<0
            ? ConvertMenuBarToPopup(LoadMenuW(ResourceModule(),MAKEINTRESOURCEW(kMenuPlaylistToolbar)))
            : DetachPopup(LoadMenuW(ResourceModule(),MAKEINTRESOURCEW(
                playlist_selected_rows_.size()>1?kMenuPlaylistItems:kMenuPlaylistItem)),0);
        if(menu) {
            PreparePlaylistMenu(menu);BeginPopupMenuStyle(menu);
            POINT p{};GetCursorPos(&p);
            const UINT selected=TrackPlayerPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,
                p.x,p.y,0,playlist_window_,nullptr);
            EndPopupMenuStyle();DestroyMenu(menu);
            if(selected && !HandlePlaylistCommand(selected)) HandleContextCommand(selected);
        }
        playlist_send_to_catalog_.Clear();break;
    }
    case TTP_SKIN_PROPERTIES: HandleContextCommand(kCmdFileProperties);break;
    case TTP_SKIN_ALWAYS_ON_TOP: HandleContextCommand(kCmdAlwaysOnTop);break;
    case TTP_SKIN_VISUAL_NEXT: SetVisualType((settings_.visual.type+1)%5);break;
    case TTP_SKIN_VISUAL_MENU: {POINT p{};GetCursorPos(&p);ShowVisualContextMenu(p);break;}
    case TTP_SKIN_EQ_BANDS:
        for(int i=0;i<10;++i) SetEqualizerSliderValue(kEqSliderFirstBand+i,std::clamp(value,-12,12),true);
        break;
    case TTP_SKIN_EQ_ENABLE: HandleEqualizerCommand(kEqCommandEnable);break;
    case TTP_SKIN_EQ_PRESETS: {POINT p{};GetCursorPos(&p);ShowEqualizerProfileMenu(p);break;}
    case TTP_SKIN_TIME_MODE: HandleContextCommand(kCmdShowElapsedTime);break;
    default:
        if(command>=TTP_SKIN_EQ_VALUE && command<TTP_SKIN_EQ_VALUE+11)
            SetEqualizerSliderValue(command==TTP_SKIN_EQ_VALUE?kEqSliderPreamp:kEqSliderFirstBand+int(command-TTP_SKIN_EQ_VALUE-1),value,true);
        break;
    }
    RefreshPlaybackUi();
}
}
