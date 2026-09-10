#pragma once

#include <algorithm>

namespace ttplayer::ui {

struct DesktopLyricViewport {
    int source_x{};
    int destination_x{};
    int played_right{};
};

// FUN_00417342: the progress coordinate belongs to the complete cached line
// bitmap, not the visible window. Keep its head still until progress crosses
// half the window, follow progress through the middle, then keep its tail
// still. This source offset is also used with karaoke highlighting disabled.
inline DesktopLyricViewport CalculateDesktopLyricViewport(
    int bitmap_width, int window_width, int played_pixels,
    int alignment, int rows, int row) noexcept {
    DesktopLyricViewport result;
    if (bitmap_width <= 0 || window_width <= 0) return result;
    played_pixels = std::clamp(played_pixels, 0, bitmap_width);
    if (bitmap_width > window_width) {
        result.source_x = std::clamp(played_pixels - window_width / 2,
                                     0, bitmap_width - window_width);
    } else {
        if (alignment == 3 && rows == 2) alignment = row == 0 ? 1 : 2;
        if (alignment == 0)
            result.destination_x = (window_width - bitmap_width) / 2;
        else if (alignment == 2)
            result.destination_x = window_width - bitmap_width;
    }
    result.played_right = result.destination_x + played_pixels - result.source_x;
    return result;
}

} // namespace ttplayer::ui
