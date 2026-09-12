#include "ttplayer/ui/desktop_lyrics.h"
#include "ttplayer/ui/desktop_lyric_layout.h"
#include "ttplayer/ui/window_drag.h"

#include "ttplayer/core/text.h"
#include "ttplayer/lyrics/lrc_parser.h"
#include "ttplayer/settings/settings.h"
#include "ttplayer/skin/skin.h"
#include "../app/resource_ids.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <commctrl.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>
#include <windowsx.h>

namespace ttplayer::ui {
namespace {
constexpr wchar_t kControlClass[] = L"DeskLrcCtrlClass";
constexpr wchar_t kPaintClass[] = L"DeskLrcPaintClass";
constexpr wchar_t kBarClass[] = L"DeskLrcBarClass";
constexpr wchar_t kSkinButtonClass[] = L"SkinButton";
constexpr wchar_t kSkinIconClass[] = L"SkinIcon";

constexpr UINT_PTR kRenderTimer = 0x7b;
constexpr UINT_PTR kHoverTimer = 0x7c;
constexpr UINT kMenuProfiles = 0x184;
constexpr UINT kMenuDesktopLyric = 0x185;
constexpr UINT kMenuPlaylist = 0x97;

constexpr UINT kCommandPlay = 32000;
constexpr UINT kCommandPause = 32001;
constexpr UINT kCommandPrevious = 32005;
constexpr UINT kCommandNext = 32006;
constexpr UINT kCommandIcon = 0x7dd8;
constexpr UINT kCommandList = 0x803e;
constexpr UINT kCommandSettings = 0x803f;
constexpr UINT kCommandKaraoke = 0x866;
constexpr UINT kCommandLines = 0x8d1;
constexpr UINT kCommandLock = 0x8040;
constexpr UINT kCommandUnlock = 0x8041;
constexpr UINT kCommandTopmost = 0x8042;
constexpr UINT kCommandBackgroundPassthrough = 0x8043;
constexpr UINT kCommandReturn = 0x8038;
constexpr UINT kCommandClose = 8;
constexpr UINT kCommandShowLyrics = 0x7d64;
constexpr UINT kCommandDesktopOptions = 0x8046;
constexpr UINT kMessageShowOptionsControl = 0x7f4;
constexpr UINT kCommandProfileFirst = 0x80a2;
constexpr UINT kCommandProfileLast = 0x80a4;
constexpr UINT kCommandOneLine = 0x80b0;
constexpr UINT kCommandTwoLines = 0x80b1;
constexpr UINT kCommandZoomIn = IDC_DESKTOP_LYRIC_ZOOM_IN;
constexpr UINT kCommandZoomOut = IDC_DESKTOP_LYRIC_ZOOM_OUT;

constexpr unsigned int kDragInterior = 1;
constexpr unsigned int kDragRight = 0x10;
constexpr unsigned int kDragBottom = 0x20;
constexpr unsigned int kDragLeft = 0x40;
constexpr unsigned int kDragTop = 0x80;

constexpr int kMinimumWidth = 400;
constexpr int kMaximumWidth = 10000;
constexpr int kMinimumLineHeight = 24;
constexpr int kMaximumLineHeight = 150;

struct ScopedDc final {
    HWND window{};
    HDC dc{};
    explicit ScopedDc(HWND source) : window(source), dc(GetDC(source)) {}
    ~ScopedDc() { if (dc) ReleaseDC(window, dc); }
    ScopedDc(const ScopedDc&) = delete;
    ScopedDc& operator=(const ScopedDc&) = delete;
};

std::wstring ResourceText(HMODULE module, UINT identifier) {
    if (!module) return {};
    const wchar_t* value{};
    const int length = LoadStringW(module, identifier,
        reinterpret_cast<LPWSTR>(&value), 0);
    return length > 0 && value
        ? std::wstring(value, static_cast<size_t>(length)) : std::wstring{};
}

std::wstring ResourceCommandText(HMODULE module, UINT identifier) {
    auto result = ResourceText(module, identifier);
    const size_t separator = result.find_last_of(L"\r\n");
    if (separator != std::wstring::npos) result.erase(0, separator + 1);
    const size_t tab = result.find(L'\t');
    if (tab != std::wstring::npos) result.erase(tab);
    return result;
}

std::wstring WideLyric(std::string_view value) {
    try {
        return core::Utf8ToWide(value);
    } catch (const std::exception&) {
        if (value.empty()) return {};
        const int count = MultiByteToWideChar(CP_ACP, 0, value.data(),
            static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0) return {};
        std::wstring result(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_ACP, 0, value.data(),
            static_cast<int>(value.size()), result.data(), count);
        return result;
    }
}

bool ValidRect(const RECT& value) noexcept {
    return value.right > value.left && value.bottom > value.top;
}

int Width(const RECT& value) noexcept { return value.right - value.left; }
int Height(const RECT& value) noexcept { return value.bottom - value.top; }

bool IntervalsMeet(int first_begin, int first_end, int second_begin,
                   int second_end, int margin) noexcept {
    return first_begin <= second_end + margin &&
           first_end >= second_begin - margin;
}

int NearestSnapOffset(int first, int second, int distance) noexcept {
    const int nearest = std::abs(first) <= std::abs(second) ? first : second;
    return std::abs(nearest) <= distance ? nearest : 0;
}

// FUN_004196D5 offsets first and then magnetically joins a moving rectangle
// to work-area edges. Resolve the proposed rectangle's monitor so crossing
// a screen boundary does not keep snapping/clamping against the primary.
// The projection checks keep an unrelated axis
// from snapping when the rectangle is wholly beyond that side of the work
// area; the actual magnetic range is ten pixels.
void MoveAndSnapToWorkArea(RECT& target, int delta_x, int delta_y) {
    OffsetRect(&target, delta_x, delta_y);
    constexpr int kSnapDistance = 10;
    const RECT work = DragWorkAreaForRect(target);
    int snap_x{};
    int snap_y{};
    if (IntervalsMeet(target.top, target.bottom, work.top, work.bottom,
                      kSnapDistance)) {
        snap_x = NearestSnapOffset(work.left - target.left,
                                   work.right - target.right,
                                   kSnapDistance);
    }
    if (IntervalsMeet(target.left, target.right, work.left, work.right,
                      kSnapDistance)) {
        snap_y = NearestSnapOffset(work.top - target.top,
                                   work.bottom - target.bottom,
                                   kSnapDistance);
    }
    if (snap_x != 0 || snap_y != 0) OffsetRect(&target, snap_x, snap_y);
}

RECT DefaultBarRect(UINT command) {
    switch (command) {
    case kCommandPlay: return RECT{48, 6, 65, 19};
    case kCommandPrevious: return RECT{29, 6, 46, 19};
    case kCommandNext: return RECT{67, 6, 84, 19};
    case kCommandList: return RECT{86, 6, 103, 19};
    case kCommandSettings: return RECT{108, 6, 123, 19};
    case kCommandKaraoke: return RECT{126, 6, 141, 19};
    case kCommandLines: return RECT{144, 6, 159, 19};
    case kCommandLock: return RECT{162, 6, 176, 19};
    case kCommandTopmost: return RECT{179, 6, 191, 19};
    case kCommandReturn: return RECT{194, 6, 208, 19};
    case kCommandClose: return RECT{210, 6, 224, 19};
    default: return RECT{};
    }
}

LRESULT CALLBACK PassiveChildProc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool RegisterClassIfMissing(HINSTANCE instance, const wchar_t* name,
                            WNDPROC procedure, UINT style = CS_DBLCLKS) {
    WNDCLASSEXW existing{sizeof(existing)};
    if (GetClassInfoExW(instance, name, &existing) ||
        GetClassInfoExW(nullptr, name, &existing)) return true;
    WNDCLASSEXW type{sizeof(type)};
    type.style = style;
    type.lpfnWndProc = procedure;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = nullptr;
    type.lpszClassName = name;
    return RegisterClassExW(&type) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

COLORREF InterpolateColor(COLORREF first, COLORREF second, int amount,
                          int total) {
    if (total <= 0) return second;
    amount = std::clamp(amount, 0, total);
    const auto channel = [amount, total](BYTE a, BYTE b) {
        return static_cast<BYTE>((static_cast<int>(a) * (total - amount) +
                                  static_cast<int>(b) * amount) / total);
    };
    return RGB(channel(GetRValue(first), GetRValue(second)),
               channel(GetGValue(first), GetGValue(second)),
               channel(GetBValue(first), GetBValue(second)));
}

COLORREF GradientColor(const std::array<COLORREF, 3>& colors, int count,
                       int position, int extent) {
    count = std::clamp(count, 1, 3);
    if (count == 1 || extent <= 1) return colors[0];
    position = std::clamp(position, 0, extent - 1);
    const int scaled = position * (count - 1);
    const int segment = std::min(count - 2, scaled / std::max(1, extent - 1));
    const int segment_start = segment * std::max(1, extent - 1);
    return InterpolateColor(colors[static_cast<size_t>(segment)],
        colors[static_cast<size_t>(segment + 1)],
        scaled - segment_start, std::max(1, extent - 1));
}

void CompositePixel(unsigned char* target, COLORREF color, unsigned int alpha) {
    alpha = std::min(alpha, 255U);
    if (alpha == 0) return;
    const unsigned int inverse = 255U - alpha;
    const unsigned int old_alpha = target[3];
    target[0] = static_cast<unsigned char>(
        (GetBValue(color) * alpha + target[0] * inverse) / 255U);
    target[1] = static_cast<unsigned char>(
        (GetGValue(color) * alpha + target[1] * inverse) / 255U);
    target[2] = static_cast<unsigned char>(
        (GetRValue(color) * alpha + target[2] * inverse) / 255U);
    target[3] = static_cast<unsigned char>(
        alpha + old_alpha * inverse / 255U);
}

} // namespace

class DesktopLyricsWindow::Impl final {
public:
    struct Button final {
        UINT command{};
        HWND window{};
    };

    struct GlyphMask final {
        std::wstring text;
        int width{};
        int height{};
        int row{};
        std::vector<unsigned char> alpha;
    };

    ~Impl() { Destroy(); }

