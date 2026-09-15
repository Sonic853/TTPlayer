#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ttplayer::playlist {

// CPlayerWnd::0045BBF6 owns one random order shared by manual navigation and
// natural completion. This is not the playlist's destructive Shuffle command.
class RandomPlaybackOrder {
public:
    enum class Request { previous, next, initialize_preview, preview_next };
    void Reset() noexcept;
    [[nodiscard]] bool AtEnd(size_t count) const noexcept;
    [[nodiscard]] std::optional<size_t> Select(
        size_t count, std::optional<size_t> playing, Request request);
private:
    std::vector<size_t> order_;
    std::int64_t position_{};
};

} // namespace ttplayer::playlist
