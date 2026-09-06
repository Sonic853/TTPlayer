#pragma once

#include <algorithm>
#include <cstdint>

namespace ttplayer::ui {

// CSkinWnd::ShowWindow at 0044E2FB/0046FA5D never fades all the way to
// transparent. Five is the hand-off alpha used immediately before Hide and
// immediately after Show; the configured value is restored while hidden so a
// temporary transition cannot become the next persistent target.
struct RecoveredWindowVisibilityFade {
    std::uint8_t from{};
    std::uint8_t to{};
    std::uint8_t restore{};
    unsigned int duration_ms{};
    bool show{};
    bool animate{};
};

[[nodiscard]] constexpr RecoveredWindowVisibilityFade
BuildRecoveredWindowVisibilityFade(bool show,
                                    std::uint8_t configured_alpha,
                                    bool opaque_when_active,
                                    bool window_is_active,
                                    bool fade_windows) noexcept {
    constexpr std::uint8_t handoff_alpha = 5;
    const std::uint8_t visible_alpha = opaque_when_active
        ? static_cast<std::uint8_t>(255) : configured_alpha;
    const std::uint8_t current_alpha =
        opaque_when_active && window_is_active
            ? static_cast<std::uint8_t>(255) : configured_alpha;
    const std::uint8_t from = show ? handoff_alpha : current_alpha;
    const std::uint8_t to = show ? visible_alpha : handoff_alpha;
    const unsigned int distance = from < to ? to - from : from - to;
    return {from, to, configured_alpha, distance, show,
            fade_windows && distance >= 5};
}

// 0040C0C9/0040C0F6 expose elapsed progress as an integer percentage and
// 0044EFBE performs a truncating integer interpolation with that percentage.
[[nodiscard]] constexpr std::uint8_t InterpolateRecoveredWindowAlpha(
    std::uint8_t from, std::uint8_t to, unsigned int elapsed_ms,
    unsigned int duration_ms) noexcept {
    if (duration_ms == 0 || elapsed_ms >= duration_ms) return to;
    const int percent = static_cast<int>(
        (static_cast<std::uint64_t>(elapsed_ms) * 100U) / duration_ms);
    const int value = static_cast<int>(from) +
        (static_cast<int>(to) - static_cast<int>(from)) * percent / 100;
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

} // namespace ttplayer::ui
