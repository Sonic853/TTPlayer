#include "ttplayer/playlist/playback_order.h"

#include <algorithm>
#include <random>

namespace ttplayer::playlist {
void RandomPlaybackOrder::Reset() noexcept {
    // 004EB8D2 clears the vector, not PlayerWnd +0x4394. Its empty length
    // forces reconstruction on the next request regardless of the old cursor.
    order_.clear();
}

bool RandomPlaybackOrder::AtEnd(size_t count) const noexcept {
    return count != 0 && order_.size() == count &&
        position_ >= static_cast<std::int64_t>(count - 1);
}

std::optional<size_t> RandomPlaybackOrder::Select(
    size_t count, std::optional<size_t> playing, Request request) {
    if (count < 2) {
        Reset();
        return count == 1 ? std::optional<size_t>{0} : std::nullopt;
    }
    if (request == Request::previous) --position_;
    else if (request == Request::next) ++position_;

    if (position_ < 0 || position_ >= static_cast<std::int64_t>(count) ||
        order_.size() != count) {
        order_.clear();
        order_.reserve(count);
        const bool anchored = playing && *playing < count;
        for (size_t row = 0; row < count; ++row)
            if (!anchored || row != *playing) order_.push_back(row);
        size_t first = 0, last = count;
        if (anchored) {
            if (request == Request::next || request == Request::initialize_preview) {
                order_.insert(order_.begin(), *playing);
                first = 1;
                position_ = 1;
            } else {
                order_.push_back(*playing);
                last = count - 1;
                position_ = static_cast<std::int64_t>(last);
            }
        } else {
            position_ = request == Request::next ? 0 : static_cast<std::int64_t>(count - 1);
        }
        // 004725D6 uses a forward Fisher-Yates shuffle on just this range:
        // the current song stays at the first/last position when rebuilding.
        static thread_local std::mt19937 generator{std::random_device{}()};
        for (size_t row = first + 1; row < last; ++row) {
            std::uniform_int_distribution<size_t> pick(first, row);
            std::swap(order_[row], order_[pick(generator)]);
        }
    }

    const auto selected = static_cast<size_t>(position_);
    if (request == Request::initialize_preview) {
        // 0045BD29 retains EAX before clearing the stored position. Return
        // the previewed successor without consuming the upcoming Next step.
        position_ = 0;
        return order_[selected];
    }
    if (request == Request::preview_next) {
        if (selected + 1 >= count) return std::nullopt;
        return order_[selected + 1];
    }
    return order_[selected];
}
} // namespace ttplayer::playlist
