#include "player_window_internal.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include "ttplayer/i18n/i18n.h"
#include "../app/resource_ids.h"

#include <algorithm>
#include <cwctype>

namespace ttplayer::ui {
namespace {
std::wstring PathKey(const std::filesystem::path& path) {
    std::error_code error;
    auto full=std::filesystem::absolute(path,error);
    auto key=(error?path:full).lexically_normal().wstring();
    std::transform(key.begin(),key.end(),key.begin(),towlower);
    return key;
}
bool SamePath(const std::filesystem::path& a,const std::filesystem::path& b) {
    return !a.empty() && !b.empty() && PathKey(a)==PathKey(b);
}
std::wstring SeenKey(const audio::ExtensionCorrection& c) {
    return PathKey(c.source)+L"|"+std::to_wstring(c.stamp.size)+L"|"+
        std::to_wstring(c.stamp.modified)+L"|"+std::to_wstring(c.stamp.file_id);
}
struct Prompt { std::wstring details; bool collision{}; };
INT_PTR CALLBACK CorrectionDialog(HWND dialog,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_INITDIALOG) {
        const auto& prompt=*reinterpret_cast<const Prompt*>(lparam);
        SetDlgItemTextW(dialog,IDC_EXTENSION_DETAILS,prompt.details.c_str());
        if (prompt.collision) {
            SetWindowTextW(dialog,i18n::Literal(L"更改后缀：目标文件已存在"));
            SetDlgItemTextW(dialog,IDOK,i18n::Literal(L"覆盖"));
        } else ShowWindow(GetDlgItem(dialog,IDC_EXTENSION_NUMBER),SW_HIDE);
        RECT owner{},rect{};
        GetWindowRect(GetParent(dialog),&owner); GetWindowRect(dialog,&rect);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(dialog,MONITOR_DEFAULTTONEAREST),&monitor);
        const int width=rect.right-rect.left,height=rect.bottom-rect.top;
        const int x=std::max(monitor.rcWork.left,std::min(monitor.rcWork.right-width,
            owner.left+(owner.right-owner.left-width)/2));
        const int y=std::max(monitor.rcWork.top,std::min(monitor.rcWork.bottom-height,
            owner.top+(owner.bottom-owner.top-height)/2));
        SetWindowPos(dialog,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER);
        SetFocus(GetDlgItem(dialog,IDCANCEL));
        return FALSE;
    }
    if (message==WM_COMMAND && (LOWORD(wparam)==IDOK || LOWORD(wparam)==IDCANCEL ||
        LOWORD(wparam)==IDC_EXTENSION_NUMBER)) {
        EndDialog(dialog,LOWORD(wparam)); return TRUE;
    }
    if (message==WM_CLOSE) { EndDialog(dialog,IDCANCEL); return TRUE; }
    return FALSE;
}
}

void PlayerWindow::CheckExtensionCorrection(const std::filesystem::path& path,int subtrack) {
    // Only the successful PlayCurrent path calls this. A later playback
    // request supersedes prompts deferred by an options/modal window.
    extension_corrections_.clear();
    if (!settings_.general.prompt_extension_correction) return;
    if (auto correction=audio::FindExtensionCorrection(path,subtrack))
        QueueExtensionCorrection(std::move(*correction));
}

void PlayerWindow::QueueExtensionCorrection(audio::ExtensionCorrection correction) {
    if (!settings_.general.prompt_extension_correction || !window_ ||
        close_after_skin_window_fade_ || extension_corrections_.size()>=128) return;
    if (extension_corrections_seen_.contains(SeenKey(correction))) return;
    if (std::any_of(extension_corrections_.begin(),extension_corrections_.end(),
        [&](const auto& queued){return SamePath(queued.source,correction.source);})) return;
    extension_corrections_.push_back(std::move(correction));
    PostMessageW(window_,detail::kMsgExtensionCorrection,0,0);
}

