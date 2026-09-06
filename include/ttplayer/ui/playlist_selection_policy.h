#pragma once

namespace ttplayer::ui {

enum class PlaylistSelectionTrigger {
    selection_changed,
    item_activated,
    natural_completion,
};

// Files/LVS_OWNERDATA never starts playback from LVN_ITEMCHANGED.  The
// PlayFollowCursor option is consulted only by the natural-completion path;
// explicit activation (double-click/Enter/LVN_ITEMACTIVATE) always plays.
[[nodiscard]] constexpr bool ShouldStartPlaylistPlayback(
    PlaylistSelectionTrigger trigger, bool play_follow_cursor) noexcept {
    return trigger == PlaylistSelectionTrigger::item_activated ||
        (trigger == PlaylistSelectionTrigger::natural_completion &&
         play_follow_cursor);
}

} // namespace ttplayer::ui
