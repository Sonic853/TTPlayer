#pragma once

#include "file_info_probe_client.h"
#include "ttplayer/playlist/playlist.h"

namespace ttplayer::ui::detail {

enum class TrackInfoState { success, provisional, failed, unavailable };

// Playlist, catalogue and search consume the same reader result. S_FALSE means
// tags are available but a duration has not been verified (large archives).
TrackInfoState ApplyFileInfoProbeToTrack(
    playlist::Track& track, const std::optional<FileInfoProbeReadResult>& probe,
    FileInfoProbeProcessState process = FileInfoProbeProcessState::completed);

} // namespace ttplayer::ui::detail
