#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"

namespace ttplayer::ui {
using namespace detail;
namespace {
void Place(HWND window, const RECT& bounds) {
    if (IsWindow(window) && !IsRectEmpty(&bounds))
        SetWindowPos(window, nullptr, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
}
RECT RelativeTo(RECT bounds, const RECT& main) {
    OffsetRect(&bounds, main.left, main.top);
    return bounds;
}
}

bool PlayerWindow::ResetPluginWindowLayout() {
    // Layout(restore) is a pre-attach API. Never install native skin rectangles
    // over live provider subclasses, nor interpret the provider's state text.
    TtpSkinLayout previous{};
    previous.size = sizeof(previous);
    if (!external_skin_->Layout(previous, false)) return false;
    TtpSkinContent content{sizeof(content)};
    const bool have_content = external_skin_->ContentState(content);
    RECT previous_lyric{};
    GetWindowRect(lyric_window_, &previous_lyric);
    external_skin_->Detach();
    TtpSkinLayout defaults{};
    defaults.size = sizeof(defaults);
    const bool staged = external_skin_->Layout(defaults, true);
    if (staged) {
        SetWindowPos(window_, nullptr, 100, 100, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        if (skin_) Place(lyric_window_, RelativeTo(skin_->Lyric().position,
                                                  RECT{100, 100, 100, 100}));
    }
    if (!staged || !external_skin_->Attach(window_, playlist_window_,
                                          equalizer_window_, lyric_window_)) {
        external_skin_->Detach();
        external_skin_->Layout(previous, true);
        Place(window_, previous.windows[0]);
        Place(lyric_window_, previous_lyric);
        if (!external_skin_->Attach(window_, playlist_window_, equalizer_window_, lyric_window_)) {
            // A failed provider must not leave stripped HWNDs behind.
            external_skin_.reset();
            ApplyLoadedSkin(false, true);
        }
        return false;
    }
    if (have_content) external_skin_->ContentState(content, true);
    RemovePluginSkinNativeTips();
    return true;
}

void PlayerWindow::RearrangeWindows() {
    if (!IsWindow(window_)) return;
    CompleteSkinWindowFadeForReplacement();
    if (!IsWindow(window_) || close_after_skin_window_fade_) return;
    if (fullscreen_mode_ != 0) SetFullScreenMode(0);
    if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
    EndSkinMouseCapture();
    if (EqualizerOwnsCapture()) ReleaseEqualizerCapture();

    // FUN_0046C7F3. This is a layout reset, not a visibility or skin switch.
    // Mini mode intentionally leaves the normal playlist/equalizer untouched.
    if (external_skin_) {
        if (!ResetPluginWindowLayout()) return;
    } else if (skin_ && skin_->Valid()) {
        const SIZE size = mini_mode_ ? skin_->MiniWindowSize() : skin_->WindowSize();
        const int top = mini_mode_ ? 0 : 100;
        const RECT main{100, top, 100 + size.cx, top + size.cy};
        Place(window_, main);
        if (mini_mode_) {
            Place(lyric_window_, RECT{main.right, main.top, main.right + 200, main.bottom});
        } else {
            Place(lyric_window_, RelativeTo(skin_->Lyric().position, main));
            Place(equalizer_window_, RelativeTo(skin_->Equalizer().position, main));
            Place(playlist_window_, RelativeTo(skin_->Playlist().position, main));
        }
    } else {
        // The original unskinned branch centers a 2x2 group: main / lyrics,
        // playlist / equalizer. Read back the actual native window size.
        Place(window_, RECT{100, 100, 500, 400});
        RECT main{}, work{};
        GetWindowRect(window_, &main);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const LONG width = main.right - main.left;
        const LONG height = main.bottom - main.top;
        main.left = (work.left + work.right) / 2 - width;
        main.top = (work.top + work.bottom) / 2 - height;
        main.right = main.left + width;
        main.bottom = main.top + height;
        Place(window_, main);
        RECT target = main;
        OffsetRect(&target, width, 0); Place(lyric_window_, target);
        target = main;
        OffsetRect(&target, 0, height); Place(playlist_window_, target);
        OffsetRect(&target, width, 0); Place(equalizer_window_, target);
    }

    desktop_lyrics_.Rearrange();
    if (!external_skin_) {
        LayoutLyricControls();
        UpdateLyricWindowRegion();
        UpdateLyricToolRects();
        if (!mini_mode_) {
            LayoutPlaylistListControls();
            UpdatePlaylistWindowRegion();
            UpdatePlaylistToolRects();
            UpdateEqualizerWindowRegion();
            UpdateEqualizerToolRects();
        }
        UpdateMainToolRects();
    }
    UpdateVisualWindowLayout();
    // Update both the active mode cache and persisted rectangles, including
    // the hidden lyric surface while desktop lyrics are visible.
    CaptureWindowState();
    for (const HWND target : RegisteredDragWindows())
        RedrawWindow(target, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    if (!mini_mode_) SetActiveWindow(window_);
}
}