void PlayerWindow::PromptExtensionCorrection() {
    if (!settings_.general.prompt_extension_correction) { extension_corrections_.clear(); return; }
    if (extension_correction_open_ || extension_corrections_.empty() ||
        !window_ || !IsWindowEnabled(window_) || close_after_skin_window_fade_ ||
        lyric_save_in_progress_ || options_window_) return;
    auto correction=std::move(extension_corrections_.front());
    extension_corrections_.pop_front();
    const auto playback_state=audio_->State();
    if (!playback_source_open_ || !opened_track_ || opened_track_->subtrack!=0 ||
        !SamePath(opened_track_->path,correction.source) ||
        (playback_state!=audio::PlaybackState::playing &&
         playback_state!=audio::PlaybackState::paused)) return;
    if (audio::ReadMediaFileStamp(correction.source)!=correction.stamp) return;
    // Stopped/skipped tracks must never display a deferred prompt merely
    // because they still exist in a playlist.
    const playlist::Track source=*opened_track_;
    if (extension_corrections_seen_.contains(SeenKey(correction))) return;
    extension_corrections_seen_.insert(SeenKey(correction));
    extension_correction_open_=true;
    struct Reset { bool& value; ~Reset(){value=false;} } reset{extension_correction_open_};

    const auto make_prompt=[&](const std::optional<audio::MediaFileStamp>& existing) {
        Prompt prompt;
        prompt.collision=existing.has_value();
        prompt.details=i18n::Text(L"识别格式：")+correction.format+L"\r\n\r\n"+
            i18n::Text(L"当前文件：")+correction.source.wstring()+L"\r\n"+
            i18n::Text(L"文件大小：")+audio::MediaFileSizeText(correction.stamp.size)+L"\r\n\r\n"+
            i18n::Text(L"更改为：")+correction.target.wstring();
        if (existing) {
            prompt.details+=L"\r\n"+i18n::Text(L"已有文件大小：")+audio::MediaFileSizeText(existing->size)+
                L"\r\n\r\n"+i18n::Text(L"覆盖将替换已有文件；加编号将保留两个文件，例如：")+
                correction.target.stem().wstring()+L" (1)"+correction.target.extension().wstring();
        } else prompt.details+=L"\r\n\r\n"+i18n::Text(L"是否更改为与实际格式对应的后缀？");
        return prompt;
    };
    auto destination=audio::ReadMediaFileStamp(correction.target);
    auto prompt=make_prompt(destination);
    auto choice=ShowWtlModalDialog(instance_,MAKEINTRESOURCEW(IDD_EXTENSION_CORRECTION),
        window_,CorrectionDialog,reinterpret_cast<LPARAM>(&prompt));
    if (choice!=IDOK && choice!=IDC_EXTENSION_NUMBER) return;
    // A destination may have appeared while the first confirmation was open.
    if (!destination) {
        destination=audio::ReadMediaFileStamp(correction.target);
        if (destination) {
            prompt=make_prompt(destination);
            choice=ShowWtlModalDialog(instance_,MAKEINTRESOURCEW(IDD_EXTENSION_CORRECTION),
                window_,CorrectionDialog,reinterpret_cast<LPARAM>(&prompt));
            if (choice!=IDOK && choice!=IDC_EXTENSION_NUMBER) return;
        }
    }
    if (!settings_.general.prompt_extension_correction || !IsWindow(window_) ||
        close_after_skin_window_fade_) return;
    const auto mode=choice==IDC_EXTENSION_NUMBER?audio::ExtensionCollision::number:
        destination?audio::ExtensionCollision::overwrite:audio::ExtensionCollision::fail;

    // Capture the live source AFTER the modal loop: playback can have advanced.
    const auto state=audio_->State();
    const bool active=state==audio::PlaybackState::playing || state==audio::PlaybackState::paused;
    const bool affected=opened_track_ && (SamePath(opened_track_->path,correction.source) ||
        (mode==audio::ExtensionCollision::overwrite && SamePath(opened_track_->path,correction.target)));
    const auto position=audio_->Position();
    if (affected) {
        CancelWaveTrackChange();
        pending_natural_play_=false;
        pending_failed_advance_=false;
        playback_was_active_=false;
        playback_source_open_=false;
        audio_->Stop(); // Release the reader before MoveFile; no fade tail here.
    }
    // An outgoing crossfade can still have a file handle open.
    PollFadingAudio(true);
    const auto result=audio::CorrectFileExtension(correction,mode,destination);
    if (!result.error) {
        for (size_t list=0;list<playlists_.Size();++list) {
            auto& playlist=playlists_.At(list);
            bool dirty{};
            for (size_t row=0;row<playlist.Tracks().size();++row) {
                const auto path=playlist.Tracks()[row].path;
                if (SamePath(path,correction.source)) dirty|=playlist.SetPath(row,result.target);
                else if (mode==audio::ExtensionCollision::overwrite && SamePath(path,result.target)) {
                    auto replacement=source;
                    replacement.path=result.target;
                    replacement.rating=playlist.Tracks()[row].rating;
                    replacement.subtrack=playlist.Tracks()[row].subtrack;
                    dirty|=playlist.SetTrack(row,std::move(replacement));
                }
            }
            if (dirty) playlists_.MarkDirty(list);
        }
        static_cast<void>(UpdateMediaLibraryTrackPath(source,result.target,
            mode==audio::ExtensionCollision::overwrite));
        for (size_t row=0;row<media_library_playback_.Tracks().size();++row)
            if (SamePath(media_library_playback_.Tracks()[row].path,correction.source))
                media_library_playback_.SetPath(row,result.target);
        if (opened_track_ && SamePath(opened_track_->path,correction.source)) opened_track_->path=result.target;
        else if (opened_track_ && mode==audio::ExtensionCollision::overwrite &&
            SamePath(opened_track_->path,result.target)) {
            const auto subtrack=opened_track_->subtrack;
            opened_track_=source; opened_track_->path=result.target; opened_track_->subtrack=subtrack;
        }
        if (SamePath(std::filesystem::path(settings_.player.playing_file_name),correction.source))
            settings_.player.playing_file_name=result.target.wstring();
        // Associations are keyed by the full media path; the .lrc itself stays put.
        std::vector<std::pair<lyrics::SongKey,std::filesystem::path>> associations;
        for (const auto& item:lyric_associations_.All())
            if (SamePath(item.first.path,correction.source)) associations.push_back(item);
        for (const auto& [key,lyric]:associations) {
            lyric_associations_.Erase(key);
            lyric_associations_.Set({result.target,key.subtrack},lyric);
        }
        lyric_associations_.Save();
        playlists_.FlushDirty(true);
        RefreshPlaylist();
    }
    if (affected && active && PlayCurrent())
        audio_->RestoreAfterOutputRestart(position,state==audio::PlaybackState::paused);
    RefreshPlaybackUi();
    if (result.error) {
        wchar_t* system_text{};
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,result.error,0,reinterpret_cast<LPWSTR>(&system_text),0,nullptr);
        auto text=i18n::Text(L"未能更改文件后缀。文件可能已变化或正在被占用。")+L"\r\n\r\n"+
            correction.source.wstring()+L"\r\n"+(system_text?system_text:L"")+
            L" ("+std::to_wstring(result.error)+L")";
        if (system_text) LocalFree(system_text);
        MessageBoxW(window_,text.c_str(),i18n::Literal(L"更改文件后缀"),MB_OK|MB_ICONWARNING);
    }
}
} // namespace ttplayer::ui
