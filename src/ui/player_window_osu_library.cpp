#include "player_window_internal.h"
#include "ttplayer/audio/osu_library.h"
#include "ttplayer/audio/osz_archive.h"
#include "ttplayer/core/text.h"
#include "ttplayer/i18n/i18n.h"
#include "../core/environment_path.h"
#include "track_metadata.h"
#include "modern_file_dialog.h"
#include <commctrl.h>
#include <mutex>

namespace ttplayer::ui {
using namespace detail;
struct PlayerWindow::OsuLibraryState {
    struct Job {
        std::atomic_bool canceled{};
        std::mutex mutex;
        bool ready{},applied{},interactive{};
        HRESULT status{E_PENDING};
        std::filesystem::path path;
        std::vector<playlist::Track> tracks;
    };
    std::array<std::shared_ptr<Job>,2> jobs;
};

namespace {
struct OsuExportProgress {
    std::atomic_bool canceled{};
    std::mutex mutex;
    bool ready{};
    HRESULT status{E_PENDING};
    uint64_t done{},total{};
    size_t completed{};
    std::wstring current;
};
INT_PTR CALLBACK OsuExportProgressProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* state=reinterpret_cast<OsuExportProgress*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        state=reinterpret_cast<OsuExportProgress*>(lparam);SetWindowLongPtrW(window,DWLP_USER,lparam);
        SetWindowTextW(window,i18n::Literal(L"导出osz文件"));
        const auto add=[&](const wchar_t* type,const wchar_t* text,DWORD style,int id,RECT rect) {
            MapDialogRect(window,&rect);
            const HWND child=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,rect.left,rect.top,
                rect.right-rect.left,rect.bottom-rect.top,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return child;
        };
        add(L"STATIC",i18n::Literal(L"正在收集谱面及资源…"),SS_PATHELLIPSIS,100,{10,10,285,35});
        add(PROGRESS_CLASSW,L"",0,101,{10,38,285,50});
        add(L"STATIC",L"",0,102,{10,58,210,72});
        const auto button=add(L"BUTTON",i18n::Literal(L"取消"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDCANCEL,{230,56,285,73});
        SendDlgItemMessageW(window,101,PBM_SETRANGE32,0,1000);
        RECT bounds{},owner{};GetWindowRect(window,&bounds);
        GetWindowRect(GetParent(window),&owner);
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(GetParent(window),MONITOR_DEFAULTTONEAREST),&monitor);
        const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
        const int x=std::max(monitor.rcWork.left,std::min((owner.left+owner.right-width)/2,monitor.rcWork.right-width));
        const int y=std::max(monitor.rcWork.top,std::min((owner.top+owner.bottom-height)/2,monitor.rcWork.bottom-height));
        SetWindowPos(window,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetTimer(window,1,100,nullptr);SetFocus(button);return FALSE;
    }
    if(!state)return FALSE;
    if((message==WM_COMMAND && LOWORD(wparam)==IDCANCEL) || message==WM_CLOSE) {
        state->canceled=true;EnableWindow(GetDlgItem(window,IDCANCEL),FALSE);
        SetDlgItemTextW(window,100,i18n::Literal(L"正在取消…"));return TRUE;
    }
    if(message==WM_TIMER) {
        std::lock_guard lock(state->mutex);
        if(state->ready){EndDialog(window,IDOK);return TRUE;}
        if(!state->canceled)SetDlgItemTextW(window,100,state->current.empty()?i18n::Literal(L"正在收集谱面及资源…"):state->current.c_str());
        const auto text=i18n::Text(L"已导出谱面包：")+std::to_wstring(state->completed);
        SetDlgItemTextW(window,102,text.c_str());
        const auto position=state->total?static_cast<WPARAM>(1000.0*static_cast<double>(state->done)/static_cast<double>(state->total)):0;
        SendDlgItemMessageW(window,101,PBM_SETPOS,position,0);return TRUE;
    }
    if(message==WM_DESTROY){state->canceled=true;KillTimer(window,1);}
    return FALSE;
}
std::wstring OsuMetadata(const playlist::Track& track,const char* key) {
    for(const auto& field:track.metadata)if(field.first==key)return core::Utf8ToWide(field.second);
    return {};
}
}

