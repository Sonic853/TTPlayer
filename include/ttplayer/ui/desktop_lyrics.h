#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <windows.h>

namespace ttplayer::lyrics { struct Lyrics; }
namespace ttplayer::settings {
struct DesktopLyricSettings;
}
namespace ttplayer::skin { class LegacySkin; }

namespace ttplayer::ui {

// Independent reconstruction of CDeskLrcCtrl/CDeskLrcPaint/CDeskLrcBar
// (00416032..0041A537).  The three popup HWNDs deliberately outlive display
// mode changes, just as the original lyric manager keeps both normal and
// desktop surfaces alive and switches their visibility.
class DesktopLyricsWindow final {
public:
    // PlayerWindow can wrap TrackPopupMenuEx with its recovered owner-draw
    // menu renderer.  Tests and non-skinned hosts may leave this empty.
    using PopupMenuTracker =
        std::function<UINT(HMENU menu, POINT screen_point, HWND owner)>;

    DesktopLyricsWindow();
    ~DesktopLyricsWindow();
    DesktopLyricsWindow(const DesktopLyricsWindow&) = delete;
    DesktopLyricsWindow& operator=(const DesktopLyricsWindow&) = delete;

    bool Create(HINSTANCE instance, HWND lyric_owner, HWND command_target,
                HMODULE resource_module,
                settings::DesktopLyricSettings* settings,
                RECT* persisted_bounds,
                PopupMenuTracker popup_tracker = {});
    void Destroy() noexcept;

    void SetSkin(const skin::LegacySkin* skin);
    void SetLyrics(const lyrics::Lyrics* lyrics);
    void SetFallbackText(std::wstring text);
    void UpdatePlayback(std::chrono::milliseconds position, bool playing);
    void ApplySettings();
    // Reconcile owner/owned Z order without rebuilding fonts or repainting.
    void RefreshTopmost();
    void CaptureBounds() noexcept;

    void Show(bool visible);
    void Toggle();
    [[nodiscard]] bool Visible() const noexcept;

    [[nodiscard]] HWND ControlHandle() const noexcept;
    [[nodiscard]] HWND PaintHandle() const noexcept;
    [[nodiscard]] HWND BarHandle() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ttplayer::ui
