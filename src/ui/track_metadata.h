#pragma once

#include "file_info_probe_client.h"
#include "ttplayer/playlist/playlist.h"
#include <algorithm>

namespace ttplayer::ui::detail {

enum class TrackInfoState { success, provisional, failed, unavailable };

inline bool IsPersonalLibraryTrack(const playlist::Track& track) noexcept {
    return track.path.native().starts_with(L"https://ttpbrowser.invalid/");
}

inline bool IsMusicBrowserTrack(const playlist::Track& track) noexcept {
    if (IsPersonalLibraryTrack(track)) return true;
    const auto& source = track.path.native();
    if (!source.starts_with(L"https://") && !source.starts_with(L"http://")) return false;
    // Provider pages such as Sayobot submit real audio URLs with a row ID.
    // Ordinary pasted URLs and local files retain their existing formatting.
    return std::any_of(track.metadata.begin(), track.metadata.end(), [](const auto& field) {
        return field.first == "TTPlayer.Tid" && !field.second.empty();
    });
}

inline bool IsOsuLibraryMetadata(
    const std::vector<std::pair<std::string, std::string>>& metadata) noexcept {
    return std::any_of(metadata.begin(), metadata.end(), [](const auto& field) {
        return field.first == "TTPlayer.Osu.Source" &&
            (field.second == "1" || field.second == "2");
    });
}

inline bool IsOsuLibraryTrack(const playlist::Track& track) noexcept {
    return IsOsuLibraryMetadata(track.metadata);
}

// These fields describe the selected beatmap, not tags in its shared audio
// file. An audio-reader refresh must retain the original row's identity,
// title and background, even when another beatmap uses the same audio path.
inline void PreserveOsuTrackMetadata(
    const std::vector<std::pair<std::string, std::string>>& original,
    std::vector<std::pair<std::string, std::string>>& updated) {
    if (!IsOsuLibraryMetadata(original) || &original == &updated) return;
    std::erase_if(updated, [](const auto& field) {
        return field.first.starts_with("TTPlayer.Osu.");
    });
    for (const auto& field : original)
        if (field.first.starts_with("TTPlayer.Osu.")) updated.push_back(field);
}

inline void RestoreOsuTrackMetadata(playlist::Track& track) {
    if (!IsOsuLibraryTrack(track)) return;
    const auto restore = [&](const char* source, const char* tag,
                             std::string& destination) {
        const auto found = std::find_if(track.metadata.begin(), track.metadata.end(),
            [source](const auto& field) { return field.first == source; });
        if (found == track.metadata.end()) return;
        const auto value = found->second;
        destination = value;
        std::erase_if(track.metadata, [tag](const auto& field) {
            return _stricmp(field.first.c_str(), tag) == 0;
        });
        if (!value.empty()) track.metadata.emplace_back(tag, value);
    };
    restore("TTPlayer.Osu.Title", "Title", track.title);
    restore("TTPlayer.Osu.Artist", "Artist", track.artist);
    restore("TTPlayer.Osu.Album", "Album", track.album);
}

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
