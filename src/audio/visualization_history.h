#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ttplayer::audio::detail {

// Analysis-only retention for output queues shorter than an FFT window.
// Feed original contiguous output packets, never UI snapshots. Playback still
// keeps its configured queue depth; this storage adds no output latency.
class VisualizationHistory {
public:
    static constexpr size_t capacity = 4096;

    void Clear() noexcept { begin_ = end_ = 0; }

    void Append(uint64_t first, std::span<const int16_t> left,
                std::span<const int16_t> right) noexcept {
        const size_t count = std::min(left.size(), right.size());
        if (!count) return;
        if (first != end_) begin_ = end_ = first;
        const size_t skip = count > capacity ? count - capacity : 0;
        for (size_t i = skip; i < count; ++i) {
            const size_t slot = static_cast<size_t>((first + i) % capacity);
            left_[slot] = left[i]; right_[slot] = right[i];
        }
        end_ = first + count;
        if (end_ - begin_ > capacity) begin_ = end_ - capacity;
    }

    size_t Read(uint64_t cursor, std::span<int16_t> left,
                std::span<int16_t> right, uint64_t& first) const noexcept {
        const size_t limit = std::min({left.size(), right.size(), capacity});
        if (!limit || cursor < begin_ || cursor >= end_) return 0;
        const uint64_t last = cursor + std::min<uint64_t>(end_ - cursor, limit);
        first = std::max(begin_, last >= limit ? last - limit : 0);
        const size_t count = static_cast<size_t>(last - first);
        for (size_t i = 0; i < count; ++i) {
            const size_t slot = static_cast<size_t>((first + i) % capacity);
            left[i] = left_[slot]; right[i] = right_[slot];
        }
        return count;
    }

private:
    std::array<int16_t, capacity> left_{}, right_{};
    uint64_t begin_{}, end_{};
};
} // namespace ttplayer::audio::detail
