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

// waveOut reports 32-bit counters even for high-rate, multichannel PCM.
// Less than 4 GiB can be queued, so the submitted count identifies the wrap
// unambiguously. Both values are in the same unit and restart at each reset.
[[nodiscard]] constexpr std::uint64_t ExpandWaveOutPosition(
    std::uint32_t reported, std::uint64_t submitted) noexcept {
    constexpr std::uint64_t period = UINT64_C(1) << 32;
    return reported > submitted ? submitted : reported + (submitted - reported) / period * period;
}

} // namespace ttplayer::audio
