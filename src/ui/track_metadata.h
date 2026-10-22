#pragma once

#include "file_info_probe_client.h"
#include "ttplayer/playlist/playlist.h"
#include <algorithm>

namespace ttplayer::ui::detail {

enum class TrackInfoState { success, provisional, failed, unavailable };

// The music-service identifier belongs to the playlist row, not to the
// decoder's tag enumeration. Retain it when a reader replaces media tags.
inline void PreserveBrowserTrackIdentity(
    const std::vector<std::pair<std::string, std::string>>& original,
    std::vector<std::pair<std::string, std::string>>& updated) {
    const auto found = std::find_if(original.begin(), original.end(),
        [](const auto& field) { return field.first == "TTPlayer.Tid"; });
    if (found == original.end()) return;
    const auto identity = *found;
    std::erase_if(updated, [](const auto& field) { return field.first == "TTPlayer.Tid"; });
    updated.push_back(identity);
}

// Playlist, catalogue and search consume the same reader result. S_FALSE means
// tags are available but a duration has not been verified (large archives).
TrackInfoState ApplyFileInfoProbeToTrack(
    playlist::Track& track, const std::optional<FileInfoProbeReadResult>& probe,
    FileInfoProbeProcessState process = FileInfoProbeProcessState::completed);

} // namespace ttplayer::ui::detail