void PlayerWindow::AppendOsuExportMenu(HMENU menu,bool has_row) const {
    if(!menu || settings_.playlist.library_mode || !playlists_.IsExternal(playlists_.ActiveIndex()) ||
       !audio::HasOsuExportSupport())return;
    const UINT disabled=osu_exporting_?MF_GRAYED:0;
    int position=GetMenuItemCount(menu);
    for(int i=0;i<position;++i)if(GetMenuItemID(menu,i)==kCmdPlaylistOptions){position=i;break;}
    const bool before_options=position<GetMenuItemCount(menu);
    if(position>0 && !(GetMenuState(menu,position-1,MF_BYPOSITION)&MF_SEPARATOR))
        InsertMenuW(menu,position++,MF_BYPOSITION|MF_SEPARATOR,0,nullptr);
    if(has_row)InsertMenuW(menu,position++,MF_BYPOSITION|MF_STRING|disabled|(playlist_selected_rows_.empty()?MF_GRAYED:0),kPlaylistExportOsu,
        i18n::Literal(playlist_selected_rows_.size()>1?L"导出已选歌曲":L"导出osz文件"));
    InsertMenuW(menu,position++,MF_BYPOSITION|MF_STRING|disabled|(VisiblePlaylistTrackCount()?0:MF_GRAYED),kPlaylistExportAllOsu,i18n::Literal(L"导出全部歌曲"));
    if(before_options)InsertMenuW(menu,position,MF_BYPOSITION|MF_SEPARATOR,0,nullptr);
}

void PlayerWindow::ExportOsuSongs(bool all) {
    if(osu_exporting_ || settings_.playlist.library_mode || !playlists_.IsExternal(playlists_.ActiveIndex()) ||
       !audio::HasOsuExportSupport())return;
    osu_exporting_=true;
    struct Reset {bool& flag;~Reset(){flag=false;}} reset{osu_exporting_};
    const HWND owner=playlist_window_?playlist_window_:window_;
    try {
        const unsigned source=playlists_.Entries()[playlists_.ActiveIndex()].external_kind;
        std::vector<std::wstring> identities;std::filesystem::path root;std::wstring suggested;
        for(size_t row=0;row<VisiblePlaylistTrackCount();++row) {
            if(!all&&!playlist_selected_rows_.contains(row))continue;
            const auto* track=VisiblePlaylistTrack(row);if(!track)continue;
            auto identity=OsuMetadata(*track,"TTPlayer.Osu.Identity");
            if(identity.empty())throw std::runtime_error("Missing beatmap identity");
            const auto record_root=OsuMetadata(*track,"TTPlayer.Osu.Root");
            if(!record_root.empty()) {
                if(!root.empty()&&root!=record_root)throw std::runtime_error("Mixed library roots");
                root=record_root;
            }
            identities.push_back(std::move(identity));
            if(suggested.empty())suggested=core::Utf8ToWide(track->artist.empty()?track->title:track->artist+" - "+track->title);
        }
        if(identities.empty())return;
        if(root.empty())root=core::ExpandEnvironmentPath(source==1?settings_.osu_library.stable_songs:settings_.osu_library.lazer_files);
        const bool single=!all && identities.size()==1;
        auto initial=std::filesystem::path(core::ExpandEnvironmentPath(L"%USERPROFILE%\\Downloads"));
        if(!std::filesystem::is_directory(initial))initial=core::ExpandEnvironmentPath(L"%USERPROFILE%");
        std::filesystem::path destination;
        if(single) {
            for(auto& ch:suggested)if(ch<32||wcschr(L"<>:\"/\\|?*",ch))ch=L'_';
            while(!suggested.empty()&&(suggested.back()==L'.'||suggested.back()==L' '))suggested.pop_back();
            if(suggested.empty())suggested=L"beatmap";
            if(suggested.size()>160)suggested.resize(160);
            ModernSaveFileOptions dialog;dialog.owner=owner;dialog.title=i18n::Text(L"导出osz文件");
            dialog.filters={{L"osu! 谱面包 (*.osz)",L"*.osz"}};dialog.default_extension=L"osz";
            dialog.initial_path=initial/(suggested+L".osz");
            const auto selected=ModernSaveFile(dialog);if(!selected)return;destination=*selected;
        }else {
            ModernFolderOptions dialog;dialog.owner=owner;dialog.initial_path=initial;
            dialog.title=i18n::Text(all?L"导出全部歌曲 — 选择保存文件夹":L"导出已选歌曲 — 选择保存文件夹");
            const auto selected=ModernPickFolder(dialog);if(!selected)return;destination=selected->path;
        }
        OsuExportProgress progress;
        std::thread worker([&] {
            const auto result=audio::ExportOsuLibrary(source,root,identities,destination,single,single,
                [&]{return progress.canceled.load();},
                [&](uint32_t phase,uint64_t done,uint64_t total,const wchar_t* name) {
                    std::lock_guard lock(progress.mutex);progress.current=name;
                    progress.done=phase==6?done:0;progress.total=phase==6?total:0;
                },[&](const std::filesystem::path&) {std::lock_guard lock(progress.mutex);++progress.completed;});
            std::lock_guard lock(progress.mutex);progress.status=result;progress.ready=true;
        });
        // A modal UI loop keeps painting/playback responsive. The worker owns
        // only captured values and progress state, never PlayerWindow or HWNDs.
        struct Template {DLGTEMPLATE dialog;WORD menu,cls,title;};
        const Template layout{{WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME,0,0,0,0,295,83},0,0,0};
        const auto result=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&layout.dialog,owner,OsuExportProgressProc,reinterpret_cast<LPARAM>(&progress));
        if(result==-1)progress.canceled=true;
        worker.join();
        const bool canceled=progress.status==HRESULT_FROM_WIN32(ERROR_CANCELLED);
        auto message=i18n::Text(SUCCEEDED(progress.status)?L"导出完成。":canceled?L"已取消导出。":L"导出失败。")+
            L"\r\n"+i18n::Text(L"已导出谱面包：")+std::to_wstring(progress.completed)+L"\r\n"+destination.native();
        if(FAILED(progress.status)&&!canceled) {
            wchar_t code[32]{};swprintf_s(code,L"\r\n0x%08X",static_cast<unsigned>(progress.status));message+=code;
            wchar_t error[1024]{};
            if(HRESULT_FACILITY(progress.status)==FACILITY_WIN32 && FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,HRESULT_CODE(progress.status),0,error,1024,nullptr))message+=L"\r\n"+std::wstring(error);
            if(source==2)message+=L"\r\n"+i18n::Text(L"请检查资源是否完整；如果 osu!lazer 正在写入曲库，请关闭后重试。");
        }
        MessageBoxW(owner,message.c_str(),i18n::Literal(L"导出osz文件"),MB_OK|(FAILED(progress.status)&&!canceled?MB_ICONWARNING:MB_ICONINFORMATION));
    }catch(...) {MessageBoxW(owner,i18n::Literal(L"无法准备导出，请重新读取歌曲列表后重试。"),i18n::Literal(L"导出osz文件"),MB_OK|MB_ICONWARNING);}
}

