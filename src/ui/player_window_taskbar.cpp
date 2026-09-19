#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"

#include <algorithm>

namespace ttplayer::ui {
using namespace detail;

TaskbarPlaybackState PlayerWindow::TaskbarState() const {
    const auto state = audio_->State();
    TaskbarPlaybackState result;
    result.playing = state == audio::PlaybackState::playing;
    if (close_after_skin_window_fade_ ||
        (audio_->StopFadePending() && !pending_wave_track_change_) ||
        state == audio::PlaybackState::opening || playlists_.Empty()) return result;
    result.previous_enabled = IsSkinElementEnabled(L"prev");
    result.next_enabled = IsSkinElementEnabled(L"next");
    if (pending_wave_track_change_) return result;
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
    const auto buttons = TaskbarState();
    static_cast<void>(taskbar_playback_.Update(buttons, TaskbarLabels()));
#if !defined(TTPLAYER_LEGACY_WINDOWS)
    if (!OpenedTrack()) system_media_controls_.Clear();
    else {
        auto clock = audio_->ClockSnapshot();
        if (pending_wave_track_change_) clock.state = audio::PlaybackState::opening;
        system_media_controls_.Update(clock, buttons, close_after_skin_window_fade_);
    }
#endif
    const auto state = audio_->State();
    if (!pending_wave_track_change_ && state != audio::PlaybackState::playing &&
        state != audio::PlaybackState::paused)
        taskbar_preview_.Clear();
}

void PlayerWindow::HandleTaskbarPlaybackClick(WPARAM wparam) {
    switch (TaskbarPlaybackControls::DecodeClick(wparam, TaskbarState())) {
    case TaskbarPlaybackAction::previous: SelectRelative(false); break;
    case TaskbarPlaybackAction::next: SelectRelative(true); break;
    case TaskbarPlaybackAction::play_pause:
        if (audio_->State() == audio::PlaybackState::playing) audio_->Pause();
        else if (audio_->State() == audio::PlaybackState::paused) audio_->Resume();
        else static_cast<void>(PlayCurrent());
        break;
    default: return;
    }
    RefreshPlaybackUi();
}

#if !defined(TTPLAYER_LEGACY_WINDOWS)
void PlayerWindow::HandleSystemMediaCommand(SystemMediaControls::Command command, int64_t position_ms) {
    // Recheck on the window thread: an input may have arrived during a decoder
    // change, stop fade, or a nested save-lyrics dialog.
    if (lyric_save_in_progress_ || close_after_skin_window_fade_ ||
        (audio_->StopFadePending() && !pending_wave_track_change_)) return;
    const auto state = audio_->State();
    if (state == audio::PlaybackState::opening) return;
    const auto buttons = TaskbarState();
    using Command = SystemMediaControls::Command;
    switch (command) {
    case Command::play:
        if (!buttons.play_pause_enabled) return;
        if (state == audio::PlaybackState::paused) audio_->Resume();
        else if (state != audio::PlaybackState::playing) static_cast<void>(PlayCurrent());
        break;
    case Command::pause:
        if (state == audio::PlaybackState::playing) audio_->Pause();
        break;
    case Command::stop:
        if (pending_wave_track_change_ || state == audio::PlaybackState::playing ||
            state == audio::PlaybackState::paused) Stop();
        break;
    case Command::previous:
        if (buttons.previous_enabled) SelectRelative(false);
        break;
    case Command::next:
        if (buttons.next_enabled) SelectRelative(true);
        break;
    case Command::seek:
        if ((state == audio::PlaybackState::playing || state == audio::PlaybackState::paused) &&
            audio_->Duration().count() > 0)
            audio_->Seek(std::chrono::milliseconds(std::clamp<int64_t>(position_ms, 0, audio_->Duration().count())));
        break;
    }
    RefreshPlaybackUi();
}
#endif
} // namespace ttplayer::ui
