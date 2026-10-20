#pragma once

#include <algorithm>
#include <cstdint>

namespace ttplayer::audio {

// 004AC605 refills to 80%, but the original queue contains many small reader
// packets. A bounded queue of large packets must leave room for the partly
// played packet that cannot be recycled while the device is stopped. Units
// may be frames or bytes, provided capacity, packet and requested agree.
[[nodiscard]] constexpr std::uint64_t ReachableRefillWatermark(
    std::uint64_t capacity, std::uint64_t packet,
    std::uint64_t requested) noexcept {
    if (!capacity) return 0;
    const auto reachable = capacity > packet ? capacity - packet : capacity;
    return std::max<std::uint64_t>(1, std::min(requested, reachable));
}

} // namespace ttplayer::audio
