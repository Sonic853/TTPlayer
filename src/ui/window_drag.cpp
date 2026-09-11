#include "ttplayer/ui/window_drag.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <vector>

namespace ttplayer::ui {
namespace {
bool IntervalsOverlapInclusive(LONG first_start, LONG first_end,
                               LONG second_start, LONG second_end) noexcept {
    return first_start <= second_end && second_start <= first_end;
}

int IntervalDistance(LONG first_start, LONG first_end,
                     LONG second_start, LONG second_end) noexcept {
    if (IntervalsOverlapInclusive(first_start, first_end, second_start, second_end)) return 0;
    if (first_end < second_start) return static_cast<int>(second_start - first_end);
    return static_cast<int>(first_start - second_end);
}

int Closest(const std::vector<int>& candidates, int threshold) noexcept {
    if (candidates.empty()) return 0;
    int result = candidates.front();
    for (size_t index = 1; index < candidates.size(); ++index) {
        if (std::abs(candidates[index]) < std::abs(result)) result = candidates[index];
    }
    return std::abs(result) <= threshold ? result : 0;
}
} // namespace

RECT DragWorkAreaForRect(const RECT& proposed) noexcept {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST), &info))
        return info.rcWork;
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) return work;
    // Retain a usable desktop boundary if monitor/work-area queries fail.
    work.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    work.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    work.right = work.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    work.bottom = work.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return work;
}

bool AreDragWindowsAttached(const RECT& first, const RECT& second) noexcept {
    const bool vertical_overlap = IntervalsOverlapInclusive(
        first.top, first.bottom, second.top, second.bottom);
    const bool horizontal_overlap = IntervalsOverlapInclusive(
        first.left, first.right, second.left, second.right);
    // FUN_0040BFD7 accepts every matching boundary pair, including same-side
    // alignment.  The latter is what connects LX-iPlay's overlapping 500 px
    // player/playlist windows: their left and right edges are identical even
    // though neither rectangle sits outside the other.
    const bool horizontal_boundary =
        first.left == second.left || first.left == second.right ||
        first.right == second.left || first.right == second.right;
    const bool vertical_boundary =
        first.top == second.top || first.top == second.bottom ||
        first.bottom == second.top || first.bottom == second.bottom;
    return (vertical_overlap && horizontal_boundary) ||
           (horizontal_overlap && vertical_boundary);
}

POINT ComputeDragSnapCorrection(std::span<const RECT> moving,
                                std::span<const RECT> stationary,
                                const RECT& work_area,
                                int threshold) noexcept {
    if (moving.empty() || threshold < 0) return {};
    std::vector<int> horizontal;
    std::vector<int> vertical;
    horizontal.reserve(moving.size() * (2 + stationary.size() * 4));
    vertical.reserve(moving.size() * (2 + stationary.size() * 4));

    for (const RECT& source : moving) {
        // FUN_0041094D uses only matching work-area edges.
        horizontal.push_back(static_cast<int>(work_area.left - source.left));
        horizontal.push_back(static_cast<int>(work_area.right - source.right));
        vertical.push_back(static_cast<int>(work_area.top - source.top));
        vertical.push_back(static_cast<int>(work_area.bottom - source.bottom));

        for (const RECT& target : stationary) {
            if (IntervalDistance(source.top, source.bottom,
                                 target.top, target.bottom) <= threshold) {
                horizontal.push_back(static_cast<int>(target.left - source.left));
                horizontal.push_back(static_cast<int>(target.right - source.left));
                horizontal.push_back(static_cast<int>(target.left - source.right));
                horizontal.push_back(static_cast<int>(target.right - source.right));
            }
            if (IntervalDistance(source.left, source.right,
                                 target.left, target.right) <= threshold) {
                vertical.push_back(static_cast<int>(target.top - source.top));
                vertical.push_back(static_cast<int>(target.bottom - source.top));
                vertical.push_back(static_cast<int>(target.top - source.bottom));
                vertical.push_back(static_cast<int>(target.bottom - source.bottom));
            }
        }
    }

    return {Closest(horizontal, threshold), Closest(vertical, threshold)};
}

} // namespace ttplayer::ui
