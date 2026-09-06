#pragma once

#include "ttplayer/playlist/playlist.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttplayer::ui {

// The original Music.library query owns its CPlayItems independently from the
// numbered playlist catalogue.  Keep that ownership boundary explicit when a
// rebuilt library result becomes the playback source.
[[nodiscard]] playlist::Playlist BuildMediaLibraryPlaybackSnapshot(
    std::span<const playlist::Track> tracks, size_t selected);

// Music.library addresses CPlayItems by media identity rather than by the
// ordinal currently displayed in Files.  The identity used by the original
// catalogue is represented by normalized path plus CUE/archive subtrack.
[[nodiscard]] std::wstring MediaLibraryTrackIdentity(
    const playlist::Track& track);

[[nodiscard]] size_t SetMediaLibraryPlaybackRating(
    playlist::Playlist& playback, std::wstring_view identity, int rating);

struct MediaLibraryPlaybackEraseResult {
    size_t removed{};
    size_t removed_before_current{};
    bool removed_current{};
};

[[nodiscard]] MediaLibraryPlaybackEraseResult
EraseMediaLibraryPlaybackTracks(
    playlist::Playlist& playback,
    std::span<const std::wstring> identities,
    std::optional<size_t> current);

// The materialized query keeps Track copies and stable Music.library item
// ordinals in parallel.  CPlayList sort/shuffle (004866FC/004867C8) moves
// CPlayItem pointers, so both rebuilt vectors must follow the same map.
[[nodiscard]] bool SortMediaLibraryResult(
    std::vector<playlist::Track>& tracks, std::vector<size_t>& item_indices,
    playlist::SortKey key, bool ascending,
    std::span<const std::wstring> display_titles = {});
[[nodiscard]] bool ShuffleMediaLibraryResult(
    std::vector<playlist::Track>& tracks, std::vector<size_t>& item_indices);

// Apply one successful physical-file rename to independent Music.library
// Track snapshots. Every CUE/subtrack row which names that same physical
// source follows the move while retaining its own logical subtrack identity.
[[nodiscard]] size_t ReplaceMediaLibraryTrackPath(
    std::vector<playlist::Track>& tracks, const playlist::Track& source,
    const std::filesystem::path& target);

// Plain value form of the CPlayItem information populated by
// 004ADA76/004AD95B from an opened Sound reader.  Keeping the merge policy
// independent from the private reader object makes it deterministic to test
// and ensures no DLL-owned pointer escapes the indexing worker.
struct MediaLibraryReaderInfo {
    int duration_ms{};
    std::string media_type;
    std::uint32_t bitrate_bps{};
    std::uint32_t sample_rate_hz{};
    std::vector<std::pair<std::string, std::string>> metadata;
};

void ApplyMediaLibraryReaderInfo(playlist::Track& track,
                                 MediaLibraryReaderInfo info);

} // namespace ttplayer::ui
