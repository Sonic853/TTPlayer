#include "player_window_internal.h"
#include "ttplayer/audio/osu_library.h"
#include "ttplayer/audio/osz_archive.h"
#include "ttplayer/core/text.h"
#include "../settings/osu_storage_path.h"
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
        const auto path=std::filesystem::path(settings::detail::ExpandOsuPath(raw));
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
                            {"TTPlayer.Osu.Title",track.title},{"TTPlayer.Osu.Artist",track.artist},{"TTPlayer.Osu.Album",track.album},
                            {"TTPlayer.Osu.Beatmap",core::WideToUtf8(entry.beatmap.native())},
                            {"TTPlayer.Osu.Background",core::WideToUtf8(entry.background.native())}};
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
