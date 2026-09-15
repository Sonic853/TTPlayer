#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace ttplayer::playlist {

// UI-owned cursor, with a worker that only builds immutable index arrays.
// Modern: three rolling rounds up to 5000 tracks, otherwise one shuffled cycle.
// The XP/Win7 distribution always uses one shuffled cycle.
class RandomPlaybackOrder {
public:
#if defined(TTPLAYER_LEGACY_WINDOWS)
    static constexpr size_t kThreeRoundLimit = 0;
#else
    static constexpr size_t kThreeRoundLimit = 5000;
#endif
    enum class Request { previous, next, initialize_preview, preview_next };
    struct Selection {
        std::optional<size_t> row;
        bool pending{};
    };
    RandomPlaybackOrder();
    ~RandomPlaybackOrder();
    RandomPlaybackOrder(const RandomPlaybackOrder&) = delete;
    RandomPlaybackOrder& operator=(const RandomPlaybackOrder&) = delete;
    void Reset() noexcept;
    void SetContext(std::uint64_t source, std::uint64_t revision);
    void Prepare(size_t count, std::optional<size_t> playing);
    [[nodiscard]] bool AtEnd(size_t count) const noexcept;
    [[nodiscard]] Selection TrySelect(
        size_t count, std::optional<size_t> playing, Request request);
    // Blocking convenience for non-UI consumers/tests; UI uses TrySelect.
    [[nodiscard]] std::optional<size_t> Select(
        size_t count, std::optional<size_t> playing, Request request);
private:
    struct State;
    std::unique_ptr<State> state_;
    std::uint64_t source_{};
    std::uint64_t revision_{};
};

} // namespace ttplayer::playlist
