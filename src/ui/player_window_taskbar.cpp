#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"

namespace ttplayer::ui {
using namespace detail;

TaskbarPlaybackState PlayerWindow::TaskbarState() const {
    const auto state = audio_.State();
    TaskbarPlaybackState result;
    result.playing = state == audio::PlaybackState::playing;
    if (close_after_skin_window_fade_ || audio_.StopFadePending() ||
        state == audio::PlaybackState::opening || playlists_.Empty()) return result;
    result.previous_enabled = IsSkinElementEnabled(L"prev");
    result.next_enabled = IsSkinElementEnabled(L"next");
    result.play_pause_enabled = result.playing || state == audio::PlaybackState::paused ||
        PlaybackTrackForUi() || !PlaybackPlaylist().Tracks().empty();
    return result;
}

TaskbarPlaybackLabels PlayerWindow::TaskbarLabels() const {
    const HMODULE resources = ResourceModule();
    return {ResourceCommandLabel(resources, kCmdPrevious),
            ResourceCommandLabel(resources, kCmdPlay),
            ResourceCommandLabel(resources, kCmdPause),
            ResourceCommandLabel(resources, kCmdNext)};
}

void PlayerWindow::UpdateTaskbarPlayback() {
    static_cast<void>(taskbar_playback_.Update(TaskbarState(), TaskbarLabels()));
}

void PlayerWindow::HandleTaskbarPlaybackClick(WPARAM wparam) {
    switch (TaskbarPlaybackControls::DecodeClick(wparam, TaskbarState())) {
    case TaskbarPlaybackAction::previous: SelectRelative(false); break;
    case TaskbarPlaybackAction::next: SelectRelative(true); break;
    case TaskbarPlaybackAction::play_pause:
        if (audio_.State() == audio::PlaybackState::playing) audio_.Pause();
        else if (audio_.State() == audio::PlaybackState::paused) audio_.Resume();
        else static_cast<void>(PlayCurrent());
        break;
    default: return;
    }
    RefreshPlaybackUi();
}
} // namespace ttplayer::ui
