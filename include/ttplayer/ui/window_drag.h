#pragma once

#include <span>
#include <windows.h>

namespace ttplayer::ui {

// Pure geometry recovered from FUN_0040BFD7, FUN_0041094D and
// FUN_0044F804. Keeping it independent from HWND mutation makes the exact
// ten-pixel snap boundary regression-testable.
[[nodiscard]] bool AreDragWindowsAttached(const RECT& first, const RECT& second) noexcept;

[[nodiscard]] POINT ComputeDragSnapCorrection(
    std::span<const RECT> moving,
    std::span<const RECT> stationary,
    const RECT& work_area,
    int threshold) noexcept;

} // namespace ttplayer::ui
