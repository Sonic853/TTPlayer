#pragma once

#include <cstddef>
#include <optional>

namespace ttplayer::ui {

// The private ListCtrl used by the PlayLists catalogue reports an insertion
// position, not a final row.  FUN_0041CED2 removes selected rows which precede
// that position before it performs the adjacent swaps reported as -197.
struct PlaylistCatalogMove {
    size_t source{};
    size_t insertion{};
    size_t destination{};
};

[[nodiscard]] constexpr std::optional<PlaylistCatalogMove>
PlanPlaylistCatalogMove(size_t source, size_t insertion, size_t count,
                        bool control_down = false) noexcept {
    // The original 0041D497/0041D516 path tests MK_LBUTTON only.  Ctrl does
    // not turn a catalogue reorder into a copy operation.
    static_cast<void>(control_down);
    if (source >= count || insertion > count) return std::nullopt;
    const size_t destination = source < insertion ? insertion - 1 : insertion;
    if (destination >= count) return std::nullopt;
    return PlaylistCatalogMove{source, insertion, destination};
}

// Pure index mapping used for every PlayerWindow field whose numerical row
// must continue to identify the same stable playlist object after a move.
[[nodiscard]] constexpr size_t RemapPlaylistCatalogIndex(
    size_t index, const PlaylistCatalogMove& move) noexcept {
    if (index == move.source) return move.destination;
    if (move.source < move.destination && index > move.source &&
        index <= move.destination) return index - 1;
    if (move.destination < move.source && index >= move.destination &&
        index < move.source) return index + 1;
    return index;
}

} // namespace ttplayer::ui
