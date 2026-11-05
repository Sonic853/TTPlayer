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

std::vector<RECT> WindowWorkAreas() {
    std::vector<RECT> result;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
            MONITORINFO info{sizeof(info)};
            if (GetMonitorInfoW(monitor, &info) && !IsRectEmpty(&info.rcWork))
                reinterpret_cast<std::vector<RECT>*>(context)->push_back(info.rcWork);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
    if (result.empty()) {
        const RECT work = DragWorkAreaForRect({});
        if (!IsRectEmpty(&work)) result.push_back(work);
    }
    return result;
}

void RecoverWindowLayout(std::span<RECT> windows, std::span<const RECT> work_areas) {
    if (work_areas.empty()) return;
    const auto reachable = [&](const RECT& rect) {
        // A visible sliver of the body does not make an off-screen title bar
        // draggable. Permit deliberate partial placement and monitor spanning.
        const LONG width = std::min(32L, rect.right - rect.left);
        const LONG height = std::min(16L, rect.bottom - rect.top);
        for (const RECT& work : work_areas)
            if (rect.top >= work.top && rect.top + height <= work.bottom &&
                std::min(rect.right, work.right) - std::max(rect.left, work.left) >= width)
                return true;
        return false;
    };
    const auto nearest = [&](const RECT& rect) -> const RECT& {
        size_t best = 0;
        double distance = (std::numeric_limits<double>::max)();
        for (size_t i = 0; i < work_areas.size(); ++i) {
            const auto& work = work_areas[i];
            const double dx = std::max({0.0, static_cast<double>(work.left) - rect.right,
                static_cast<double>(rect.left) - work.right});
            const double dy = std::max({0.0, static_cast<double>(work.top) - rect.bottom,
                static_cast<double>(rect.top) - work.bottom});
            const double squared = dx * dx + dy * dy;
            if (squared < distance) { best = i; distance = squared; }
        }
        return work_areas[best];
    };
    const auto correction = [](const RECT& rect, const RECT& work) -> POINT {
        // Keep saved dimensions, including a playlist larger than the new
        // display. Its title and upper-left resize corner remain accessible.
        return {std::clamp(rect.left, work.left, std::max(work.left, work.right - (rect.right - rect.left))) - rect.left,
                std::clamp(rect.top, work.top, std::max(work.top, work.bottom - (rect.bottom - rect.top))) - rect.top};
    };
    std::vector<bool> visited(windows.size());
    for (size_t first = 0; first < windows.size(); ++first) {
        if (visited[first] || IsRectEmpty(&windows[first])) continue;
        std::vector<size_t> group{first};
        visited[first] = true;
        for (size_t cursor = 0; cursor < group.size(); ++cursor)
            for (size_t i = 0; i < windows.size(); ++i)
                if (!visited[i] && !IsRectEmpty(&windows[i]) &&
                    AreDragWindowsAttached(windows[group[cursor]], windows[i])) {
                    visited[i] = true;
                    group.push_back(i);
                }
        bool any_reachable = false;
        RECT combined = windows[first];
        for (const size_t i : group) {
            any_reachable |= reachable(windows[i]);
            UnionRect(&combined, &combined, &windows[i]);
        }
        if (!any_reachable) {
            const RECT& work = nearest(windows[first]);
            const bool fits = combined.right - combined.left <= work.right - work.left &&
                              combined.bottom - combined.top <= work.bottom - work.top;
            const POINT delta = correction(fits ? combined : windows[first], work);
            for (const size_t i : group) OffsetRect(&windows[i], delta.x, delta.y);
        }
        for (const size_t i : group) if (!reachable(windows[i])) {
            const POINT delta = correction(windows[i], nearest(windows[i]));
            OffsetRect(&windows[i], delta.x, delta.y);
        }
    }
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
