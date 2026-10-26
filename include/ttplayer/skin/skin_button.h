#pragma once

#include <windows.h>
#include <commctrl.h>

namespace ttplayer::skin {
// 5.7.9 SkinButton: +34 is transient state; +3c is the check state.
// A held pointer may enter without a preceding DOWN. Leaving while held
// clears state 2, but retains capture so only this button can receive UP.
struct SkinButtonState {
    bool operator==(const SkinButtonState&) const = default;
    int transient{};
    bool checked{};
    bool enabled{true};

    void Sync(bool enable, bool check) noexcept {
        if (checked != check) { checked = check; Cancel(); }
        if (enabled != enable) { enabled = enable; transient = enable ? 0 : 3; }
    }
    void Move(bool inside, bool held) noexcept {
        if (!enabled) return;
        if (!inside) transient = 0;
        else if (held) transient = 2;
        else if (!checked) transient = 1;
    }
    void Down() noexcept { if (enabled) transient = 2; }
    void Cancel() noexcept { transient = checked ? 2 : 0; }
    bool Release(bool inside) noexcept {
        const bool invoke = enabled && inside && transient == 2;
        Cancel();
        return invoke;
    }
    int Frame() const noexcept { return checked && transient != 1 ? 2 : transient; }
};

inline constexpr wchar_t kPngButtonHitRegion[] = L"TTPlayer.PngButtonHitRegion";
inline constexpr UINT_PTR kPngButtonRegionSubclass = 0x54545041;

inline void ClearPngButtonHitRegion(HWND window) {
    if (const auto region = reinterpret_cast<HRGN>(RemovePropW(window, kPngButtonHitRegion)))
        DeleteObject(region);
}

inline LRESULT CALLBACK PngButtonRegionSubclass(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam, UINT_PTR identifier, DWORD_PTR) {
    if (message == WM_NCHITTEST) {
        const auto region = reinterpret_cast<HRGN>(GetPropW(window, kPngButtonHitRegion));
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        ScreenToClient(window, &point);
        if (region && !PtInRegion(region, point.x, point.y)) return HTTRANSPARENT;
    } else if (message == WM_NCDESTROY) {
        ClearPngButtonHitRegion(window);
        RemoveWindowSubclass(window, PngButtonRegionSubclass, identifier);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

// Takes ownership on success. PNG HWNDs paint their complete client rectangle,
// while the normal alpha mask still controls hit testing and captured releases.
inline bool SetPngButtonHitRegion(HWND window, HRGN region) {
    ClearPngButtonHitRegion(window);
    RemoveWindowSubclass(window, PngButtonRegionSubclass, kPngButtonRegionSubclass);
    if (!region) return true;
    if (!SetWindowSubclass(window, PngButtonRegionSubclass, kPngButtonRegionSubclass, 0)) return false;
    if (!SetPropW(window, kPngButtonHitRegion, region)) {
        RemoveWindowSubclass(window, PngButtonRegionSubclass, kPngButtonRegionSubclass);
        return false;
    }
    return true;
}

inline bool SkinButtonContains(HWND window, POINT point) {
    if (const auto hit = reinterpret_cast<HRGN>(GetPropW(window, kPngButtonHitRegion))) {
        RECT client{};
        GetClientRect(window, &client);
        return PtInRect(&client, point) && PtInRegion(hit, point.x, point.y);
    }
    const HRGN region = CreateRectRgn(0, 0, 0, 0);
    const int kind = region ? GetWindowRgn(window, region) : ERROR;
    RECT bounds{};
    GetClientRect(window, &bounds);
    const bool inside = kind == ERROR ? PtInRect(&bounds, point) != FALSE
                                     : PtInRegion(region, point.x, point.y) != FALSE;
    if (region) DeleteObject(region);
    return inside;
}

// HWND capture operations are deliberately ordered before state changes:
// Set/ReleaseCapture synchronously dispatch WM_CAPTURECHANGED to the old HWND.
inline bool HandleSkinButtonInput(HWND window, SkinButtonState& state,
    UINT message, WPARAM flags, LPARAM position, bool& invoke) {
    invoke = false;
    const POINT point{static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
    switch (message) {
    case WM_MOUSEMOVE: {
        if (!state.enabled) return true;
        const bool inside = SkinButtonContains(window, point);
        if (inside && GetCapture() != window) SetCapture(window);
        else if (!inside && !(flags & MK_LBUTTON) && GetCapture() == window) ReleaseCapture();
        state.Move(inside, (flags & MK_LBUTTON) != 0);
        return true;
    }
    case WM_LBUTTONDOWN:
        state.Down();
        if (GetWindowLongPtrW(window, GWL_STYLE) & WS_TABSTOP) SetFocus(window);
        return true;
    case WM_LBUTTONUP:
        invoke = state.Release(SkinButtonContains(window, point));
        if (GetCapture() == window) ReleaseCapture();
        return true;
    case WM_CAPTURECHANGED:
        state.Cancel();
        return true;
    case WM_CANCELMODE:
        // Native DefWindowProc only releases owned capture. Without it there
        // is no WM_CAPTURECHANGED and the current (e.g. disabled) frame stays.
        if (GetCapture() == window) ReleaseCapture();
        return true;
    case WM_ENABLE:
        state.enabled = flags != 0;
        state.transient = flags ? 0 : 3;
        return true;
    }
    return false;
}
} // namespace ttplayer::skin