void PlayerWindow::AppendOsuLibraryMenu(HMENU menu) const {
    if(!menu || !audio::HasOszSupport())return;
    if(GetMenuState(menu,kPlaylistShowOsu,MF_BYCOMMAND)!=UINT(-1))return;
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    const UINT available=audio::HasOsuLibrarySupport()?0:MF_GRAYED;
    AppendMenuW(menu,MF_STRING|available|(settings_.osu_library.show_stable?MF_CHECKED:0),
        kPlaylistShowOsu,i18n::Literal(L"显示osu列表"));
    AppendMenuW(menu,MF_STRING|available|(settings_.osu_library.show_lazer?MF_CHECKED:0),
        kPlaylistShowOsuLazer,i18n::Literal(L"显示osu!lazer列表"));
}

void PlayerWindow::SetOsuLibraryVisible(unsigned source,bool visible,bool interactive) {
    if(source<1 || source>2 || !audio::HasOsuLibrarySupport())return;
    auto& enabled=source==1?settings_.osu_library.show_stable:settings_.osu_library.show_lazer;
    enabled=visible;window_state_saved_=false;
    if(!visible) {
        if(osu_libraries_ && osu_libraries_->jobs[source-1]) {
            osu_libraries_->jobs[source-1]->canceled=true;
            osu_libraries_->jobs[source-1].reset();
        }
        if(const auto row=playlists_.ExternalListIndex(source)) {
            // Hiding a catalogue must not interrupt its current song or queue.
            if(playing_playlist_index_==row) {
                media_library_playback_=playlists_.At(*row);
                media_library_playback_active_=true;
                playing_playlist_index_.reset();
            } else if(playing_playlist_index_ && *playing_playlist_index_>*row)--*playing_playlist_index_;
            playlists_.RemoveExternalList(source);
            playlist_context_list_.reset();playlist_list_hover_.reset();
            playlist_list_selection_=playlist_list_focus_=playlists_.ActiveIndex();
            playlist_selected_rows_.clear();playlist_selection_.reset();playlist_scroll_=0;
            RestorePlaylistRowSelection();
        }
        RefreshPlaylist();return;
    }
    RefreshOsuLibraries(interactive);
}

