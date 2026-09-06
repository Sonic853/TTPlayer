#pragma once

#include "ttplayer/plugins/plugin_manager.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ttplayer::ui {

// One checked row in resource 228's "search type" ListView.  This catalogue
// is deliberately derived only from reader creators which were successfully
// loaded by PluginManager; the search dialog must not advertise a hard-coded
// extension whose decoder is absent.
struct PlaylistLocalSearchFormat {
    std::wstring description;
    std::wstring pattern;
};

enum class PlaylistLocalSearchRootKind {
    shell_csidl,
    drive,
    custom,
};

struct PlaylistLocalSearchRoot {
    PlaylistLocalSearchRootKind kind{PlaylistLocalSearchRootKind::shell_csidl};
    int csidl{};
    std::filesystem::path path;
};

// 004A6423 first appends the five raw CSIDL values, then the children of
// CSIDL_DRIVES, and finally resource 0x814B.  The drive bitmask is collected
// without touching any volume; type/availability checks occur in the worker.
[[nodiscard]] std::vector<PlaylistLocalSearchRoot>
BuildPlaylistLocalSearchLocations(
    std::uint32_t logical_drive_mask,
    const std::filesystem::path& custom_path = {});

// Selecting CSIDL_DRIVES expands to every advertised drive root, so the
// worker receives a true multi-root request. Other rows produce one root.
[[nodiscard]] std::vector<PlaylistLocalSearchRoot>
BuildPlaylistLocalSearchRootPlan(
    std::span<const PlaylistLocalSearchRoot> locations,
    std::size_t selected_index);

[[nodiscard]] std::vector<PlaylistLocalSearchFormat>
BuildPlaylistLocalSearchFormats(
    std::span<const plugins::ReaderFormat> reader_formats);

[[nodiscard]] bool PlaylistLocalSearchMatches(
    const std::filesystem::path& path,
    std::span<const PlaylistLocalSearchFormat> enabled_formats);

[[nodiscard]] bool PlaylistLocalSearchPassesMinimum(
    int duration_ms, bool enabled, std::uint32_t seconds) noexcept;

} // namespace ttplayer::ui
