#pragma once

#include "ttplayer/playlist/playlist.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ttplayer::ui {

// These services were shipped outside TTPlayer.exe.  Keep the recovered
// command eligibility and ttpres.dll failure resources independently
// testable without contacting their retired endpoints.
enum class LegacyPlaylistNetworkAction {
    freedb,
    download,
    report,
};

struct LegacyBackendNotice {
    std::uint32_t caption_resource{};
    std::uint32_t message_resource{};
};

[[nodiscard]] bool IsLegacyNetworkTrack(const playlist::Track& track);
[[nodiscard]] bool SupportsLegacyFreeDbQuery(const playlist::Track& track);
[[nodiscard]] bool LegacyNetworkActionAcceptsTrack(
    LegacyPlaylistNetworkAction action, const playlist::Track& track);
[[nodiscard]] LegacyBackendNotice LegacyBackendUnavailableNotice(
    LegacyPlaylistNetworkAction action) noexcept;
[[nodiscard]] std::wstring BuildLegacyReportTrackText(
    const playlist::Track& track, std::wstring_view heading);

} // namespace ttplayer::ui
