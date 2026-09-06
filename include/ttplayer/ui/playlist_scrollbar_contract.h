#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ttplayer::ui {

// Parts 0/1, 2/3 and 5 in the private scrollbar custom-draw notification at
// 00488463 map to the two arrows, the two page tracks and the thumb.
enum class PlaylistScrollbarPart : unsigned char {
    none,
    line_up,
    page_up,
    thumb,
    page_down,
    line_down,
};

// The private custom-draw state at 00488463 uses 0x40 for hot and 1 for
// pressed, selecting source columns 1 and 2 respectively.  During thumb
// capture the pressed column remains active even beyond the narrow bar.
[[nodiscard]] constexpr int PlaylistScrollbarImageFrame(
    PlaylistScrollbarPart part, PlaylistScrollbarPart hovered,
    PlaylistScrollbarPart pressed) noexcept {
    if (part == PlaylistScrollbarPart::thumb && pressed == part) return 2;
    if (pressed == part && hovered == part) return 2;
    if (hovered == part) return 1;
    return 0;
}

struct PlaylistScrollbarMetrics {
    int top{};
    int bottom{};
    int button_extent{};
    int track_top{};
    int track_bottom{};
    int thumb_top{};
    int thumb_bottom{};
    int thumb_extent{};
    int thumb_travel{};
    size_t item_count{};
    size_t page_size{1};
    size_t maximum{};
    size_t position{};
};

[[nodiscard]] constexpr PlaylistScrollbarMetrics MakePlaylistScrollbarMetrics(
    int top, int bottom, int button_extent, int native_thumb_extent,
    int resize_center, size_t item_count, size_t page_size,
    size_t position) noexcept {
    PlaylistScrollbarMetrics result{};
    result.top = top;
    result.bottom = std::max(top, bottom);
    const int total_extent = result.bottom - result.top;
    result.button_extent = std::clamp(button_extent, 0, total_extent / 2);
    result.track_top = result.top + result.button_extent;
    result.track_bottom = result.bottom - result.button_extent;
    const int track_extent = result.track_bottom - result.track_top;
    result.item_count = item_count;
    result.page_size = std::max<size_t>(1, page_size);
    result.maximum = item_count > result.page_size
        ? item_count - result.page_size : 0;
    result.position = std::min(position, result.maximum);

    const int native_extent = std::clamp(
        std::max(1, native_thumb_extent), 0, track_extent);
    int target_extent = native_extent;
    if (resize_center > 0 && item_count != 0 && track_extent > 0) {
        const auto visible = std::min(item_count, result.page_size);
        const auto proportional = static_cast<int>(
            static_cast<std::uint64_t>(track_extent) * visible / item_count);
        target_extent = std::max(native_extent, proportional);
    }
    result.thumb_extent = std::clamp(target_extent, 0, track_extent);
    result.thumb_travel = std::max(0, track_extent - result.thumb_extent);
    const auto offset = result.maximum == 0 ? 0 : static_cast<int>(
        static_cast<std::uint64_t>(result.position) *
            static_cast<unsigned int>(result.thumb_travel) /
        result.maximum);
    result.thumb_top = result.track_top + offset;
    result.thumb_bottom = result.thumb_top + result.thumb_extent;
    return result;
}

[[nodiscard]] constexpr PlaylistScrollbarPart HitTestPlaylistScrollbar(
    const PlaylistScrollbarMetrics& metrics, int y) noexcept {
    if (y < metrics.top || y >= metrics.bottom || metrics.maximum == 0)
        return PlaylistScrollbarPart::none;
    if (y < metrics.track_top) return PlaylistScrollbarPart::line_up;
    if (y >= metrics.track_bottom) return PlaylistScrollbarPart::line_down;
    if (y < metrics.thumb_top) return PlaylistScrollbarPart::page_up;
    if (y < metrics.thumb_bottom) return PlaylistScrollbarPart::thumb;
    return PlaylistScrollbarPart::page_down;
}

[[nodiscard]] constexpr int PlaylistScrollbarStep(
    PlaylistScrollbarPart part, size_t page_size) noexcept {
    const size_t bounded_page = std::min<size_t>(
        std::max<size_t>(1, page_size),
        static_cast<size_t>(std::numeric_limits<int>::max()));
    switch (part) {
    case PlaylistScrollbarPart::line_up:
        return -1;
    case PlaylistScrollbarPart::page_up:
        return -static_cast<int>(bounded_page);
    case PlaylistScrollbarPart::page_down:
        return static_cast<int>(bounded_page);
    case PlaylistScrollbarPart::line_down:
        return 1;
    default:
        return 0;
    }
}

[[nodiscard]] constexpr size_t PlaylistScrollbarPositionFromThumbTop(
    const PlaylistScrollbarMetrics& metrics, int thumb_top) noexcept {
    if (metrics.maximum == 0 || metrics.thumb_travel <= 0) return 0;
    const int pixels = std::clamp(thumb_top - metrics.track_top,
                                  0, metrics.thumb_travel);
    // Round to the nearest owner-data row; this prevents a one-pixel reverse
    // motion from sticking on the preceding row during captured thumb drag.
    return static_cast<size_t>(
        (static_cast<std::uint64_t>(pixels) * metrics.maximum +
         static_cast<unsigned int>(metrics.thumb_travel / 2)) /
        static_cast<unsigned int>(metrics.thumb_travel));
}

} // namespace ttplayer::ui
