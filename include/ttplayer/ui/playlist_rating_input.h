#pragma once

#include <cstddef>
#include <optional>

namespace ttplayer::ui {

// NM_CLICK is delivered by the native Files ListCtrl only after a completed
// left click.  The rebuilt owner-drawn list receives raw mouse messages, so it
// needs a small state machine to preserve that release-time contract.
struct PlaylistRatingHit {
    size_t row{};
    int rating{};

    friend bool operator==(const PlaylistRatingHit&,
                           const PlaylistRatingHit&) = default;
};

class PlaylistRatingGesture {
public:
    void Begin(PlaylistRatingHit hit) noexcept { pressed_row_ = hit.row; }

    // Leaving the rating span or crossing into another row cancels this click
    // permanently, just as a ListCtrl drag/cancel does not emit NM_CLICK for
    // the original row.
    [[nodiscard]] bool Continue(
        std::optional<PlaylistRatingHit> hit) noexcept {
        if (!pressed_row_ || !hit || hit->row != *pressed_row_) {
            pressed_row_.reset();
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<PlaylistRatingHit> Release(
        std::optional<PlaylistRatingHit> hit) noexcept {
        if (!Continue(hit)) return std::nullopt;
        pressed_row_.reset();
        return hit;
    }

    void Cancel() noexcept { pressed_row_.reset(); }
    [[nodiscard]] bool Active() const noexcept {
        return pressed_row_.has_value();
    }

private:
    std::optional<size_t> pressed_row_;
};

} // namespace ttplayer::ui
