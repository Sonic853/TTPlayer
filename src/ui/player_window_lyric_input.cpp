#include "player_window_internal.h"

#include <dwmapi.h>
#include <windowsx.h>

namespace ttplayer::ui {
namespace {
constexpr wchar_t kFullScreenLyricInputClass[] = L"TTPlayer_FullScreenLyricInput";

bool InputOnlyCompositionAvailable() {
    // NOREDIRECTIONBITMAP requires Windows 8+. RtlGetVersion avoids the
    // compatibility version of GetVersionEx when using the legacy manifest.
    static const bool supported = [] {
        using VersionFunction = LONG (WINAPI*)(OSVERSIONINFOW*);
        const auto version = reinterpret_cast<VersionFunction>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW info{sizeof(info)};
        return version && version(&info) == 0 &&
            (info.dwMajorVersion > 6 || (info.dwMajorVersion == 6 && info.dwMinorVersion >= 2));
    }();
    BOOL composed{};
    return supported && SUCCEEDED(DwmIsCompositionEnabled(&composed)) && composed;
}
} // namespace

LRESULT CALLBACK PlayerWindow::FullScreenLyricInputProc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->fullscreen_lyric_input_ = window;
    }
    if (message == WM_NCDESTROY) {
        if (self && self->fullscreen_lyric_input_ == window) self->fullscreen_lyric_input_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint); EndPaint(window, &paint);
        return 0; // Input-only: no redirection surface, tint or opacity hack.
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return self && self->fullscreen_lyric_detached_ && self->ActiveLyricDragAllowed()
            ? HTCLIENT : HTTRANSPARENT;
    default: break;
    }
    if (self && self->fullscreen_lyric_detached_ && self->lyric_control_ &&
        IsWindow(self->lyric_control_) && self->ActiveLyricDragAllowed()) {
        switch (message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            MapWindowPoints(window, self->lyric_control_, &point, 1);
            // The real lyric control takes capture/focus and commits seeks.
            // Never let the transparent area's click fall into VisualCtrl's
            // click-to-cycle handler or the desktop underneath it.
            return SendMessageW(self->lyric_control_, message, wparam,
                                MAKELPARAM(point.x, point.y));
        }
        case WM_SETCURSOR:
        case WM_CONTEXTMENU:
            return SendMessageW(self->lyric_control_, message,
                reinterpret_cast<WPARAM>(self->lyric_control_), lparam);
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            return SendMessageW(self->lyric_control_, message, wparam, lparam);
        default: break;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void PlayerWindow::DestroyFullScreenLyricInput() {
    const bool updating = std::exchange(fullscreen_lyric_input_updating_, true);
    const HWND input = std::exchange(fullscreen_lyric_input_, nullptr);
    if (input && IsWindow(input)) DestroyWindow(input);
    fullscreen_lyric_input_updating_ = updating;
}

void PlayerWindow::UpdateFullScreenLyricInput() {
    if (fullscreen_lyric_input_updating_) return;
    fullscreen_lyric_input_updating_ = true;
    struct Guard { bool& updating; ~Guard() { updating = false; } } guard{fullscreen_lyric_input_updating_};
    if (!fullscreen_lyric_detached_ || !lyric_control_ || !IsWindow(lyric_control_) ||
        !IsWindowVisible(lyric_control_) || !IsWindowEnabled(lyric_control_) ||
        (GetWindowLongPtrW(lyric_control_, GWL_STYLE) & WS_CHILD) ||
        !settings_.lyric.fullscreen_transparent || !ActiveLyricDragAllowed() ||
        !InputOnlyCompositionAvailable()) {
        DestroyFullScreenLyricInput();
        return;
    }
    RECT bounds{};
    GetClientRect(lyric_control_, &bounds);
    MapWindowPoints(lyric_control_, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
    if (IsRectEmpty(&bounds)) { DestroyFullScreenLyricInput(); return; }
    if (!fullscreen_lyric_input_) {
        const HINSTANCE instance = instance_ ? instance_ : GetModuleHandleW(nullptr);
        WNDCLASSEXW type{sizeof(type)};
        if (!GetClassInfoExW(instance, kFullScreenLyricInputClass, &type)) {
            type.lpfnWndProc = FullScreenLyricInputProc;
            type.hInstance = instance;
            type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            type.lpszClassName = kFullScreenLyricInputClass;
            if (!RegisterClassExW(&type)) return;
        }
        // A separate no-redirection popup is visually empty but has a normal
        // rectangular input region (unlike colour-key/zero-alpha windows).
        // Ownership guarantees teardown with LyricCtrl and keeps it above
        // the glyph window without adding a taskbar/Alt-Tab entry.
        if (!CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kFullScreenLyricInputClass, L"", WS_POPUP, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top,
                lyric_control_, nullptr, instance, this)) return;
    }
    HWND above = GetWindow(lyric_control_, GW_HWNDPREV);
    UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW;
    if (above == fullscreen_lyric_input_) flags |= SWP_NOZORDER;
    // Stay immediately above lyrics, not above every other application. This
    // also respects the HWND_BOTTOM desktop-background fullscreen variant.
    SetWindowPos(fullscreen_lyric_input_, above ? above : HWND_TOP,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, flags);
}

} // namespace ttplayer::ui