void PlayerWindow::RefreshOsuLibraries(bool interactive) {
    if(!window_ || !IsWindow(window_) || close_after_skin_window_fade_ ||
       !audio::HasOsuLibrarySupport())return;
    if(!osu_libraries_)osu_libraries_=std::make_shared<OsuLibraryState>();
    for(unsigned source=1;source<=2;++source) {
        if(!(source==1?settings_.osu_library.show_stable:settings_.osu_library.show_lazer))continue;
        const auto raw=source==1?settings_.osu_library.stable_songs:settings_.osu_library.lazer_files;
        const auto path=std::filesystem::path(core::ExpandEnvironmentPath(raw));
        auto& job=osu_libraries_->jobs[source-1];
        if(job && job->path==path)continue;
        if(job)job->canceled=true;
        const auto index=playlists_.EnsureExternalList(source,source==1?L"osu!":L"osu!lazer");
        if(index==playlist::PlaylistStore::npos)continue;
        playlists_.At(index).SetTitle(source==1?L"osu!（读取中）":L"osu!lazer（读取中）");
        job=std::make_shared<OsuLibraryState::Job>();job->path=path;job->interactive=interactive;
        const auto receiver=window_;
        try {
            std::thread([work=job,receiver,source] {
                std::vector<playlist::Track> tracks;
                HRESULT status=E_FAIL;
                try {
                    std::vector<audio::OsuLibraryMedia> records;
                    status=work->path.empty()?HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND):
                        audio::ReadOsuLibrary(source,work->path,records,[&]{return work->canceled.load();});
                    if(SUCCEEDED(status))for(const auto& entry:records) {
                        if(work->canceled){status=HRESULT_FROM_WIN32(ERROR_CANCELLED);break;}
                        playlist::Track track;track.path=entry.audio;
                        track.title=core::WideToUtf8(entry.title);track.artist=core::WideToUtf8(entry.artist);
                        track.album=core::WideToUtf8(entry.album);
                        track.duration_ms=entry.duration_ms?static_cast<int>(std::min<uint64_t>(entry.duration_ms,INT_MAX)):-2;
                        track.metadata={{"TTPlayer.Osu.Source",std::to_string(source)},
                            {"TTPlayer.Osu.Identity",core::WideToUtf8(entry.identity)},
                            {"TTPlayer.Osu.Root",core::WideToUtf8(work->path.native())},
                            {"TTPlayer.Osu.Title",track.title},{"TTPlayer.Osu.Artist",track.artist},{"TTPlayer.Osu.Album",track.album},
                            {"TTPlayer.Osu.Beatmap",core::WideToUtf8(entry.beatmap.native())},
                            {"TTPlayer.Osu.Background",core::WideToUtf8(entry.background.native())}};
                        RestoreOsuTrackMetadata(track);
                        tracks.push_back(std::move(track));
                    }
                }catch(const std::bad_alloc&){status=E_OUTOFMEMORY;}catch(...){status=E_FAIL;}
                const std::lock_guard lock(work->mutex);
                if(work->canceled)return;
                work->status=status;work->tracks=std::move(tracks);work->ready=true;
                PostMessageW(receiver,kMsgOsuLibraryReady,0,0);
            }).detach();
        }catch(...) {const std::lock_guard lock(job->mutex);job->status=E_OUTOFMEMORY;job->ready=true;PostMessageW(window_,kMsgOsuLibraryReady,0,0);}
    }
    RefreshPlaylist();
}

void PlayerWindow::ApplyOsuLibraryResults() {
    if(!osu_libraries_)return;
    for(unsigned source=1;source<=2;++source) {
        const auto job=osu_libraries_->jobs[source-1];if(!job)continue;
        std::unique_lock lock(job->mutex);
        if(!job->ready || job->applied || job->canceled)continue;
        job->applied=true;
        const auto index=playlists_.ExternalListIndex(source);if(!index)continue;
        const auto status=job->status;
        auto tracks=std::move(job->tracks);lock.unlock();
        auto& list=playlists_.At(*index);
        list.SetTitle(source==1?L"osu!":L"osu!lazer");
        if(SUCCEEDED(status)) {
            // Preserve the old playback queue if settings refreshed its source.
            if(playing_playlist_index_==index) {
                media_library_playback_=list;media_library_playback_active_=true;playing_playlist_index_.reset();
            }
            list.Clear();list.InsertRange(0,std::move(tracks));
            if(playlists_.ActiveIndex()==*index) {
                playlist_selected_rows_.clear();playlist_selection_.reset();playlist_scroll_=0;
                RestorePlaylistRowSelection();
            }
        } else {
            list.SetTitle(source==1?L"osu!（读取失败）":L"osu!lazer（读取失败）");
            if(job->interactive && status!=HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                wchar_t code[24]{};swprintf_s(code,L"0x%08X",static_cast<unsigned>(status));
                auto message=i18n::Text(L"无法读取歌曲列表，请检查媒体库 → osu! 中的路径。")+L"\r\n\r\n"+job->path.native()+L"\r\n"+code;
                if(source==2)message+=L"\r\n"+i18n::Text(L"如果 osu!lazer 正在运行，请关闭后取消勾选并重新显示列表。");
                MessageBoxW(playlist_window_?playlist_window_:window_,message.c_str(),L"千千静听",MB_OK|MB_ICONWARNING);
            }
        }
    }
    RefreshPlaylist();
}

void PlayerWindow::ShutdownOsuLibraries() {
    if(!osu_libraries_)return;
    for(const auto& job:osu_libraries_->jobs)if(job)job->canceled=true;
    osu_libraries_.reset();
}
}