    bool Create(HINSTANCE instance, HWND lyric_owner, HWND command_target,
                HMODULE resource_module,
                settings::DesktopLyricSettings* desktop_settings,
                RECT* persisted_bounds, PopupMenuTracker tracker) {
        Destroy();
        if (!instance || !lyric_owner || !desktop_settings ||
            !persisted_bounds) return false;
        instance_ = instance;
        owner_ = lyric_owner;
        command_target_ = command_target;
        resource_module_ = resource_module;
        settings_ = desktop_settings;
        persisted_bounds_ = persisted_bounds;
        popup_tracker_ = std::move(tracker);

        if (!RegisterClassIfMissing(instance_, kControlClass, ControlProc) ||
            !RegisterClassIfMissing(instance_, kPaintClass, PaintProc, 0) ||
            !RegisterClassIfMissing(instance_, kBarClass, BarProc, CS_DBLCLKS) ||
            !RegisterClassIfMissing(instance_, kSkinButtonClass,
                                    PassiveChildProc) ||
            !RegisterClassIfMissing(instance_, kSkinIconClass,
                                    PassiveChildProc)) return false;

        RECT bounds = *persisted_bounds_;
        if (!ValidRect(bounds)) bounds = RECT{100, 100, 720, 166};
        const int width = std::clamp(Width(bounds), kMinimumWidth,
                                     kMaximumWidth);
        bounds.right = bounds.left + width;
        if (Height(bounds) <= 0) bounds.bottom = bounds.top + 66;

        const DWORD topmost = settings_->topmost ? WS_EX_TOPMOST : 0;
        control_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | topmost,
            kControlClass, L"DeskLrcCtrl", WS_POPUP | WS_CLIPSIBLINGS,
            bounds.left, bounds.top, Width(bounds), Height(bounds), owner_,
            nullptr, instance_, this);
        if (!control_) {
            Destroy();
            return false;
        }
        paint_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | topmost,
            kPaintClass, L"PaintWnd", WS_POPUP | WS_CLIPSIBLINGS,
            bounds.left, bounds.top, Width(bounds), Height(bounds), control_,
            nullptr, instance_, this);
        bar_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | topmost,
            kBarClass, L"BarWnd", WS_POPUP | WS_CLIPSIBLINGS,
            -1000, -1000, 231, 25, control_, nullptr, instance_, this);
        if (!paint_ || !bar_) {
            Destroy();
            return false;
        }

        CreateBarChildren();
        SetTimer(control_, kRenderTimer, 30, nullptr);
        SetTimer(control_, kHoverTimer, 100, nullptr);
        ApplySettings();
        PositionWindows(bounds, true);
        return true;
    }

    void Destroy() noexcept {
        visible_ = false;
        const HWND capture = GetCapture();
        if (capture &&
            (capture == control_ || capture == bar_ ||
             (bar_ && IsWindow(bar_) && IsChild(bar_, capture)))) {
            ReleaseCapture();
        }
        dragging_ = false;
        drag_hit_ = 0;
        manual_resizing_ = false;
        bar_drag_anchor_valid_ = false;
        render_request_pending_ = false;
        hover_primed_ = false;
        menu_tracking_ = false;
        control_alpha_valid_ = false;
        hover_button_ = 0;
        pressed_button_ = 0;
        if (control_ && IsWindow(control_)) {
            KillTimer(control_, kRenderTimer);
            KillTimer(control_, kHoverTimer);
        }
        if (bar_ && IsWindow(bar_)) DestroyWindow(bar_);
        bar_ = nullptr;
        tooltip_ = nullptr;
        for (auto& button : buttons_) button.window = nullptr;
        icon_ = nullptr;
        if (paint_ && IsWindow(paint_)) DestroyWindow(paint_);
        paint_ = nullptr;
        if (control_ && IsWindow(control_)) DestroyWindow(control_);
        control_ = nullptr;
        if (font_) DeleteObject(font_);
        font_ = nullptr;
        owner_ = nullptr;
        command_target_ = nullptr;
        resource_module_ = nullptr;
        settings_ = nullptr;
        persisted_bounds_ = nullptr;
        skin_ = nullptr;
        lyrics_ = nullptr;
        popup_tracker_ = {};
        masks_.clear();
    }

    void SetSkin(const skin::LegacySkin* value) {
        skin_ = value;
        ApplyBarSkin();
    }

    void SetLyrics(const lyrics::Lyrics* value) {
        lyrics_ = value;
        mask_cache_dirty_ = true;
        rendered_line_ = std::numeric_limits<size_t>::max();
        RenderLayered();
    }

    void SetFallbackText(std::wstring value) {
        if (fallback_text_ == value) return;
        fallback_text_ = std::move(value);
        mask_cache_dirty_ = true;
        RenderLayered();
    }

    void UpdatePlayback(std::chrono::milliseconds position, bool playing) {
        anchor_position_ = position;
        anchor_time_ = std::chrono::steady_clock::now();
        if (playing_ != playing) {
            playing_ = playing;
            InvalidateBar();
        }
        RenderLayered();
    }

    void ApplySettings() {
        if (!settings_ || !control_ || !IsWindow(control_)) return;
        settings_->lines = std::clamp(settings_->lines, 1, 2);
        settings_->align = std::clamp(settings_->align, 0, 3);
        settings_->text_alpha = std::clamp(settings_->text_alpha, 0, 255);
        settings_->background_alpha =
            std::clamp(settings_->background_alpha, 0, 255);
        RebuildFont();

        RECT bounds{};
        GetWindowRect(control_, &bounds);
        const bool font_changed = std::memcmp(&last_font_, &settings_->font,
                                              sizeof(LOGFONTW)) != 0;
        if (!manual_resizing_ &&
            (applied_lines_ != settings_->lines || font_changed)) {
            const int font_height = MeasureFontHeight();
            const int desired = std::max(kMinimumLineHeight * settings_->lines,
                font_height * 3 / 2 * settings_->lines);
            bounds.bottom = bounds.top + std::min(
                kMaximumLineHeight * settings_->lines, desired);
            applied_lines_ = settings_->lines;
            last_font_ = settings_->font;
        }
        PositionWindows(bounds, true);

        hover_primed_ = false;
        SetControlAlpha(RestingControlAlpha());
        ApplyClickThrough();
        ApplyTopmost();
        if (settings_->lock) SetBarVisible(false);
        mask_cache_dirty_ = true;
        InvalidateRect(control_, nullptr, TRUE);
        InvalidateBar();
        RenderLayered();
    }

    void CaptureBounds() noexcept {
        if (control_ && IsWindow(control_) && persisted_bounds_)
            GetWindowRect(control_, persisted_bounds_);
    }

    void Show(bool show) {
        if (!control_ || !paint_ || !IsWindow(control_) || !IsWindow(paint_))
            return;
        visible_ = show;
        if (!show) {
            SetBarVisible(false);
            hover_primed_ = false;
            SetControlAlpha(0);
            ShowWindow(paint_, SW_HIDE);
            ShowWindow(control_, SW_HIDE);
            return;
        }
        RECT bounds{};
        GetWindowRect(control_, &bounds);
        PositionWindows(bounds, true);
        ShowWindow(control_, SW_SHOWNOACTIVATE);
        ShowWindow(paint_, SW_SHOWNOACTIVATE);
        // Hiding the non-topmost lyric owner temporarily hides its owned
        // desktop surfaces and can demote the control in the Win32 z-order.
        // Reapply 0041964D after the original hide-then-show transition.
        ApplyTopmost();
        SetControlAlpha(RestingControlAlpha());
        RenderLayered();
    }

    void Toggle() { Show(!Visible()); }
    [[nodiscard]] bool Visible() const noexcept {
        return control_ && IsWindow(control_) && visible_ &&
               IsWindowVisible(control_) != FALSE;
    }

    [[nodiscard]] HWND ControlHandle() const noexcept {
        return control_ && IsWindow(control_) ? control_ : nullptr;
    }
    [[nodiscard]] HWND PaintHandle() const noexcept {
        return paint_ && IsWindow(paint_) ? paint_ : nullptr;
    }
    [[nodiscard]] HWND BarHandle() const noexcept {
        return bar_ && IsWindow(bar_) ? bar_ : nullptr;
    }

