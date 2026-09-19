#include "ttplayer/ui/player_window.h"
#include "ttplayer/ui/wtl_menu.h"
#include "player_window_internal.h"
#include "ttplayer/skin/skin_paths.h"
#include <algorithm>
#include <limits>

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
            QuerySkinPluginSelection,PaintSkinPluginVisual};
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
            if(mini_mode_) {ToggleMiniMode();CompleteSkinWindowFadeForReplacement();}
        }
        auto previous=std::move(external_skin_);
        if(previous) previous->Detach();
        if (window_) {
            static_cast<void>(CreatePlaylistWindow(true));
            static_cast<void>(CreateEqualizerWindow(true));
        }
        if(window_ && !next->Attach(window_,playlist_window_,equalizer_window_)) {
            if(previous) previous->Attach(window_,playlist_window_,equalizer_window_);
            external_skin_=std::move(previous);return false;
        }
        external_skin_=std::move(next);
        settings_.plugin_skin_file=skin::SkinPackageSelector(PlayerRuntimeDirectory()/L"Skin",path);
        settings_.player.mini_mode=false;
        if(window_) {RefreshPlaybackUi();InvalidateRect(window_,nullptr,FALSE);}
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
                    source != self.equalizer_window_)) return FALSE;
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
    case TTP_SKIN_MENU: {POINT p{};GetCursorPos(&p);ShowContextMenu(p);break;}
    case TTP_SKIN_OPTIONS: ShowOptions();break;
    case TTP_SKIN_VOLUME:
        settings_.player.volume=std::clamp(value,0,100);settings_.player.mute=false;
        audio_->SetVolume(float(settings_.player.volume)/100.0F);break;
    case TTP_SKIN_BALANCE:
        settings_.player.balance=std::clamp(value,-100,100);audio_->SetBalance(settings_.player.balance);break;
    case TTP_SKIN_SEEK:
        if(audio_->Duration().count()>0) audio_->Seek(std::chrono::milliseconds(audio_->Duration().count()*std::clamp(value,0,10000)/10000));
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
