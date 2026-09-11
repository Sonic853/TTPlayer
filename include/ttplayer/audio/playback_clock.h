#pragma once

#include <cstdint>

namespace ttplayer::audio {

// A nonpositive duration denotes an unknown endpoint, not an empty timeline.
// Bound device clocks for ordinary tracks without pinning live streams to zero.
[[nodiscard]] constexpr std::int64_t BoundPlaybackClock(
    std::int64_t position_ms, std::int64_t duration_ms) noexcept {
    if (position_ms < 0) return 0;
    return duration_ms > 0 && position_ms > duration_ms ? duration_ms : position_ms;
}

} // namespace ttplayer::audio
