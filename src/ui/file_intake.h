#pragma once
#include "ttplayer/playlist/playlist.h"
#include <stop_token>
#include <windows.h>

namespace ttplayer::ui::detail {

struct TrackSourceOptions {
    HMODULE module{};
    bool recursive{};
    bool ignore_bad{};
    std::vector<std::wstring> reader_patterns;
    std::stop_token stop;
};
struct TrackSourceCollection {
    std::vector<playlist::Track> tracks;
    std::vector<std::wstring> errors;
    bool canceled{}, failed{};
    // Only containers whose complete namespace (including CUE parsing) was
    // obtained successfully may invalidate older logical members.
    std::vector<std::filesystem::path> completed_containers;
};
// Uses the same CUE/archive/OSZ/directory collector as the import dialog.
// Owns no HWND or model and never publishes into the live playlist.
TrackSourceCollection CollectTrackSources(
    const std::vector<std::filesystem::path>& paths,
    const TrackSourceOptions& options, bool directory_member = true);

} // namespace ttplayer::ui::detail