private:
    void InvalidateBar() const noexcept {
        if (bar_ && IsWindow(bar_))
            InvalidateRect(bar_, nullptr, FALSE);
    }

    void SetButtonInteraction(UINT hover, UINT pressed) noexcept {
        // 0040A5E3 returns without invalidating when the visual state is
        // unchanged. Mouse movement within one button is not a new frame.
        if (hover_button_ == hover && pressed_button_ == pressed) return;
        const std::array<UINT,4> changed{hover_button_, hover, pressed_button_, pressed};
        hover_button_ = hover;
        pressed_button_ = pressed;
        // Native SkinButton invalidates its own HWND. The rebuilt toolbar
        // composites PNG children centrally, so restrict the parent's update
        // region to the affected children instead of erasing the entire bar.
        for (const UINT command : changed) {
            if (!command || !bar_ || !IsWindow(bar_)) continue;
            const HWND child = GetDlgItem(bar_, static_cast<int>(command));
            RECT bounds{};
            if (child && GetWindowRect(child, &bounds)) {
                MapWindowPoints(nullptr, bar_, reinterpret_cast<POINT*>(&bounds), 2);
                InvalidateRect(bar_, &bounds, FALSE);
            }
        }
    }

    [[nodiscard]] BYTE ConfiguredControlAlpha() const noexcept {
        return settings_
            ? static_cast<BYTE>(std::max(1, settings_->background_alpha))
            : static_cast<BYTE>(1);
    }

    [[nodiscard]] BYTE RestingControlAlpha() const noexcept {
        return settings_ && settings_->background_show
            ? ConfiguredControlAlpha() : static_cast<BYTE>(0);
    }

    [[nodiscard]] static BYTE HoverProbeAlpha() noexcept {
        const HDC screen = GetDC(nullptr);
        const int bits = screen ? GetDeviceCaps(screen, BITSPIXEL) : 32;
        if (screen) ReleaseDC(nullptr, screen);
        // FUN_0041A341 uses 1 on normal displays and 5 below 17 bpp.
        return static_cast<BYTE>(bits < 17 ? 5 : 1);
    }

    void SetControlAlpha(BYTE alpha) noexcept {
        if (!control_ || !IsWindow(control_)) return;
        if (control_alpha_valid_ && control_alpha_ == alpha) return;
        if (SetLayeredWindowAttributes(control_, 0, alpha, LWA_ALPHA)) {
            control_alpha_ = alpha;
            control_alpha_valid_ = true;
        }
    }

    void SetBarVisible(bool show) noexcept {
        if (!bar_ || !IsWindow(bar_)) return;
        if (!show) {
            hover_button_ = 0;
            pressed_button_ = 0;
        }
        if ((IsWindowVisible(bar_) != FALSE) == show) return;
        SetWindowPos(bar_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
            (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        if (!show) InvalidateBar();
    }

    void RevealBarForCursor() {
        if (!visible_ || !settings_ || settings_->lock ||
            !control_ || !IsWindow(control_)) return;
        hover_primed_ = true;
        if (!settings_->background_transparent)
            SetControlAlpha(ConfiguredControlAlpha());
        if (!bar_ || !IsWindow(bar_) || IsWindowVisible(bar_)) return;
        RECT bounds{};
        GetWindowRect(control_, &bounds);
        PositionWindows(bounds, false);
        SetBarVisible(true);
    }

    void RequestRender() noexcept {
        if (render_request_pending_ || !paint_ || !IsWindow(paint_)) return;
        if (PostMessageW(paint_, WM_TIMER, kRenderTimer, 0))
            render_request_pending_ = true;
    }

    static LRESULT CALLBACK ControlProc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        const LRESULT result = self
            ? self->HandleControlMessage(window, message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
        if (message == WM_NCDESTROY) {
            if (self && self->control_ == window) {
                self->control_ = nullptr;
                self->visible_ = false;
                self->dragging_ = false;
                self->drag_hit_ = 0;
                self->manual_resizing_ = false;
                self->bar_drag_anchor_valid_ = false;
                self->control_alpha_valid_ = false;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return result;
    }

    static LRESULT CALLBACK PaintProc(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam) {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_TIMER && wparam == kRenderTimer) {
            self->render_request_pending_ = false;
            if (self->visible_) self->RenderLayered();
            return 0;
        }
        if (self && (message == WM_CONTEXTMENU || message == WM_RBUTTONUP)) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (message == WM_RBUTTONUP) ClientToScreen(window, &point);
            self->ShowContextMenu(point);
            return 0;
        }
        if (self && (message == WM_CLOSE ||
            (message == WM_SYSCOMMAND &&
             (wparam & 0xfff0U) == SC_CLOSE))) {
            self->DispatchMain(kCommandShowLyrics,
                               static_cast<LPARAM>(-1));
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
        if (message == WM_NCDESTROY) {
            if (self && self->paint_ == window) {
                self->paint_ = nullptr;
                self->render_request_pending_ = false;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return result;
    }

    static LRESULT CALLBACK BarProc(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        const LRESULT result = self
            ? self->HandleBarMessage(window, message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
        if (message == WM_NCDESTROY) {
            if (self && self->bar_ == window) {
                self->bar_ = nullptr;
                self->tooltip_ = nullptr;
                self->icon_ = nullptr;
                for (auto& button : self->buttons_)
                    button.window = nullptr;
                self->hover_button_ = 0;
                self->pressed_button_ = 0;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return result;
    }

    static LRESULT CALLBACK ButtonSubclassProc(HWND window, UINT message,
        WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
        auto* self = reinterpret_cast<Impl*>(reference);
        if (!self) return DefSubclassProc(window, message, wparam, lparam);
        const UINT command = static_cast<UINT>(GetDlgCtrlID(window));
        if (self->tooltip_ && message >= WM_MOUSEFIRST &&
            message <= WM_MOUSELAST) {
            MSG relay{};
            relay.hwnd = window;
            relay.message = message;
            relay.wParam = wparam;
            relay.lParam = lparam;
            relay.time = static_cast<DWORD>(GetMessageTime());
            GetCursorPos(&relay.pt);
            SendMessageW(self->tooltip_, TTM_RELAYEVENT, 0,
                         reinterpret_cast<LPARAM>(&relay));
        }
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_MOUSEMOVE: {
            RECT client{};
            GetClientRect(window, &client);
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const bool inside = IsWindowEnabled(window) && PtInRect(&client, point);
            if (inside && self->hover_button_ != command) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
                TrackMouseEvent(&tracking);
            }
            const UINT hover = inside ? command
                : self->hover_button_ == command ? 0 : self->hover_button_;
            self->SetButtonInteraction(hover, self->pressed_button_);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (self->hover_button_ == command)
                self->SetButtonInteraction(0, self->pressed_button_);
            return 0;
        case WM_LBUTTONDOWN:
            if (!IsWindowEnabled(window)) return 0;
            SetFocus(window);
            SetCapture(window);
            self->SetButtonInteraction(command, command);
            return 0;
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            RECT client{};
            GetClientRect(window, &client);
            const bool invoke = self->pressed_button_ == command &&
                                PtInRect(&client, point) != FALSE;
            const UINT hover = PtInRect(&client, point) ? command
                : self->hover_button_ == command ? 0 : self->hover_button_;
            // Publish the release once, before synchronous CAPTURECHANGED.
            self->SetButtonInteraction(hover, 0);
            if (GetCapture() == window) ReleaseCapture();
            if (invoke) self->HandleCommand(command);
            return 0;
        }
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            if (self->pressed_button_ == command)
                self->SetButtonInteraction(self->hover_button_, 0);
            if (message == WM_CANCELMODE && GetCapture() == window) ReleaseCapture();
            return 0;
        case WM_ENABLE:
            if (!wparam) {
                self->SetButtonInteraction(self->hover_button_ == command ? 0 : self->hover_button_,
                    self->pressed_button_ == command ? 0 : self->pressed_button_);
                if (GetCapture() == window) ReleaseCapture();
            }
            self->InvalidateBar();
            return 0;
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            self->ShowContextMenu(point);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_SPACE || wparam == VK_RETURN) {
                self->HandleCommand(command);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            if (self->icon_ == window) self->icon_ = nullptr;
            for (auto& button : self->buttons_) {
                if (button.window == window) button.window = nullptr;
            }
            if (self->hover_button_ == command) self->hover_button_ = 0;
            if (self->pressed_button_ == command) self->pressed_button_ = 0;
            RemoveWindowSubclass(window, ButtonSubclassProc, 1);
            break;
        default: break;
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    LRESULT HandleControlMessage(HWND window, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST: return HTCLIENT;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            const HBRUSH brush = CreateSolidBrush(settings_
                ? settings_->background_color : RGB(255, 255, 255));
            FillRect(dc, &client, brush);
            DeleteObject(brush);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_SETCURSOR:
            if (reinterpret_cast<HWND>(wparam) == window &&
                LOWORD(lparam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(window, &point);
                SetCursor(CursorForHit(HitTest(point)));
                RevealBarForCursor();
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            BeginDrag(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_LBUTTONUP:
            EndDrag();
            return 0;
        case WM_MOUSEMOVE:
            if (dragging_ && GetCapture() == window) {
                ContinueDrag(POINT{GET_X_LPARAM(lparam),
                                   GET_Y_LPARAM(lparam)});
            }
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            dragging_ = false;
            drag_hit_ = 0;
            manual_resizing_ = false;
            bar_drag_anchor_valid_ = false;
            return 0;
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ShowContextMenu(point);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kRenderTimer) {
                if (visible_) RenderLayered();
                return 0;
            }
            if (wparam == kHoverTimer) {
                UpdateBarVisibility();
                return 0;
            }
            break;
        case WM_WINDOWPOSCHANGED:
            if (!positioning_) SyncOwnedWindows();
            break;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {
            RECT bounds{};
            GetWindowRect(window, &bounds);
            PositionWindows(bounds, true);
            return 0;
        }
        case WM_SHOWWINDOW:
            if (wparam == FALSE) {
                SetBarVisible(false);
                hover_primed_ = false;
                SetControlAlpha(0);
                if (paint_ && IsWindow(paint_)) ShowWindow(paint_, SW_HIDE);
            } else if (visible_) {
                // An owned popup is temporarily hidden when its owner is
                // minimized.  We explicitly hide the separate paint layer in
                // the FALSE branch, so it must be restored with the control;
                // otherwise only the transparent/background surface returns.
                if (paint_ && IsWindow(paint_))
                    ShowWindow(paint_, SW_SHOWNOACTIVATE);
                SetControlAlpha(RestingControlAlpha());
                RenderLayered();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0U) == SC_CLOSE) {
                DispatchMain(kCommandShowLyrics, static_cast<LPARAM>(-1));
                return 0;
            }
            break;
        case WM_CLOSE:
            DispatchMain(kCommandShowLyrics, static_cast<LPARAM>(-1));
            return 0;
        default: break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT HandleBarMessage(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
        switch (message) {
        case WM_PRINTCLIENT:
            PaintBar(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            PaintBar(dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragging_ && GetCapture() == control_) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                MapWindowPoints(window, control_, &point, 1);
                ContinueDrag(point);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            MapWindowPoints(window, control_, &point, 1);
            BeginDrag(point);
            return 0;
        }
        case WM_LBUTTONUP:
            EndDrag();
            return 0;
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ShowContextMenu(point);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0U) == SC_CLOSE) {
                DispatchMain(kCommandShowLyrics, static_cast<LPARAM>(-1));
                return 0;
            }
            break;
        case WM_CLOSE:
            DispatchMain(kCommandShowLyrics, static_cast<LPARAM>(-1));
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wparam));
            return 0;
        case WM_NOTIFY:
            if (HandleToolTipNotification(lparam)) return 0;
            break;
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_INITMENUPOPUP:
        case WM_MENUSELECT:
        case WM_EXITMENULOOP:
            // TrackPopupMenuEx must retain DeskLrcBar as its foreground owner
            // (00419372), while CPlayerWnd owns the recovered command-bar
            // renderer. Forward the owner-draw/menu lifecycle notifications
            // synchronously so the desktop popup uses the same visual path.
            if (command_target_)
                return SendMessageW(command_target_, message, wparam, lparam);
            break;
        default: break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void CreateBarChildren() {
        if (!bar_ || !IsWindow(bar_)) return;
        const RECT default_icon{5, 4, 21, 20};
        icon_ = CreateWindowExW(0, kSkinIconClass, nullptr,
            WS_CHILD | WS_VISIBLE, default_icon.left, default_icon.top,
            Width(default_icon), Height(default_icon), bar_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandIcon)),
            instance_, nullptr);
        if (icon_) SetWindowSubclass(icon_, ButtonSubclassProc, 1,
                                     reinterpret_cast<DWORD_PTR>(this));

        constexpr std::array<UINT, 13> commands{
            kCommandPlay, kCommandPrevious, kCommandNext, kCommandList,
            kCommandLines, kCommandKaraoke, kCommandSettings, kCommandLock,
            kCommandTopmost, kCommandReturn, kCommandClose,
            kCommandZoomIn, kCommandZoomOut};
        for (size_t index = 0; index < buttons_.size(); ++index) {
            buttons_[index].command = commands[index];
            const RECT bounds = DefaultBarRect(commands[index]);
            buttons_[index].window = CreateWindowExW(0, kSkinButtonClass,
                nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                bounds.left, bounds.top, Width(bounds), Height(bounds), bar_,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(commands[index])),
                instance_, nullptr);
            if (buttons_[index].window) {
                SetWindowSubclass(buttons_[index].window, ButtonSubclassProc,
                                  1, reinterpret_cast<DWORD_PTR>(this));
            }
        }
        CreateTooltips();
        ApplyBarSkin();
    }

    void CreateTooltips() {
        if (!bar_) return;
        // 00418B74 -> 0040EE16 passes zero styles and activates the stock
        // tooltip. No custom wrapping width, always-tip or prefix override.
        tooltip_ = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
            0, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, bar_, nullptr,
            instance_, nullptr);
        if (!tooltip_) return;
        SendMessageW(tooltip_, TTM_ACTIVATE, TRUE, 0);

        std::array<HWND, 14> windows{};
        windows[0] = icon_;
        for (size_t index = 0; index < buttons_.size(); ++index)
            windows[index + 1] = buttons_[index].window;
        for (const HWND control : windows) {
            if (!control) continue;
            TOOLINFOW tool{};
            // The resource-only regression executable intentionally has no
            // comctl v6 manifest.  V1 contains every field used here and is
            // accepted by both the v5 fallback and TTPlayer's v6 runtime.
            tool.cbSize = TTTOOLINFO_V1_SIZE;
            // FUN_0040EE49 installs TTF_IDISHWND and the application message
            // filter relays mouse input.  Our child subclass performs that
            // relay explicitly; asking comctl32 to subclass the same HWND a
            // second time is both unnecessary and rejected by comctl32 v5.
            tool.uFlags = TTF_IDISHWND;
            tool.hwnd = bar_;
            tool.uId = reinterpret_cast<UINT_PTR>(control);
            tool.hinst = instance_;
            // 00418B74 registers -1, not a one-time copy of the resource.
            // 00418958 chooses the current action each time a tip is queried.
            tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0,
                         reinterpret_cast<LPARAM>(&tool));
        }
    }

    std::wstring ToolTipText(HWND control) const {
        if (!control || GetParent(control) != bar_) return {};
        const UINT command = static_cast<UINT>(GetDlgCtrlID(control));
        // These optional actions do not exist in the 5.7.9 resource DLL.
        if (command == kCommandZoomIn || command == kCommandZoomOut)
            return ResourceText(GetModuleHandleW(nullptr), command == kCommandZoomIn
                ? IDS_DESKTOP_LYRIC_ZOOM_IN : IDS_DESKTOP_LYRIC_ZOOM_OUT);
        auto text = ResourceText(resource_module_,
            command == kCommandPlay && playing_ ? kCommandPause : command);
        const auto choose = [&](wchar_t separator, bool second) {
            const size_t split = text.find(separator);
            if (split == std::wstring::npos) return;
            if (second) text.erase(0, split + 1);
            else text.erase(split);
        };
        // 00418958 calls 004C1B58 with exactly these separators/indices.
        // Lines and karaoke advertise the next action, not the current mode.
        if (settings_) {
            if (command == kCommandLines) choose(L'|', settings_->lines == 1);
            else if (command == kCommandKaraoke) choose(L'|', settings_->karaoke_mode);
            else if (command == kCommandTopmost) choose(L'\n', settings_->topmost);
        }
        // Conventional "description\nshort label" fallback. Unlike main
        // window tips, 00418958 does not append a keyboard shortcut.
        if (const size_t split = text.find(L'\n'); split != std::wstring::npos)
            text.erase(0, split + 1);
        return text;
    }

    bool HandleToolTipNotification(LPARAM notification) const {
        if (!notification) return false;
        const auto* header = reinterpret_cast<const NMHDR*>(notification);
        if (header->hwndFrom != tooltip_ ||
            (header->code != TTN_GETDISPINFOW && header->code != TTN_GETDISPINFOA))
            return false;
        const auto text = ToolTipText(reinterpret_cast<HWND>(header->idFrom));
        // The shared notification handler copies into the 80-character
        // NMTTDISPINFO buffer. Leave missing resources empty, not "TTPlayer".
        if (header->code == TTN_GETDISPINFOW) {
            auto* display = reinterpret_cast<NMTTDISPINFOW*>(notification);
            wcsncpy_s(display->szText, text.c_str(), _TRUNCATE);
            display->lpszText = display->szText;
        } else {
            auto* display = reinterpret_cast<NMTTDISPINFOA*>(notification);
            const int length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1,
                                                   nullptr, 0, nullptr, nullptr);
            std::string encoded(static_cast<size_t>(std::max(1, length)), '\0');
            if (length > 0) WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1,
                encoded.data(), length, nullptr, nullptr);
            strncpy_s(display->szText, encoded.c_str(), _TRUNCATE);
            display->lpszText = display->szText;
        }
        return true;
    }

    const skin::SkinElement* ButtonElement(UINT command) const {
        if (!skin_) return nullptr;
        const auto& bar = skin_->DesktopLyricBar();
        switch (command) {
        case kCommandPlay: return playing_ ? &bar.pause : &bar.play;
        case kCommandPrevious: return &bar.previous;
        case kCommandNext: return &bar.next;
        case kCommandList: return &bar.list;
        case kCommandSettings: return &bar.settings;
        case kCommandKaraoke: return &bar.karaoke;
        case kCommandLines: return &bar.lines;
        case kCommandLock: return &bar.lock;
        case kCommandTopmost: return &bar.ontop;
        case kCommandZoomIn: return &bar.zoom_in;
        case kCommandZoomOut: return &bar.zoom_out;
        case kCommandReturn: return &bar.return_to_window;
        case kCommandClose: return &bar.close;
        default: return nullptr;
        }
    }

    void ApplyBarSkin() {
        if (!bar_) return;
        const auto* layout = skin_ ? &skin_->DesktopLyricBar() : nullptr;
        SIZE size{231, 25};
        if (layout && layout->valid && layout->background.size.cx > 0 &&
            layout->background.size.cy > 0) size = layout->background.size;
        SetWindowPos(bar_, nullptr, 0, 0, size.cx, size.cy,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        const COLORREF transparent = layout
            ? layout->transparent_color : RGB(255, 0, 255);
        SetLayeredWindowAttributes(bar_, transparent, 255,
                                   LWA_COLORKEY | LWA_ALPHA);

        RECT icon_bounds{5, 4, 21, 20};
        if (layout && ValidRect(layout->icon)) icon_bounds = layout->icon;
        if (icon_) MoveWindow(icon_, icon_bounds.left, icon_bounds.top,
            Width(icon_bounds), Height(icon_bounds), FALSE);
        for (auto& button : buttons_) {
            RECT bounds = DefaultBarRect(button.command);
            const auto* element = ButtonElement(button.command);
            const bool present = layout && layout->valid
                ? element && element->image && ValidRect(element->bounds)
                : ValidRect(bounds);
            if (element && ValidRect(element->bounds)) bounds = element->bounds;
            if (!button.window) continue;
            // Omitted controls must not leave an invisible default hit target
            // over a differently arranged toolbar. Optional A+/A- stay absent
            // in all existing skins until explicitly declared by their XML.
            ShowWindow(button.window, present ? SW_SHOWNA : SW_HIDE);
            if (present) MoveWindow(button.window, bounds.left, bounds.top,
                Width(bounds), Height(bounds), FALSE);
            if (tooltip_) {
                TOOLINFOW tool{};
                tool.cbSize = TTTOOLINFO_V1_SIZE;
                tool.hwnd = bar_;
                tool.uId = reinterpret_cast<UINT_PTR>(button.window);
                SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
                if (present) {
                    tool.uFlags = TTF_IDISHWND;
                    tool.hinst = instance_;
                    tool.lpszText = LPSTR_TEXTCALLBACKW;
                    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
                }
            }
        }
        RECT control_bounds{};
        GetWindowRect(control_, &control_bounds);
        PositionWindows(control_bounds, false);
        InvalidateBar();
    }

    void PaintBar(HDC dc) const {
        if (!dc || !bar_) return;
        RECT client{};
        GetClientRect(bar_, &client);
        const int width = Width(client), height = Height(client);
        if (width <= 0 || height <= 0) return;
        const HDC canvas = CreateCompatibleDC(dc);
        const HBITMAP buffer = CreateCompatibleBitmap(dc, width, height);
        if (!canvas || !buffer) {
            if (buffer) DeleteObject(buffer);
            if (canvas) DeleteDC(canvas);
            return; // Keep the last frame rather than clearing the live bar.
        }
        const HGDIOBJ previous = SelectObject(canvas, buffer);
        PaintBarContents(canvas, client);
        // Clearing the visible colour-keyed window before drawing each PNG
        // exposed incomplete/transparent frames during hover. Compose every
        // layer offscreen and publish exactly one complete frame instead.
        BitBlt(dc, 0, 0, width, height, canvas, 0, 0, SRCCOPY);
        SelectObject(canvas, previous);
        DeleteObject(buffer);
        DeleteDC(canvas);
    }

    void PaintBarContents(HDC dc, const RECT& client) const {
        const auto* layout = skin_ ? &skin_->DesktopLyricBar() : nullptr;
        const COLORREF transparent = layout
            ? layout->transparent_color : RGB(255, 0, 255);
        const HBRUSH clear = CreateSolidBrush(transparent);
        FillRect(dc, &client, clear);
        DeleteObject(clear);

        if (layout && layout->valid && layout->background.image) {
            layout->background.image.Draw(dc, 0, 0, layout->background.size.cx,
                layout->background.size.cy, 0, 0, layout->background.size.cx,
                layout->background.size.cy);
        }
        if (icon_) {
            RECT bounds{};
            GetWindowRect(icon_, &bounds);
            MapWindowPoints(nullptr, bar_, reinterpret_cast<POINT*>(&bounds), 2);
            HICON icon = skin_ ? skin_->Icon() : nullptr;
            if (!icon && command_target_) {
                icon = reinterpret_cast<HICON>(SendMessageW(
                    command_target_, WM_GETICON, ICON_SMALL, 0));
            }
            if (icon) DrawIconEx(dc, bounds.left, bounds.top, icon,
                Width(bounds), Height(bounds), 0, nullptr, DI_NORMAL);
        }
        for (const auto& button : buttons_) DrawBarButton(dc, button);
    }

    void DrawBarButton(HDC dc, const Button& button) const {
        const auto* element = ButtonElement(button.command);
        if (!element || !element->image || !button.window ||
            !(GetWindowLongPtrW(button.window, GWL_STYLE) & WS_VISIBLE)) return;
        RECT bounds{};
        GetWindowRect(button.window, &bounds);
        MapWindowPoints(nullptr, bar_, reinterpret_cast<POINT*>(&bounds), 2);
        int frame = 0;
        const bool checked =
            (button.command == kCommandKaraoke && settings_ &&
             settings_->karaoke_mode) ||
            (button.command == kCommandLines && settings_ &&
             settings_->lines == 2) ||
            (button.command == kCommandLock && settings_ && settings_->lock) ||
            (button.command == kCommandTopmost && settings_ && settings_->topmost);
        if (!IsWindowEnabled(button.window)) frame = 3;
        else if (pressed_button_ == button.command && hover_button_ == button.command) frame = 2;
        else if (hover_button_ == button.command) frame = 1;
        else if (checked) frame = 2;
        const int frame_width = element->frames > 1
            ? element->image_size.cx / element->frames
            : element->image_size.cx;
        if (frame_width <= 0 || element->image_size.cy <= 0) return;
        // The PNG's HBITMAP is only a compatibility view; SRCCOPY of that
        // view discards alpha and paints black behind transparent sprites.
        // Draw preserves GDI+ source-over for PNG and old SRCCOPY for BMP.
        element->image.Draw(dc, bounds.left, bounds.top, Width(bounds), Height(bounds),
            frame_width * frame, 0, frame_width, element->image_size.cy);
    }

    void HandleCommand(UINT command) {
        if (!settings_) return;
        if (command >= kCommandProfileFirst && command <= kCommandProfileLast) {
            const size_t index = command - kCommandProfileFirst;
            settings_->profile = static_cast<int>(index);
            settings_->current = settings_->profiles[index];
            settings_->current.name.clear();
            mask_cache_dirty_ = true;
            RenderLayered();
        } else {
            switch (command) {
            case kCommandIcon:
                DispatchMain(kCommandIcon,
                    reinterpret_cast<LPARAM>(bar_));
                if (owner_) SetForegroundWindow(owner_);
                break;
            case kCommandPlay:
                DispatchMain(playing_ ? kCommandPause : kCommandPlay, 0);
                break;
            case kCommandPrevious:
            case kCommandNext:
                DispatchMain(command, 0);
                break;
            case kCommandList:
                ShowPlaylistMenu();
                break;
            case kCommandSettings:
                ShowProfileMenu();
                break;
            case kCommandKaraoke:
                settings_->karaoke_mode = !settings_->karaoke_mode;
                mask_cache_dirty_ = true;
                RenderLayered();
                break;
            case kCommandLines:
                SetLines(settings_->lines == 1 ? 2 : 1);
                break;
            case kCommandOneLine:
                SetLines(1);
                break;
            case kCommandTwoLines:
                SetLines(2);
                break;
            case kCommandLock:
                if (command_target_) {
                    DispatchMain(settings_->lock ? kCommandUnlock
                                                 : kCommandLock, 0);
                } else {
                    settings_->lock = !settings_->lock;
                    ApplySettings();
                }
                break;
            case kCommandBackgroundPassthrough:
                settings_->background_transparent =
                    !settings_->background_transparent;
                ApplyClickThrough();
                hover_primed_ = false;
                UpdateBarVisibility();
                break;
            case kCommandTopmost:
                settings_->topmost = !settings_->topmost;
                ApplyTopmost();
                break;
            case kCommandZoomIn:
            case kCommandZoomOut: {
                // Local skin extension, not an undocumented 5.7.9 command.
                // Use the same persistent LOGFONT and layout refresh as the
                // desktop-lyric options page and vertical resize operation.
                const auto raw = static_cast<int64_t>(settings_->font.lfHeight);
                const int current = static_cast<int>(std::clamp<int64_t>(
                    raw == 0 ? 40 : raw < 0 ? -raw : raw, 12, 96));
                settings_->font.lfHeight = -std::clamp(current +
                    (command == kCommandZoomIn ? 2 : -2), 12, 96);
                settings_->font_valid = true;
                ApplySettings();
                break;
            }
            case kCommandReturn:
                DispatchMain(kCommandReturn, 0);
                break;
            case kCommandClose:
                DispatchMain(kCommandShowLyrics, static_cast<LPARAM>(-1));
                break;
            case kCommandDesktopOptions:
                if (command_target_) PostMessageW(command_target_,
                    kMessageShowOptionsControl, 0x181, 7);
                break;
            default:
                DispatchMain(command, 0);
                break;
            }
        }
        InvalidateBar();
    }

    void SetLines(int lines) {
        if (!settings_) return;
        lines = std::clamp(lines, 1, 2);
        if (settings_->lines == lines) return;
        settings_->lines = lines;
        applied_lines_ = 0;
        ApplySettings();
    }

    void DispatchMain(UINT command, LPARAM parameter) const {
        if (command_target_)
            SendMessageW(command_target_, WM_COMMAND,
                MAKEWPARAM(command, 0), parameter);
    }

    UINT TrackMenu(HMENU menu, POINT point) {
        if (!menu) return 0;
        menu_tracking_ = true;
        struct TrackingReset final {
            bool& active;
            ~TrackingReset() { active = false; }
        } reset{menu_tracking_};
        if (popup_tracker_) return popup_tracker_(menu, point, bar_);
        SetForegroundWindow(bar_);
        // 0041914F uses TPM_RETURNCMD (0x100). TPM_NONOTIFY suppresses
        // the root WM_INITMENUPOPUP needed to replace resource 0x7ef4.
        const UINT selected = TrackPopupMenuEx(menu,
            TPM_RIGHTBUTTON | TPM_RETURNCMD,
            point.x, point.y, bar_, nullptr);
        PostMessageW(bar_, WM_NULL, 0, 0);
        return selected;
    }

    void ShowContextMenu(POINT point) {
        if (!resource_module_ || !settings_) return;
        if (point.x == -1 && point.y == -1) {
            RECT bounds{};
            GetWindowRect(control_, &bounds);
            point = POINT{(bounds.left + bounds.right) / 2,
                          (bounds.top + bounds.bottom) / 2};
        }
        const HMENU root = LoadMenuW(resource_module_,
            MAKEINTRESOURCEW(kMenuDesktopLyric));
        if (!root) return;
        const HMENU popup = GetSubMenu(root, 0);
        if (!popup) {
            DestroyMenu(root);
            return;
        }
        if (HMENU profiles = GetSubMenu(popup, 0)) PopulateProfiles(profiles);
        CheckMenuItem(popup, kCommandOneLine, MF_BYCOMMAND |
            (settings_->lines == 1 ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(popup, kCommandTwoLines, MF_BYCOMMAND |
            (settings_->lines == 2 ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(popup, kCommandTopmost, MF_BYCOMMAND |
            (settings_->topmost ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(popup, kCommandBackgroundPassthrough, MF_BYCOMMAND |
            (settings_->background_transparent ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(popup, kCommandLock, MF_BYCOMMAND |
            (settings_->lock ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(popup, kCommandKaraoke, MF_BYCOMMAND |
            (settings_->karaoke_mode ? MF_CHECKED : MF_UNCHECKED));
        const UINT selected = TrackMenu(popup, point);
        DestroyMenu(root);
        if (selected) HandleCommand(selected);
    }

    void ShowProfileMenu() {
        if (!resource_module_ || !settings_) return;
        const HMENU root = LoadMenuW(resource_module_,
            MAKEINTRESOURCEW(kMenuProfiles));
        if (!root) return;
        const HMENU popup = GetSubMenu(root, 0);
        if (!popup) {
            DestroyMenu(root);
            return;
        }
        if (GetMenuItemCount(popup) > 0)
            DeleteMenu(popup, 0, MF_BYPOSITION);
        for (int index = 2; index >= 0; --index) {
            std::wstring name = settings_->profiles[static_cast<size_t>(index)].name;
            if (name.empty()) name = ResourceCommandText(
                resource_module_, kCommandProfileFirst +
                    static_cast<UINT>(index));
            if (name.empty()) name = L"<>";
            UINT flags = MF_BYPOSITION | MF_STRING;
            if (settings_->profile == index) flags |= MF_CHECKED;
            InsertMenuW(popup, 0, flags,
                kCommandProfileFirst + static_cast<UINT>(index), name.c_str());
        }
        RECT anchor{};
        if (HWND button = FindButton(kCommandSettings))
            GetWindowRect(button, &anchor);
        else GetWindowRect(bar_, &anchor);
        const UINT selected = TrackMenu(popup, POINT{anchor.left, anchor.bottom});
        DestroyMenu(root);
        if (selected) HandleCommand(selected);
    }

    void ShowPlaylistMenu() {
        if (!resource_module_) return;
        const HMENU root = LoadMenuW(resource_module_,
            MAKEINTRESOURCEW(kMenuPlaylist));
        if (!root) return;
        const HMENU popup = GetSubMenu(root, 0);
        if (!popup) {
            DestroyMenu(root);
            return;
        }
        RECT anchor{};
        if (HWND button = FindButton(kCommandList)) GetWindowRect(button, &anchor);
        else GetWindowRect(bar_, &anchor);
        const UINT selected = TrackMenu(popup, POINT{anchor.left, anchor.bottom});
        DestroyMenu(root);
        if (selected > 9999 && selected < 30000) DispatchMain(selected, 0);
    }

    void PopulateProfiles(HMENU menu) const {
        if (!menu || !settings_) return;
        while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
        for (size_t index = 0; index < settings_->profiles.size(); ++index) {
            std::wstring name = settings_->profiles[index].name;
            if (name.empty()) name = ResourceCommandText(resource_module_,
                kCommandProfileFirst + static_cast<UINT>(index));
            if (name.empty()) name = L"<>";
            UINT flags = MF_STRING;
            if (settings_->profile == static_cast<int>(index))
                flags |= MF_CHECKED;
            AppendMenuW(menu, flags,
                kCommandProfileFirst + static_cast<UINT>(index), name.c_str());
        }
    }

    HWND FindButton(UINT command) const noexcept {
        const auto found = std::find_if(buttons_.begin(), buttons_.end(),
            [command](const Button& value) { return value.command == command; });
        return found == buttons_.end() ? nullptr : found->window;
    }

    unsigned int HitTest(POINT point) const {
        RECT client{};
        GetClientRect(control_, &client);
        const int edge_x = std::min(4, GetSystemMetrics(SM_CXFRAME));
        const int edge_y = std::min(4, GetSystemMetrics(SM_CYFRAME));
        unsigned int result{};
        if (point.x >= client.right - edge_x && point.x <= client.right)
            result |= kDragRight;
        else if (point.x >= client.left && point.x <= client.left + edge_x)
            result |= kDragLeft;
        if (point.y >= client.bottom - edge_y && point.y <= client.bottom)
            result |= kDragBottom;
        else if (point.y >= client.top && point.y <= client.top + edge_y)
            result |= kDragTop;
        if ((result & (kDragLeft | kDragRight)) == 0 &&
            (result & (kDragTop | kDragBottom)) != 0) {
            if (point.x <= edge_x * 2) result |= kDragLeft;
            else if (point.x >= client.right - edge_x * 2)
                result |= kDragRight;
        } else if ((result & (kDragLeft | kDragRight)) != 0 &&
                   (result & (kDragTop | kDragBottom)) == 0) {
            if (point.y <= edge_y * 2) result |= kDragTop;
            else if (point.y >= client.bottom - edge_y * 2)
                result |= kDragBottom;
        }
        return result == 0 ? kDragInterior : result;
    }

    static HCURSOR CursorForHit(unsigned int hit) {
        // FUN_00415CB8 loads system cursor resource 0x7F89 on NT 5+, which
        // is IDC_HAND (the pre-NT branch builds an equivalent custom cursor).
        LPCWSTR cursor = IDC_HAND;
        if (hit == kDragLeft || hit == kDragRight) cursor = IDC_SIZEWE;
        else if (hit == kDragTop || hit == kDragBottom) cursor = IDC_SIZENS;
        else if (hit == (kDragLeft | kDragTop) ||
                 hit == (kDragRight | kDragBottom)) cursor = IDC_SIZENWSE;
        else if (hit == (kDragRight | kDragTop) ||
                 hit == (kDragLeft | kDragBottom)) cursor = IDC_SIZENESW;
        return LoadCursorW(nullptr, cursor);
    }

    void BeginDrag(POINT client_point) {
        if (!control_ || (settings_ && settings_->lock)) return;
        drag_client_anchor_ = client_point;
        bar_drag_anchor_valid_ = false;
        if (bar_ && IsWindow(bar_)) {
            POINT screen_point = client_point;
            ClientToScreen(control_, &screen_point);
            RECT bar_bounds{};
            GetWindowRect(bar_, &bar_bounds);
            if (PtInRect(&bar_bounds, screen_point)) {
                bar_drag_anchor_ = screen_point;
                ScreenToClient(bar_, &bar_drag_anchor_);
                bar_drag_anchor_valid_ = true;
            }
        }
        drag_hit_ = HitTest(client_point);
        manual_resizing_ = drag_hit_ > 1;
        dragging_ = true;
        SetCapture(control_);
    }

    void EndDrag() {
        if (GetCapture() == control_) ReleaseCapture();
        dragging_ = false;
        drag_hit_ = 0;
        manual_resizing_ = false;
        bar_drag_anchor_valid_ = false;
        CaptureBounds();
    }

    void ContinueDrag(POINT client_point) {
        if (!control_ || GetCapture() != control_) return;
        const int delta_x = client_point.x - drag_client_anchor_.x;
        const int delta_y = client_point.y - drag_client_anchor_.y;
        if (delta_x == 0 && delta_y == 0) return;

        RECT before{};
        GetWindowRect(control_, &before);
        RECT target = before;
        RECT old_bar{};
        if (bar_ && IsWindow(bar_)) GetWindowRect(bar_, &old_bar);

        if (settings_ && settings_->auto_width)
            drag_hit_ &= ~(kDragLeft | kDragRight);
        if (drag_hit_ < 2) {
            MoveAndSnapToWorkArea(target, delta_x, delta_y);
        } else {
            if ((drag_hit_ & kDragLeft) != 0) target.left += delta_x;
            if ((drag_hit_ & kDragRight) != 0) target.right += delta_x;
            if ((drag_hit_ & kDragTop) != 0) target.top += delta_y;
            if ((drag_hit_ & kDragBottom) != 0) target.bottom += delta_y;

            int width = Width(target);
            if (width < kMinimumWidth) {
                if ((drag_hit_ & kDragLeft) != 0) target.left = target.right - kMinimumWidth;
                else target.right = target.left + kMinimumWidth;
            } else if (width > kMaximumWidth) {
                if ((drag_hit_ & kDragLeft) != 0) target.left = target.right - kMaximumWidth;
                else target.right = target.left + kMaximumWidth;
            }
            const int lines = settings_ ? std::clamp(settings_->lines, 1, 2) : 1;
            const int minimum = kMinimumLineHeight * lines;
            const int maximum = kMaximumLineHeight * lines;
            const int height = Height(target);
            if (height < minimum) {
                if ((drag_hit_ & kDragTop) != 0) target.top = target.bottom - minimum;
                else target.bottom = target.top + minimum;
            } else if (height > maximum) {
                if ((drag_hit_ & kDragTop) != 0) target.top = target.bottom - maximum;
                else target.bottom = target.top + maximum;
            }
            if ((drag_hit_ & kDragRight) != 0)
                drag_client_anchor_.x = client_point.x;
            if ((drag_hit_ & kDragBottom) != 0)
                drag_client_anchor_.y = client_point.y;
        }
        PositionWindows(target, true);

        RECT after{};
        GetWindowRect(control_, &after);
        if (bar_drag_anchor_valid_ && drag_hit_ < 2 &&
            bar_ && IsWindow(bar_) && !EqualRect(&before, &target)) {
            RECT new_bar{};
            GetWindowRect(bar_, &new_bar);
            const bool old_above = old_bar.top < before.top;
            // 00419E3E compares both toolbar positions against the control's
            // pre-move top, not against the new control rectangle.
            const bool new_above = new_bar.top < before.top;
            if (old_above != new_above || EqualRect(&old_bar, &new_bar)) {
                POINT cursor{new_bar.left + bar_drag_anchor_.x,
                             new_bar.top + bar_drag_anchor_.y};
                SetCursorPos(cursor.x, cursor.y);
                drag_client_anchor_ = cursor;
                ScreenToClient(control_, &drag_client_anchor_);
            }
        }

        if (settings_ &&
            (drag_hit_ & (kDragTop | kDragBottom)) != 0 && delta_y != 0) {
            const int lines = std::clamp(settings_->lines, 1, 2);
            const int pixel_height = std::max(1,
                ((Height(after) / lines) * 2) / 3 - 4);
            settings_->font.lfHeight = -pixel_height;
            settings_->font_valid = true;
            RebuildFont();
            last_font_ = settings_->font;
        }
        mask_cache_dirty_ = true;
        RequestRender();
    }

    void PositionWindows(RECT target, bool persist) {
        if (!control_ || !IsWindow(control_)) return;
        target.right = target.left + std::clamp(Width(target),
            kMinimumWidth, kMaximumWidth);
        const RECT work = DragWorkAreaForRect(target);
        RECT bar_rect{};
        if (bar_ && IsWindow(bar_)) GetWindowRect(bar_, &bar_rect);
        const int bar_width = std::max(1, Width(bar_rect));
        const int bar_height = std::max(1, Height(bar_rect));
        const int desktop_left = work.left;
        const int desktop_top = work.top;

        // FUN_00419983 keeps the whole lyric vertically visible, but only
        // requires one toolbar-width strip to remain reachable horizontally.
        // This is intentionally less restrictive than clamping the complete
        // lyric rectangle into the work area.
        if (target.top < desktop_top)
            OffsetRect(&target, 0, desktop_top - target.top);
        else if (target.bottom > work.bottom)
            OffsetRect(&target, 0, work.bottom - target.bottom);
        if (target.right < desktop_left + bar_width) {
            OffsetRect(&target, desktop_left + bar_width - target.right, 0);
        } else if (target.left > work.right - bar_width) {
            OffsetRect(&target, work.right - bar_width - target.left, 0);
        }

        int bar_x = target.left + (Width(target) - bar_width) / 2;
        const int work_right = static_cast<int>(work.right);
        bar_x = std::clamp(bar_x, desktop_left,
            std::max(desktop_left, work_right - bar_width));
        const int bar_y = target.top - bar_height > desktop_top
            ? target.top - bar_height : target.bottom;

        positioning_ = true;
        const bool have_paint = paint_ && IsWindow(paint_);
        const bool have_bar = bar_ && IsWindow(bar_);
        HDWP positions = BeginDeferWindowPos(
            1 + static_cast<int>(have_paint) + static_cast<int>(have_bar));
        if (positions) {
            positions = DeferWindowPos(positions, control_, nullptr,
                target.left, target.top, Width(target), Height(target),
                SWP_NOZORDER | SWP_NOACTIVATE);
            if (positions && have_paint) positions = DeferWindowPos(positions, paint_, nullptr,
                target.left, target.top, Width(target), Height(target),
                SWP_NOZORDER | SWP_NOACTIVATE);
            if (positions && have_bar) positions = DeferWindowPos(positions, bar_, nullptr,
                bar_x, bar_y, bar_width, bar_height,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (positions) EndDeferWindowPos(positions);
        } else {
            MoveWindow(control_, target.left, target.top,
                       Width(target), Height(target), FALSE);
            if (have_paint)
                MoveWindow(paint_, target.left, target.top,
                           Width(target), Height(target), FALSE);
            if (have_bar)
                SetWindowPos(bar_, nullptr, bar_x, bar_y, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        positioning_ = false;
        if (persist && persisted_bounds_) *persisted_bounds_ = target;
        mask_cache_dirty_ = true;
    }

    void SyncOwnedWindows() {
        if (!control_ || !IsWindow(control_)) return;
        RECT bounds{};
        GetWindowRect(control_, &bounds);
        PositionWindows(bounds, true);
        RenderLayered();
    }

    void UpdateBarVisibility() {
        if (!visible_ || !bar_ || !IsWindow(bar_) || !control_ ||
            !IsWindow(control_) || !settings_ || settings_->lock) {
            SetBarVisible(false);
            hover_primed_ = false;
            if (!settings_ || !settings_->background_show)
                SetControlAlpha(0);
            return;
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        RECT control_bounds{};
        RECT bar_bounds{};
        GetWindowRect(control_, &control_bounds);
        GetWindowRect(bar_, &bar_bounds);
        const HWND capture = GetCapture();
        const bool over_control = PtInRect(&control_bounds, cursor) != FALSE;
        const bool over_bar = PtInRect(&bar_bounds, cursor) != FALSE;
        const bool captured = capture == control_ || capture == bar_ ||
                              IsChild(bar_, capture) != FALSE;

        if (over_control) {
            if (!hover_primed_) {
                hover_primed_ = true;
                if (settings_->background_transparent) {
                    SetBarVisible(true);
                } else if (!settings_->background_show) {
                    // With alpha zero Windows deliberately omits this layered
                    // HWND from hit testing.  0041A341 raises it to a barely
                    // visible alpha so the next mouse input reaches
                    // WM_SETCURSOR; that handler exposes the configured
                    // background and toolbar before WM_LBUTTONDOWN.
                    SetControlAlpha(HoverProbeAlpha());
                }
            }
            return;
        }
        if (over_bar || captured || menu_tracking_) return;
        if (!hover_primed_ && !IsWindowVisible(bar_)) return;

        hover_primed_ = false;
        SetBarVisible(false);
        if (!settings_->background_show) SetControlAlpha(0);
    }

    void ApplyClickThrough() {
        if (!control_ || !IsWindow(control_) || !settings_) return;
        LONG_PTR style = GetWindowLongPtrW(control_, GWL_EXSTYLE);
        const bool transparent = settings_->lock ||
                                 settings_->background_transparent;
        if (transparent) style |= WS_EX_TRANSPARENT;
        else style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        SetWindowLongPtrW(control_, GWL_EXSTYLE, style);
        SetWindowPos(control_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
            SWP_FRAMECHANGED);
    }

    void ApplyTopmost() const {
        if (!settings_) return;
        const HWND order = settings_->topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
        constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        if (control_ && IsWindow(control_))
            SetWindowPos(control_, order, 0, 0, 0, 0, flags);
        if (paint_ && IsWindow(paint_))
            SetWindowPos(paint_, order, 0, 0, 0, 0, flags);
        if (bar_ && IsWindow(bar_))
            SetWindowPos(bar_, order, 0, 0, 0, 0, flags);
    }

    void RebuildFont() {
        if (!settings_) return;
        if (font_) DeleteObject(font_);
        LOGFONTW descriptor = settings_->font;
        if (descriptor.lfHeight == 0) descriptor.lfHeight = -40;
        if (descriptor.lfWeight == 0) descriptor.lfWeight = FW_BOLD;
        descriptor.lfQuality = ANTIALIASED_QUALITY;
        font_ = CreateFontIndirectW(&descriptor);
        if (!font_) {
            wcsncpy_s(descriptor.lfFaceName, L"Microsoft YaHei",
                      _TRUNCATE);
            font_ = CreateFontIndirectW(&descriptor);
        }
        mask_cache_dirty_ = true;
    }

    int MeasureFontHeight() const {
        if (!font_) return 40;
        ScopedDc screen(nullptr);
        const int configured_height = std::abs(
            static_cast<int>(settings_->font.lfHeight));
        if (!screen.dc) return std::max(1, configured_height);
        const HGDIOBJ old = SelectObject(screen.dc, font_);
        TEXTMETRICW metric{};
        const bool measured = GetTextMetricsW(screen.dc, &metric) != FALSE;
        SelectObject(screen.dc, old);
        return measured ? std::max(1, static_cast<int>(metric.tmHeight))
                        : std::max(1, configured_height);
    }

    std::chrono::milliseconds EffectivePosition() const {
        if (!playing_ || !settings_ || !settings_->smooth)
            return anchor_position_;
        return anchor_position_ +
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - anchor_time_);
    }

    size_t ActiveLine(std::chrono::milliseconds position) const {
        if (!lyrics_ || lyrics_->lines.empty())
            return std::numeric_limits<size_t>::max();
        return lyrics_->LineAt(position).value_or(0);
    }

    std::wstring LineText(size_t index) const {
        if (!lyrics_ || index >= lyrics_->lines.size()) return {};
        return WideLyric(lyrics_->lines[index].text);
    }

    int TextWidth(std::wstring_view text) const {
        if (text.empty() || !font_) return 0;
        ScopedDc screen(nullptr);
        if (!screen.dc) return 0;
        const HGDIOBJ old = SelectObject(screen.dc, font_);
        SIZE extent{};
        GetTextExtentPoint32W(screen.dc, text.data(),
                             static_cast<int>(text.size()), &extent);
        SelectObject(screen.dc, old);
        return extent.cx;
    }

    bool ApplyAutomaticWidth(const std::vector<std::wstring>& text) {
        if (!settings_ || !settings_->auto_width || !control_) return false;
        int desired = kMinimumWidth;
        for (const auto& line : text) desired = std::max(desired,
            TextWidth(line) + 8 + (settings_->border ? 4 : 0) +
            (settings_->shadow ? 3 : 0));
        desired = std::clamp(desired, kMinimumWidth, kMaximumWidth);
        RECT bounds{};
        GetWindowRect(control_, &bounds);
        if (Width(bounds) == desired) return false;
        const int center = bounds.left + Width(bounds) / 2;
        bounds.left = center - desired / 2;
        bounds.right = bounds.left + desired;
        PositionWindows(bounds, true);
        return true;
    }

    GlyphMask BuildMask(std::wstring text, int row, int rows, int height) const {
        GlyphMask result;
        result.text = std::move(text);
        result.row = row;
        if (result.text.empty() || !font_ || height <= 0)
            return result;

        // 00417B8B/004B2B2E cache the whole line, including horizontal padding
        // (eight glyph-render pixels plus the two one-pixel border margins).
        // A window-sized mask permanently clips the text needed while scrolling.
        const int width = TextWidth(result.text) + 10;
        height /= std::max(1, rows);
        if (width <= 0 || height <= 0) return result;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* raw{};
        ScopedDc screen(nullptr);
        if (!screen.dc) return result;
        HBITMAP bitmap = CreateDIBSection(screen.dc, &info, DIB_RGB_COLORS,
                                          &raw, nullptr, 0);
        if (!bitmap || !raw) {
            if (bitmap) DeleteObject(bitmap);
            return result;
        }
        std::memset(raw, 0xff,
                    static_cast<size_t>(width) * height * sizeof(uint32_t));
        HDC memory = CreateCompatibleDC(screen.dc);
        if (!memory) {
            DeleteObject(bitmap);
            return result;
        }
        const HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
        const HGDIOBJ old_font = SelectObject(memory, font_);
        SetBkMode(memory, TRANSPARENT);
        SetTextColor(memory, RGB(0, 0, 0));

        RECT row_bounds{5, 0, width - 5, height};
        const UINT format = DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_LEFT;
        DrawTextW(memory, result.text.c_str(),
                  static_cast<int>(result.text.size()), &row_bounds, format);

        GdiFlush();
        result.width = width;
        result.height = height;
        result.alpha.resize(static_cast<size_t>(width) * height);
        const auto* pixels = static_cast<const unsigned char*>(raw);
        for (size_t pixel = 0; pixel < result.alpha.size(); ++pixel) {
            const unsigned char* source = pixels + pixel * 4;
            const unsigned int luminance =
                (static_cast<unsigned int>(source[0]) + source[1] +
                 source[2]) / 3U;
            result.alpha[pixel] = static_cast<unsigned char>(255U - luminance);
        }
        SelectObject(memory, old_font);
        SelectObject(memory, old_bitmap);
        DeleteDC(memory);
        DeleteObject(bitmap);
        return result;
    }

    void EnsureMasks(size_t current, int width, int height) {
        if (!mask_cache_dirty_ && rendered_line_ == current &&
            mask_width_ == width && mask_height_ == height) return;
        masks_.clear();
        const int rows = settings_ ? std::clamp(settings_->lines, 1, 2) : 1;
        if (current == std::numeric_limits<size_t>::max()) {
            if (!fallback_text_.empty())
                masks_.push_back(BuildMask(fallback_text_, 0, rows, height));
        } else {
            const int current_row = rows == 2
                ? static_cast<int>(current & 1U) : 0;
            masks_.push_back(BuildMask(LineText(current), current_row, rows,
                                       height));
            if (rows == 2 && lyrics_ && current + 1 < lyrics_->lines.size()) {
                masks_.push_back(BuildMask(LineText(current + 1),
                    1 - current_row, rows, height));
            }
        }
        rendered_line_ = current;
        mask_width_ = width;
        mask_height_ = height;
        mask_cache_dirty_ = false;
    }

    double LineFraction(size_t current,
                        std::chrono::milliseconds position) const {
        if (!lyrics_ || current >= lyrics_->lines.size()) return 0.0;
        const auto start = lyrics_->lines[current].time + lyrics_->offset;
        const auto end = current + 1 < lyrics_->lines.size()
            ? lyrics_->lines[current + 1].time + lyrics_->offset
            : start + std::chrono::minutes(1);
        const auto duration = std::max<std::int64_t>(1,
            (end - start).count());
        return std::clamp(
            static_cast<double>((position - start).count()) /
                static_cast<double>(duration), 0.0, 1.0);
    }

    void CompositeMask(std::vector<unsigned char>& target,
                       const GlyphMask& mask, COLORREF solid,
                       const std::array<COLORREF, 3>* gradient,
                       int gradient_count, int line_x, int offset_x, int offset_y,
                       bool outline, int clip_right) const {
        if (!settings_ || mask.alpha.empty()) return;
        const int width = mask_width_;
        const int height = mask_height_;
        const int row_height = height / std::max(1, settings_->lines);
        const int row_top = mask.row * row_height;
        // Iterate only the visible slice, not every pixel of a possibly very
        // long cached line. The same translation applies to all paint layers.
        const int x_begin = std::max(0, line_x + offset_x - (outline ? 1 : 0));
        const int x_end = std::min({width, clip_right,
            line_x + offset_x + mask.width + (outline ? 1 : 0)});
        for (int destination_y = std::max(0, row_top + offset_y);
             destination_y < std::min(height, row_top + offset_y + mask.height);
             ++destination_y) {
            const int y = destination_y - row_top - offset_y;
            for (int destination_x = x_begin; destination_x < x_end;
                 ++destination_x) {
                const int x = destination_x - line_x - offset_x;
                unsigned int alpha{};
                if (!outline) {
                    alpha = mask.alpha[static_cast<size_t>(y) * mask.width + x];
                } else {
                    for (int dy = -1; dy <= 1; ++dy) {
                        const int source_y = y + dy;
                        if (source_y < 0 || source_y >= mask.height) continue;
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int source_x = x + dx;
                            if (source_x < 0 || source_x >= mask.width) continue;
                            alpha = std::max<unsigned int>(alpha,
                                mask.alpha[static_cast<size_t>(source_y) *
                                           mask.width + source_x]);
                        }
                    }
                }
                alpha = alpha * static_cast<unsigned int>(
                    settings_->text_alpha) / 255U;
                if (alpha == 0) continue;
                COLORREF color = solid;
                if (gradient) color = GradientColor(*gradient,
                    gradient_count, std::clamp(y, 0, row_height - 1),
                    row_height);
                CompositePixel(target.data() +
                    (static_cast<size_t>(destination_y) * width +
                     destination_x) * 4, color, alpha);
            }
        }
    }

    void RenderLayered() {
        if (!paint_ || !control_ || !IsWindow(paint_) ||
            !IsWindow(control_) || !settings_) return;
        RECT bounds{};
        GetWindowRect(control_, &bounds);
        const int width = Width(bounds);
        const int height = Height(bounds);
        if (width <= 0 || height <= 0) return;
        const auto position = EffectivePosition();
        const size_t current = ActiveLine(position);

        std::vector<std::wstring> text;
        if (current == std::numeric_limits<size_t>::max()) {
            if (!fallback_text_.empty()) text.push_back(fallback_text_);
        } else {
            text.push_back(LineText(current));
            if (settings_->lines == 2 && lyrics_ &&
                current + 1 < lyrics_->lines.size())
                text.push_back(LineText(current + 1));
        }
        if (ApplyAutomaticWidth(text)) {
            GetWindowRect(control_, &bounds);
        }
        const int render_width = Width(bounds);
        const int render_height = Height(bounds);
        EnsureMasks(current, render_width, render_height);

        std::vector<unsigned char> pixels(
            static_cast<size_t>(render_width) * render_height * 4, 0);
        for (size_t index = 0; index < masks_.size(); ++index) {
            const GlyphMask& mask = masks_[index];
            const bool active = index == 0 &&
                current != std::numeric_limits<size_t>::max();
            const int played_pixels = active
                ? static_cast<int>(std::lround(mask.width * LineFraction(current, position)))
                : 0;
            const auto viewport = CalculateDesktopLyricViewport(mask.width,
                render_width, played_pixels, settings_->align,
                settings_->lines, mask.row);
            const int line_x = viewport.destination_x - viewport.source_x;
            if (settings_->shadow)
                CompositeMask(pixels, mask, settings_->shadow_color, nullptr,
                              0, line_x, 2, 2, false, render_width);
            if (settings_->border)
                CompositeMask(pixels, mask, settings_->border_color, nullptr,
                              0, line_x, 0, 0, true, render_width);
            CompositeMask(pixels, mask, 0, &settings_->current.background_colors,
                settings_->current.background_count, line_x, 0, 0, false,
                render_width);
            if (active) {
                const int clip = settings_->karaoke_mode
                    ? viewport.played_right : render_width;
                CompositeMask(pixels, mask, 0,
                    &settings_->current.played_colors,
                    settings_->current.played_count, line_x, 0, 0, false, clip);
            }
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = render_width;
        info.bmiHeader.biHeight = -render_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        ScopedDc screen(nullptr);
        if (!screen.dc) return;
        void* raw{};
        const HBITMAP bitmap = CreateDIBSection(screen.dc, &info,
            DIB_RGB_COLORS, &raw, nullptr, 0);
        if (!bitmap || !raw) {
            if (bitmap) DeleteObject(bitmap);
            return;
        }
        std::memcpy(raw, pixels.data(), pixels.size());
        const HDC memory = CreateCompatibleDC(screen.dc);
        const HGDIOBJ old = SelectObject(memory, bitmap);
        POINT source{};
        POINT destination{bounds.left, bounds.top};
        SIZE size{render_width, render_height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(paint_, screen.dc, &destination, &size, memory,
                            &source, 0, &blend, ULW_ALPHA);
        SelectObject(memory, old);
        DeleteDC(memory);
        DeleteObject(bitmap);
    }

    HINSTANCE instance_{};
    HWND owner_{};
    HWND command_target_{};
    HMODULE resource_module_{};
    settings::DesktopLyricSettings* settings_{};
    RECT* persisted_bounds_{};
    PopupMenuTracker popup_tracker_;
    const skin::LegacySkin* skin_{};
    const lyrics::Lyrics* lyrics_{};
    HWND control_{};
    HWND paint_{};
    HWND bar_{};
    HWND tooltip_{};
    HWND icon_{};
    std::array<Button, 13> buttons_{};
    HFONT font_{};
    LOGFONTW last_font_{};
    int applied_lines_{};
    std::wstring fallback_text_;
    std::chrono::milliseconds anchor_position_{};
    std::chrono::steady_clock::time_point anchor_time_{
        std::chrono::steady_clock::now()};
    bool playing_{};
    bool visible_{};
    bool positioning_{};
    bool dragging_{};
    unsigned int drag_hit_{};
    bool manual_resizing_{};
    POINT drag_client_anchor_{};
    POINT bar_drag_anchor_{};
    bool bar_drag_anchor_valid_{};
    BYTE control_alpha_{};
    bool control_alpha_valid_{};
    bool hover_primed_{};
    bool menu_tracking_{};
    bool render_request_pending_{};
    UINT hover_button_{};
    UINT pressed_button_{};
    bool mask_cache_dirty_{true};
    size_t rendered_line_{std::numeric_limits<size_t>::max()};
    int mask_width_{};
    int mask_height_{};
    std::vector<GlyphMask> masks_;
};

DesktopLyricsWindow::DesktopLyricsWindow() : impl_(std::make_unique<Impl>()) {}
DesktopLyricsWindow::~DesktopLyricsWindow() = default;

bool DesktopLyricsWindow::Create(HINSTANCE instance, HWND lyric_owner,
    HWND command_target, HMODULE resource_module,
    settings::DesktopLyricSettings* settings, RECT* persisted_bounds,
    PopupMenuTracker popup_tracker) {
    return impl_->Create(instance, lyric_owner, command_target, resource_module,
                         settings, persisted_bounds,
                         std::move(popup_tracker));
}

void DesktopLyricsWindow::Destroy() noexcept { impl_->Destroy(); }
void DesktopLyricsWindow::SetSkin(const skin::LegacySkin* skin) {
    impl_->SetSkin(skin);
}
void DesktopLyricsWindow::SetLyrics(const lyrics::Lyrics* lyrics) {
    impl_->SetLyrics(lyrics);
}
void DesktopLyricsWindow::SetFallbackText(std::wstring text) {
    impl_->SetFallbackText(std::move(text));
}
void DesktopLyricsWindow::UpdatePlayback(std::chrono::milliseconds position,
                                         bool playing) {
    impl_->UpdatePlayback(position, playing);
}
void DesktopLyricsWindow::ApplySettings() { impl_->ApplySettings(); }
void DesktopLyricsWindow::CaptureBounds() noexcept { impl_->CaptureBounds(); }
void DesktopLyricsWindow::Show(bool visible) { impl_->Show(visible); }
void DesktopLyricsWindow::Toggle() { impl_->Toggle(); }
bool DesktopLyricsWindow::Visible() const noexcept { return impl_->Visible(); }
HWND DesktopLyricsWindow::ControlHandle() const noexcept {
    return impl_->ControlHandle();
}
HWND DesktopLyricsWindow::PaintHandle() const noexcept {
    return impl_->PaintHandle();
}
HWND DesktopLyricsWindow::BarHandle() const noexcept {
    return impl_->BarHandle();
}

} // namespace ttplayer::ui
