#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace ttplayer::audio { struct PlaybackClockSnapshot; }
namespace ttplayer::ui {
struct TaskbarPlaybackState;

// UI-thread owner. WinRT types and imports stay out of the legacy build.
class SystemMediaControls {
public:
    enum class Command { play, pause, stop, previous, next, seek };
    SystemMediaControls();
    ~SystemMediaControls();
    SystemMediaControls(const SystemMediaControls&) = delete;
    SystemMediaControls& operator=(const SystemMediaControls&) = delete;

    // Copies the already decoded preview bitmap; no extra media reader/file IO.
    void SetSource(HWND window, std::wstring_view title, std::wstring_view artist,
                   std::wstring_view album, HBITMAP cover) noexcept;
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
