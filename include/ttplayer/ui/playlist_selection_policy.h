#pragma once

namespace ttplayer::ui {

enum class PlaylistSelectionTrigger {
    selection_changed,
    item_activated,
    natural_completion,
};

// Files/LVS_OWNERDATA never starts playback from LVN_ITEMCHANGED.  The
// For these selection triggers, PlayFollowCursor is consulted only at natural
// completion; explicit activation (double-click/Enter/LVN_ITEMACTIVATE) always
// plays. The separate manual Next handler also consults selected focus.
[[nodiscard]] constexpr bool ShouldStartPlaylistPlayback(
    PlaylistSelectionTrigger trigger, bool play_follow_cursor) noexcept {
    return trigger == PlaylistSelectionTrigger::item_activated ||
        (trigger == PlaylistSelectionTrigger::natural_completion &&
         play_follow_cursor);
}

} // namespace ttplayer::ui
