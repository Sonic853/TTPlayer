#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ttplayer::audio { struct PlaybackClockSnapshot; struct AudioMetadata; }
namespace ttplayer::playlist { struct Track; }
namespace ttplayer::ui {
struct TaskbarPlaybackState;

struct SystemMediaMetadata {
    std::wstring title, artist, album, album_artist;
    std::vector<std::wstring> genres;
};

// Resolve decoder tags first, then the current playlist entry; never reopen
// the media file on the window thread to obtain optional SMTC properties.
[[nodiscard]] SystemMediaMetadata BuildSystemMediaMetadata(
    const playlist::Track& track, const audio::AudioMetadata& metadata);

// UI-thread owner. WinRT types and imports stay out of the legacy build.
class SystemMediaControls {
public:
    enum class Command { play, pause, stop, previous, next, seek };
    SystemMediaControls();
    ~SystemMediaControls();
    SystemMediaControls(const SystemMediaControls&) = delete;
    SystemMediaControls& operator=(const SystemMediaControls&) = delete;

    // Copies the already decoded preview bitmap; no extra media reader/file IO.
    void SetSource(HWND window, const SystemMediaMetadata& metadata, HBITMAP cover) noexcept;
    void Update(const audio::PlaybackClockSnapshot& clock,
                const TaskbarPlaybackState& buttons, bool closing) noexcept;
    void Clear() noexcept;
    void Reset() noexcept;
    static UINT RequestMessage() noexcept;
    void DispatchPending(const std::function<void(Command, int64_t)>& dispatch);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool attempted_{};
};
} // namespace ttplayer::ui
