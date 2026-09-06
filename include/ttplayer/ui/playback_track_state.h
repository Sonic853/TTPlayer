#pragma once

#include "ttplayer/playlist/playlist.h"

#include <optional>

namespace ttplayer::ui {

enum class FileInfoCommandTarget {
    playlist_selection,
    current_playback,
};

// FUN_00464A94 gives the PlayLists/Files surfaces ownership of the command
// only while either one has focus (or no live CPlayItem exists). Invoking
// File information from the player chrome otherwise targets the currently
// playing item, even when another catalogue/list row remains selected.
[[nodiscard]] constexpr FileInfoCommandTarget ResolveFileInfoCommandTarget(
    bool has_playback_track, bool playlist_tree_focused,
    bool playlist_files_focused) noexcept {
    return has_playback_track && !playlist_tree_focused &&
            !playlist_files_focused
        ? FileInfoCommandTarget::current_playback
        : FileInfoCommandTarget::playlist_selection;
}

// A successfully opened decoder keeps its CPlayItem alive even after the
// owning CPlayList row is removed.  UI consumers must prefer a still-valid
// row, but may fall back to that independent snapshot without manufacturing
// an index into an empty list.
[[nodiscard]] inline const playlist::Track* ResolvePlaybackTrackForUi(
    const playlist::Playlist& owner, std::optional<size_t> current,
    const std::optional<playlist::Track>& opened) noexcept {
    if (current && *current < owner.Tracks().size())
        return &owner.Tracks()[*current];
    return opened ? &*opened : nullptr;
}

} // namespace ttplayer::ui
