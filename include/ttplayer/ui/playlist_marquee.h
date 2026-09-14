#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <windows.h>

namespace ttplayer::ui {

// 00482BAF creates Files as a multi-select report ListView (0x50015001,
// extended style 0x4420); 00425DEC subclasses SysListView32. Its background
// drag selects items, independently of the LVN_BEGINDRAG/OLE item-move path.
// Our skin-painted child consumes native mouse messages, so retain that
// selection gesture explicitly. The anchor is in content, not viewport,
// coordinates: scrolling must not move the start of the selection rectangle.
class PlaylistMarquee {
public:
    void Begin(POINT point, RECT viewport, size_t scroll, int row_height,
               const std::set<size_t>& selected, bool control, bool shift) {
        pending_ = true;
        active_ = false;
        origin_ = point;
        content_y_ = static_cast<int64_t>(scroll) * row_height + point.y - viewport.top;
        initial_ = selected;
        control_ = control;
        additive_ = control || shift;
    }
    void Reset() { pending_ = active_ = false; initial_.clear(); }
    [[nodiscard]] bool Pending() const { return pending_; }
    [[nodiscard]] bool Active() const { return active_; }
    [[nodiscard]] POINT Pointer() const { return pointer_; }
    [[nodiscard]] RECT Bounds() const { return bounds_; }

    std::set<size_t> Update(POINT point, RECT viewport, size_t scroll,
                           int row_height, size_t count, SIZE threshold) {
        pointer_ = point;
        if (!active_ && (std::abs(point.x - origin_.x) > threshold.cx ||
                        std::abs(point.y - origin_.y) > threshold.cy)) active_ = true;
        auto result = additive_ ? initial_ : std::set<size_t>{};
        std::erase_if(result, [count](size_t row) { return row >= count; });
        bounds_ = {};
        if (!active_ || row_height <= 0 || viewport.right <= viewport.left ||
            viewport.bottom <= viewport.top) return result;
        const LONG x = std::clamp(point.x, viewport.left, viewport.right - 1);
        const LONG y = std::clamp(point.y, viewport.top, viewport.bottom - 1);
        const int64_t offset = static_cast<int64_t>(scroll) * row_height;
        const int64_t end = offset + y - viewport.top;
        const int64_t top = std::min(content_y_, end);
        const int64_t bottom = std::max(content_y_, end) + 1;
        bounds_ = {std::max(viewport.left, std::min(origin_.x, x)),
            static_cast<LONG>(std::clamp<int64_t>(top - offset + viewport.top,
                                                viewport.top, viewport.bottom)),
            std::min(viewport.right, std::max(origin_.x, x) + 1),
            static_cast<LONG>(std::clamp<int64_t>(bottom - offset + viewport.top,
                                                viewport.top, viewport.bottom))};
        const size_t first = static_cast<size_t>(std::max<int64_t>(0, top) / row_height);
        const size_t last = std::min(count,
            static_cast<size_t>((std::max<int64_t>(0, bottom) + row_height - 1) / row_height));
        for (size_t row = first; row < last; ++row) {
            if (control_ && initial_.contains(row)) result.erase(row);
            else result.insert(row);
        }
        return result;
    }
private:
    bool pending_{}, active_{}, control_{}, additive_{};
    POINT origin_{}, pointer_{};
    RECT bounds_{};
    int64_t content_y_{};
    std::set<size_t> initial_;
};
} // namespace ttplayer::ui
