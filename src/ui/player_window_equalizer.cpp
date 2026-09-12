#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "modern_file_dialog.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <commctrl.h>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <windowsx.h>

namespace ttplayer::ui {
using namespace detail;

LRESULT PlayerWindow::HandleEqualizerControlMessage(HWND control, UINT message,
                                                     WPARAM wparam,
                                                     LPARAM lparam) {
    const HWND parent = GetParent(control);
    const auto forward_point = [&](UINT forwarded) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        MapWindowPoints(control, parent, &point, 1);
        return SendMessageW(parent, forwarded, wparam,
            MAKELPARAM(static_cast<short>(point.x),
                       static_cast<short>(point.y)));
    };
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        // The equalizer owner paints the complete composited skin, including
        // child rectangles.  Native SkinButton/SkinSlider windows have no
        // class brush, so validating without erasing preserves that surface.
        PAINTSTRUCT paint{};
        BeginPaint(control, &paint);
        EndPaint(control, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        return forward_point(message);
    }
    case WM_LBUTTONDOWN:
        SetFocus(control);
        return forward_point(message);
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        return forward_point(message);
    case WM_MOUSELEAVE:
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
    case WM_KEYDOWN:
        return SendMessageW(parent, message, wparam, lparam);
    case WM_CONTEXTMENU:
        // WM_CONTEXTMENU already carries screen coordinates.
        return SendMessageW(parent, message, wparam, lparam);
    case WM_SETCURSOR:
        return SendMessageW(parent, message, wparam, lparam);
    case WM_ENABLE:
        InvalidateRect(parent, nullptr, FALSE);
        return 0;
    default:
        return DefWindowProcW(control, message, wparam, lparam);
    }
}

LRESULT PlayerWindow::HandleEqualizerMessage(UINT message, WPARAM wparam,
                                              LPARAM lparam) {
    const auto is_slider = [](int hit) {
        return hit == kEqSliderBalance || hit == kEqSliderSurround ||
               hit == kEqSliderPreamp ||
               (hit >= kEqSliderFirstBand && hit <= kEqSliderLastBand);
    };
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(equalizer_window_, &point);
        if (is_slider(EqualizerHitTest(point))) {
            // FUN_00452518 uses the same IDC_HAND cursor (0x7F89) over the
            // whole legacy slider client, not only over its thumb bitmap.
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(equalizer_window_, &paint);
        PaintEqualizer(dc);
        EndPaint(equalizer_window_, &paint);
        return 0;
    }
    case WM_CLOSE:
        SetEqualizerTrackingStatus(0, false);
        SetSkinWindowVisible(equalizer_window_, false);
        settings_.player.equalizer_visible = false;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1) {
            RECT bounds{};
            GetWindowRect(equalizer_window_, &bounds);
            point = {(bounds.left + bounds.right) / 2,
                     (bounds.top + bounds.bottom) / 2};
        }
        ShowEqualizerProfileMenu(point);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (dragging_skin_background_ && skin_drag_window_ == equalizer_window_ &&
            GetCapture() == equalizer_window_) {
            ContinueSkinBackgroundDrag(equalizer_window_, point);
            return 0;
        }
        if (equalizer_active_slider_ &&
            EqualizerHasCapture(equalizer_active_slider_)) {
            SetEqualizerSliderFromPoint(equalizer_active_slider_, point, true);
            return 0;
        }
        if (equalizer_track_capture_slider_ &&
            EqualizerHasCapture(equalizer_track_capture_slider_)) {
            const RECT thumb = EqualizerSliderThumbRect(
                equalizer_track_capture_slider_);
            if (!PtInRect(&thumb, point)) {
                equalizer_track_capture_slider_ = 0;
                ReleaseEqualizerCapture();
                equalizer_hover_ = 0;
                InvalidateRect(equalizer_window_, nullptr, FALSE);
                return 0;
            }
        }
        int hit = EqualizerHitTest(point);
        if (is_slider(hit)) {
            const RECT thumb = EqualizerSliderThumbRect(hit);
            if (!PtInRect(&thumb, point)) hit = 0;
        }
        // FUN_00452326 captures a slider as soon as the pointer enters its
        // thumb and retains that capture until the pointer leaves.  Capture
        // is also what selects the second (hot) frame in FUN_00451F59.
        if (is_slider(hit) && !EqualizerHasCapture(hit)) {
            CaptureEqualizerControl(hit);
            // SetCapture synchronously notifies the previous control. Keep
            // the new control's state after that notification, as the
            // original per-SkinSlider object naturally does.
            equalizer_track_capture_slider_ = hit;
        } else if (hit != 0 && !is_slider(hit)) {
            if (equalizer_pressed_ && equalizer_pressed_ != hit) {
                // A captured SkinButton never transfers a held press to a
                // neighbouring button merely because the pointer crossed it.
                hit = 0;
            } else if (!equalizer_pressed_) {
                const HWND target = EqualizerControlWindow(hit);
                if (GetCapture() && GetCapture() != target &&
                    EqualizerOwnsCapture()) {
                    ReleaseEqualizerCapture();
                    equalizer_hover_ = 0;
                    InvalidateRect(equalizer_window_, nullptr, FALSE);
                    return 0;
                }
                if (target && GetCapture() != target) {
                    CaptureEqualizerControl(hit);
                    // See 0040A5E3: SetCapture first notifies the old child;
                    // commit the entered state only afterwards. Leave that
                    // commit to the common transition below so it also
                    // invalidates the owner-painted button rectangle. Setting
                    // equalizer_hover_ here used to suppress that redraw:
                    // capture changed correctly, but profile/reset/close kept
                    // displaying frame zero until some unrelated paint.
                }
            }
        } else if (!equalizer_pressed_ && !equalizer_active_slider_ &&
                   !equalizer_track_capture_slider_ && EqualizerOwnsCapture()) {
            ReleaseEqualizerCapture();
            hit = 0;
        }
        if (hit != equalizer_hover_) {
            equalizer_hover_ = hit;
            InvalidateRect(equalizer_window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        equalizer_hover_ = 0;
        InvalidateRect(equalizer_window_, nullptr, FALSE);
        return 0;
    case WM_NOTIFY:
        if (HandleToolTipNotification(equalizer_window_, lparam)) return 0;
        break;
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int hit = EqualizerHitTest(point);
        if (!hit) {
            equalizer_pressed_ = 0;
            BeginSkinBackgroundDrag(equalizer_window_, point);
        } else if (is_slider(hit)) {
            const RECT thumb = EqualizerSliderThumbRect(hit);
            const bool vertical = skin_ && (hit == kEqSliderPreamp ||
                (hit >= kEqSliderFirstBand && hit <= kEqSliderLastBand) ||
                (hit == kEqSliderSurround && skin_->Equalizer().surround.vertical) ||
                (hit == kEqSliderBalance && skin_->Equalizer().balance.vertical));
            equalizer_focused_slider_ = hit;
            equalizer_pressed_ = 0;
            equalizer_slider_initial_value_ = EqualizerSliderValue(hit);
            if (const HWND control = EqualizerControlWindow(hit))
                SetFocus(control);
            else
                SetFocus(equalizer_window_);
            if (PtInRect(&thumb, point)) {
                equalizer_slider_drag_offset_ = vertical
                    ? point.y - thumb.top : point.x - thumb.left;
                CaptureEqualizerControl(hit);
                equalizer_active_slider_ = hit;
                equalizer_track_capture_slider_ = 0;
            } else {
                equalizer_slider_drag_offset_ = vertical
                    ? (thumb.bottom - thumb.top) / 2
                    : (thumb.right - thumb.left) / 2;
                SetEqualizerSliderFromPoint(hit, point, false);
                CaptureEqualizerControl(hit);
                equalizer_active_slider_ = 0;
                equalizer_track_capture_slider_ = hit;
            }
        } else {
            CaptureEqualizerControl(hit);
            equalizer_hover_ = hit;
            equalizer_pressed_ = hit;
            InvalidateRect(equalizer_window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (equalizer_active_slider_) {
            const int slider = equalizer_active_slider_;
            equalizer_active_slider_ = 0;
            equalizer_pressed_ = 0;
            ReleaseEqualizerCapture();
            // FUN_00452295 sends SB_THUMBPOSITION (4); FUN_00460AB1
            // restores CPlayerWnd's stored playback status before the
            // equalizer commits its final value/profile state.
            SetEqualizerTrackingStatus(slider, false);
            // FUN_00452295 sends the final SB_THUMBPOSITION even when the
            // pointer never moved; CEqualizerWnd consequently changes a
            // named profile to Custom on a simple thumb click as well.
            SetEqualizerSliderValue(slider, EqualizerSliderValue(slider), true);
            InvalidateRect(equalizer_window_, nullptr, FALSE);
            return 0;
        }
        if (equalizer_track_capture_slider_) {
            const int slider = equalizer_track_capture_slider_;
            const RECT thumb = EqualizerSliderThumbRect(slider);
            if (PtInRect(&thumb, point)) {
                // FUN_00452295 leaves the non-pressed hover capture in place
                // when release is still over the thumb.
                equalizer_hover_ = slider;
            } else {
                equalizer_track_capture_slider_ = 0;
                equalizer_hover_ = 0;
                ReleaseEqualizerCapture();
            }
            InvalidateRect(equalizer_window_, nullptr, FALSE);
            return 0;
        }
        const int released = EqualizerHitTest(point);
        const int control = std::exchange(equalizer_pressed_, 0);
        if (dragging_skin_background_ && skin_drag_window_ == equalizer_window_)
            EndSkinMouseCapture();
        else ReleaseEqualizerCapture();
        if (control != 0 && control == released) {
            POINT screen = point;
            ClientToScreen(equalizer_window_, &screen);
            InvokeEqualizerControl(control, screen);
        }
        InvalidateRect(equalizer_window_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE &&
            (equalizer_active_slider_ || equalizer_track_capture_slider_)) {
            const int slider = equalizer_active_slider_
                ? equalizer_active_slider_ : equalizer_track_capture_slider_;
            SetEqualizerSliderValue(slider,
                                    equalizer_slider_initial_value_, true);
            equalizer_active_slider_ = 0;
            equalizer_track_capture_slider_ = 0;
            equalizer_pressed_ = 0;
            ReleaseEqualizerCapture();
            return 0;
        }
        if ((wparam == VK_LEFT || wparam == VK_RIGHT ||
             wparam == VK_UP || wparam == VK_DOWN) &&
            is_slider(equalizer_focused_slider_)) {
            bool vertical = equalizer_focused_slider_ == kEqSliderPreamp ||
                (equalizer_focused_slider_ >= kEqSliderFirstBand &&
                 equalizer_focused_slider_ <= kEqSliderLastBand);
            if (skin_ && equalizer_focused_slider_ == kEqSliderBalance)
                vertical = skin_->Equalizer().balance.vertical;
            if (skin_ && equalizer_focused_slider_ == kEqSliderSurround)
                vertical = skin_->Equalizer().surround.vertical;
            if ((vertical && wparam != VK_UP && wparam != VK_DOWN) ||
                (!vertical && wparam != VK_LEFT && wparam != VK_RIGHT))
                return 0;
            const bool increase = wparam == VK_RIGHT || wparam == VK_UP;
            // FUN_00452413 reports keyboard changes as SB_THUMBPOSITION,
            // which restores rather than replaces the main status text.
            SetEqualizerTrackingStatus(equalizer_focused_slider_, false);
            SetEqualizerSliderValue(equalizer_focused_slider_,
                EqualizerSliderValue(equalizer_focused_slider_) +
                    (increase ? 1 : -1), true);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (skin_drag_window_ == equalizer_window_) EndSkinMouseCapture();
        SetEqualizerTrackingStatus(0, false);
        equalizer_active_slider_ = 0;
        equalizer_track_capture_slider_ = 0;
        equalizer_pressed_ = 0;
        InvalidateRect(equalizer_window_, nullptr, FALSE);
        return 0;
    case WM_CANCELMODE:
        if (equalizer_active_slider_)
            SetEqualizerSliderValue(equalizer_active_slider_,
                                    equalizer_slider_initial_value_, true);
        equalizer_active_slider_ = 0;
        equalizer_track_capture_slider_ = 0;
        equalizer_pressed_ = 0;
        SetEqualizerTrackingStatus(0, false);
        ReleaseEqualizerCapture();
        EndSkinMouseCapture();
        InvalidateRect(equalizer_window_, nullptr, FALSE);
        return 0;
    case WM_COMMAND:
        if (HandleEqualizerCommand(LOWORD(wparam))) return 0;
        break;
    case WM_DRAWITEM:
        // The 0x90 popup is converted to the same owner-draw command-bar
        // records as the player and playlist menus.  TrackPopupMenuEx sends
        // these messages to the equalizer owner HWND, not to the main HWND.
        if (lparam && DrawPopupMenuItem(
                *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_MEASUREITEM:
        if (lparam && MeasurePopupMenuItem(
                *reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_DESTROY:
        if (skin_drag_window_ == equalizer_window_) EndSkinMouseCapture();
        SetEqualizerTrackingStatus(0, false);
        DestroyEqualizerControls();
        RemoveToolTipTools(equalizer_window_);
        equalizer_window_ = nullptr;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(equalizer_window_, message, wparam, lparam);
}

bool PlayerWindow::CreateEqualizerWindow() {
    if (equalizer_window_) return true;
    if (!skin_ || !skin_->Equalizer().valid) return false;
    const auto& layout = skin_->Equalizer();
    RECT player{};
    GetWindowRect(window_, &player);
    int x = player.left + layout.position.left;
    int y = player.top + layout.position.top;
    const RECT saved = settings_.player.equalizer_window;
    if (saved.right > saved.left && saved.bottom > saved.top) {
        x = saved.left;
        y = saved.top;
    }
    const int width = layout.background.size.cx;
    const int height = layout.background.size.cy;
    if (width <= 0 || height <= 0) return false;
    // FUN_0046AF50 creates "Equalizer" as an owned WS_POPUP with
    // WS_EX_TOOLWINDOW (0x80). It deliberately has no native caption/frame.
    equalizer_window_ = CreateWindowExW(WS_EX_TOOLWINDOW,
        kEqualizerWindowClass, L"Equalizer", WS_POPUP,
        x, y, width, height, window_, nullptr, instance_, this);
    if (!equalizer_window_) return false;
    CreateEqualizerControls();
    CreateToolTipWindow();
    UpdateEqualizerToolRects();
    UpdateEqualizerWindowRegion();
    return true;
}

void PlayerWindow::CreateEqualizerControls() {
    if (!equalizer_window_ || !skin_ || !skin_->Equalizer().valid) return;
    const auto& layout = skin_->Equalizer();
    const auto create = [this](int hit, int identifier,
                               const skin::SkinElement& element,
                               const wchar_t* class_name) {
        const bool slider = class_name == kEqualizerSliderClass;
        const bool present = !((!slider && !element.image) ||
            (slider && !element.thumb_image && !element.fill_image &&
             !element.bar_image));
        const RECT bounds = EqualizerElementBounds(element);
        const auto found = std::find_if(equalizer_controls_.begin(), equalizer_controls_.end(),
            [hit](const auto& entry) { return entry.first == hit; });
        HWND control = found == equalizer_controls_.end() ? nullptr : found->second;
        if (!control) {
            control = CreateWindowExW(0, class_name, nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, equalizer_window_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)), instance_, this);
            if (control) equalizer_controls_.emplace_back(hit, control);
        }
        // 0042955B updates bitmap/value bindings and positions the controls
        // created by 00429AAA. Absent skin elements do not destroy their HWND.
        if (control) SetWindowPos(control, nullptr,
            present ? bounds.left : -1000, present ? bounds.top : -1000,
            present ? std::max<LONG>(0, bounds.right - bounds.left) : 0,
            present ? std::max<LONG>(0, bounds.bottom - bounds.top) : 0,
            SWP_NOZORDER | SWP_NOACTIVATE);
    };

    // Creation order and IDs are taken from FUN_00429AAA.  The close button
    // is supplied by the skinned-window base and uses the show/hide command.
    create(kEqHitClose, kCmdShowEqualizer, layout.close,
           kEqualizerButtonClass);
    create(kEqHitEnabled, kEqCommandEnable, layout.enabled,
           kEqualizerButtonClass);
    create(kEqHitProfile, kEqControlProfile, layout.profile,
           kEqualizerButtonClass);
    create(kEqHitReset, kEqControlReset, layout.reset,
           kEqualizerButtonClass);
    create(kEqSliderBalance, kEqSliderBalance, layout.balance,
           kEqualizerSliderClass);
    create(kEqSliderSurround, kEqSliderSurround, layout.surround,
           kEqualizerSliderClass);
    create(kEqSliderPreamp, kEqSliderPreamp, layout.preamp,
           kEqualizerSliderClass);
    for (size_t index = 0; index < layout.bands.size(); ++index) {
        const int slider = kEqSliderFirstBand + static_cast<int>(index);
        create(slider, slider, layout.bands[index], kEqualizerSliderClass);
    }
    UpdateEqualizerControlState();
}

void PlayerWindow::DestroyEqualizerControls() {
    if (equalizer_window_) RemoveToolTipTools(equalizer_window_);
    if (EqualizerOwnsCapture()) ReleaseEqualizerCapture();
    for (const auto& [hit, control] : equalizer_controls_) {
        static_cast<void>(hit);
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    equalizer_controls_.clear();
}

void PlayerWindow::UpdateEqualizerControlState() {
    const bool enabled = settings_.equalizer.profile != -2;
    for (const auto& [hit, control] : equalizer_controls_) {
        if (hit == kEqSliderPreamp ||
            (hit >= kEqSliderFirstBand && hit <= kEqSliderLastBand)) {
            // Applying a surround value must not re-disable every already
            // disabled band. EnableWindow(FALSE) sends WM_CANCELMODE; doing
            // that during a SkinSlider drag cancels the surround capture and
            // rolls its value back. The original only changes this state when
            // the EQ enabled/profile state actually changes.
            if ((IsWindowEnabled(control) != FALSE) != enabled)
                EnableWindow(control, enabled);
        }
    }
}

void PlayerWindow::ToggleEqualizerWindow() {
    if (!equalizer_window_ && !CreateEqualizerWindow()) return;
    CompleteSkinWindowFadeForReplacement();
    if (!window_ || !IsWindow(window_) || close_after_skin_window_fade_) return;
    const bool visible = IsWindowVisible(equalizer_window_) != FALSE;
    SetSkinWindowVisible(equalizer_window_, !visible);
    settings_.player.equalizer_visible = !visible;
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void PlayerWindow::UpdateEqualizerWindowSkin(bool saved_bounds) {
    if (!skin_ || !skin_->Equalizer().valid) {
        if (equalizer_window_) {
            ShowWindow(equalizer_window_, SW_HIDE);
            // The native LX-iPlay switch retains the Equalizer HWND at an
            // empty off-screen rectangle, not the preceding skin's bounds.
            SetWindowPos(equalizer_window_, nullptr, 32767, 32767, 0, 0,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (!equalizer_window_ && !CreateEqualizerWindow()) return;
    const auto& layout = skin_->Equalizer();
    ScopedSkinRedraw redraw(equalizer_window_);
    RECT player{};
    GetWindowRect(window_, &player);
    const RECT saved = settings_.player.equalizer_window;
    const bool have_saved = saved_bounds && saved.right > saved.left && saved.bottom > saved.top;
    SetWindowPos(equalizer_window_, nullptr,
        have_saved ? saved.left : player.left + layout.position.left,
        have_saved ? saved.top : player.top + layout.position.top,
        layout.background.size.cx, layout.background.size.cy,
        SWP_NOZORDER | SWP_NOACTIVATE);
    CreateEqualizerControls();
    UpdateEqualizerWindowRegion();
    UpdateEqualizerToolRects();
    redraw.Resume();
    RedrawWindow(equalizer_window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    ShowWindow(equalizer_window_, !mini_mode_ && settings_.player.equalizer_visible
        ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void PlayerWindow::UpdateEqualizerWindowRegion() {
    if (!equalizer_window_ || !skin_ || !skin_->Equalizer().valid) return;
    const auto& background = skin_->Equalizer().background;
    HRGN region = CreateSkinWindowRegion(background, {},
        background.size.cx, background.size.cy, false, skin_->TransparentColor());
    RECT bounds{};
    if (!region || GetRgnBox(region, &bounds) == NULLREGION ||
        !SetWindowRgn(equalizer_window_, region, TRUE)) {
        if (region) DeleteObject(region);
    }
}

void PlayerWindow::UpdateEqualizerToolRects() {
    if (!equalizer_window_ || !CreateToolTipWindow()) return;
    RemoveToolTipTools(equalizer_window_);
    if (!skin_ || !skin_->Equalizer().valid) return;
    for (const auto& [hit, control] : equalizer_controls_) {
        UINT_PTR identifier{};
        if (hit == kEqHitEnabled) identifier = kEqCommandEnable;
        else if (hit == kEqHitProfile) identifier = kEqControlProfile;
        else if (hit == kEqHitReset) identifier = kEqControlReset;
        else if (hit == kEqSliderBalance || hit == kEqSliderSurround ||
                 hit == kEqSliderPreamp ||
                 (hit >= kEqSliderFirstBand && hit <= kEqSliderLastBand))
            identifier = static_cast<UINT_PTR>(hit);
        // FUN_00429AAA deliberately does not register the close button.
        if (identifier) AddToolTipControl(control, identifier);
    }
}

std::wstring PlayerWindow::EqualizerToolText(UINT_PTR tool) const {
    if (tool == kEqCommandEnable)
        return ResourceCommandLabel(ResourceModule(), kEqCommandEnable);
    if (tool == kEqControlProfile) return ResourceText(0x8175);
    if (tool == kEqControlReset) return ResourceText(0x8176);
    wchar_t text[128]{};
    if (tool == kEqSliderBalance) {
        const int balance = EqualizerSliderValue(kEqSliderBalance) * 10;
        if (balance == 0) return ResourceText(0x81bd);
        const auto format = ResourceText(balance < 0 ? 0x81be : 0x81bf);
        swprintf_s(text, format.c_str(), std::abs(balance));
        return text;
    }
    if (tool == kEqSliderSurround) {
        const int surround = settings_.equalizer.surround;
        if (surround == 0) return ResourceText(0x81c0);
        const auto format = ResourceText(0x81c1);
        swprintf_s(text, format.c_str(), surround);
        return text;
    }
    if (tool == kEqSliderPreamp ||
        (tool >= kEqSliderFirstBand && tool <= kEqSliderLastBand)) {
        if (settings_.equalizer.profile == -2) return {};
        const auto format = ResourceText(0x8177);
        swprintf_s(text, format.c_str(), EqualizerSliderValue(static_cast<int>(tool)));
        return text;
    }
    return {};
}

void PlayerWindow::SetEqualizerTrackingStatus(int slider, bool tracking) {
    std::wstring text;
    if (tracking) {
        text = EqualizerToolText(static_cast<UINT_PTR>(slider));
        // FUN_00460AB1 does not replace the status when its formatted
        // temporary string is empty.
        if (text.empty()) return;
    }
    equalizer_tracking_status_ = std::move(text);
    const auto displayed = PlaybackStatusText();
    if (status_) SetWindowTextW(status_, displayed.c_str());
    if (window_) {
        if (const auto* status = FindActiveSkinElement(L"status"))
            InvalidateRect(window_, &status->bounds, FALSE);
    }
}

RECT PlayerWindow::EqualizerElementBounds(const skin::SkinElement& element) const {
    if (!skin_) return element.bounds;
    const SIZE native = skin_->Equalizer().background.size;
    return ResolveAlignedRect(element.bounds, element.alignment, native,
                              native.cx, native.cy,
                              element.name == L"title" ? element.image_size : SIZE{});
}

int PlayerWindow::EqualizerSliderValue(int slider) const {
    if (slider == kEqSliderBalance) return settings_.player.balance / 10;
    if (slider == kEqSliderSurround) return settings_.equalizer.surround;
    if (slider == kEqSliderPreamp) return settings_.equalizer.current[0];
    if (slider >= kEqSliderFirstBand && slider <= kEqSliderLastBand)
        return settings_.equalizer.current[static_cast<size_t>(slider - kEqSliderFirstBand + 1)];
    return 0;
}

HWND PlayerWindow::EqualizerControlWindow(int control) const {
    const auto item = std::find_if(equalizer_controls_.begin(),
        equalizer_controls_.end(), [control](const auto& candidate) {
            return candidate.first == control;
        });
    return item == equalizer_controls_.end() ? nullptr : item->second;
}

bool PlayerWindow::EqualizerOwnsCapture() const {
    const HWND captured = GetCapture();
    if (!captured) return false;
    if (captured == equalizer_window_) return true;
    return std::any_of(equalizer_controls_.begin(), equalizer_controls_.end(),
        [captured](const auto& control) { return control.second == captured; });
}

bool PlayerWindow::EqualizerHasCapture(int control) const {
    const HWND expected = EqualizerControlWindow(control);
    return GetCapture() == (expected ? expected : equalizer_window_);
}

void PlayerWindow::CaptureEqualizerControl(int control) {
    const HWND target = EqualizerControlWindow(control);
    SetCapture(target ? target : equalizer_window_);
}

void PlayerWindow::ReleaseEqualizerCapture() {
    if (EqualizerOwnsCapture()) ReleaseCapture();
}

RECT PlayerWindow::EqualizerSliderThumbRect(int slider) const {
    if (!skin_) return {};
    const auto& layout = skin_->Equalizer();
    const skin::SkinElement* element = nullptr;
    int minimum{};
    int maximum{};
    if (slider == kEqSliderBalance) {
        element = &layout.balance; minimum = -10; maximum = 10;
    } else if (slider == kEqSliderSurround) {
        element = &layout.surround; minimum = 0; maximum = 16;
    } else if (slider == kEqSliderPreamp) {
        element = &layout.preamp; minimum = -12; maximum = 12;
    } else if (slider >= kEqSliderFirstBand && slider <= kEqSliderLastBand) {
        element = &layout.bands[static_cast<size_t>(slider - kEqSliderFirstBand)];
        minimum = -12; maximum = 12;
    }
    if (!element || (!element->thumb_image && !element->fill_image &&
                     !element->bar_image)) return {};
    const RECT bounds = EqualizerElementBounds(*element);
    const int thumb_width = element->thumb_image
        ? std::max(1L, element->thumb_size.cx / 4) : 1;
    const int thumb_height = element->thumb_image
        ? std::max(1L, element->thumb_size.cy) : 1;
    const int value = std::clamp(EqualizerSliderValue(slider), minimum, maximum);
    if (element->vertical) {
        const int travel = std::max(0L, bounds.bottom - bounds.top - 2 - thumb_height);
        const int top = bounds.top + 1 +
            MulDiv(maximum - value, travel, maximum - minimum);
        const int left = bounds.left + (bounds.right - bounds.left - thumb_width) / 2;
        return {left, top, left + thumb_width, top + thumb_height};
    }
    const int travel = std::max(0L, bounds.right - bounds.left - 2 - thumb_width);
    const int left = bounds.left + 1 +
        MulDiv(value - minimum, travel, maximum - minimum);
    const int top = bounds.top + (bounds.bottom - bounds.top - thumb_height) / 2;
    return {left, top, left + thumb_width, top + thumb_height};
}

int PlayerWindow::EqualizerHitTest(POINT point) const {
    if (!skin_ || !skin_->Equalizer().valid) return 0;
    const auto& layout = skin_->Equalizer();
    const struct ButtonHit { int hit; const skin::SkinElement* element; } buttons[] = {
        {kEqHitClose, &layout.close}, {kEqHitEnabled, &layout.enabled},
        {kEqHitProfile, &layout.profile}, {kEqHitReset, &layout.reset}
    };
    for (const auto& button : buttons) {
        const RECT bounds = EqualizerElementBounds(*button.element);
        if (button.element->image && PtInRect(&bounds, point)) return button.hit;
    }
    const struct SliderHit { int hit; const skin::SkinElement* element; } sliders[] = {
        {kEqSliderBalance, &layout.balance}, {kEqSliderSurround, &layout.surround},
        {kEqSliderPreamp, &layout.preamp},
        {kEqSliderFirstBand + 0, &layout.bands[0]},
        {kEqSliderFirstBand + 1, &layout.bands[1]},
        {kEqSliderFirstBand + 2, &layout.bands[2]},
        {kEqSliderFirstBand + 3, &layout.bands[3]},
        {kEqSliderFirstBand + 4, &layout.bands[4]},
        {kEqSliderFirstBand + 5, &layout.bands[5]},
        {kEqSliderFirstBand + 6, &layout.bands[6]},
        {kEqSliderFirstBand + 7, &layout.bands[7]},
        {kEqSliderFirstBand + 8, &layout.bands[8]},
        {kEqSliderFirstBand + 9, &layout.bands[9]}
    };
    for (const auto& slider : sliders) {
        if (!slider.element->thumb_image && !slider.element->fill_image &&
            !slider.element->bar_image) continue;
        if (settings_.equalizer.profile == -2 && slider.hit >= kEqSliderPreamp)
            continue;
        const RECT bounds = EqualizerElementBounds(*slider.element);
        if (PtInRect(&bounds, point)) return slider.hit;
    }
    return 0;
}

void PlayerWindow::PaintEqualizer(HDC dc) const {
    if (!skin_ || !skin_->Equalizer().valid) return;
    const auto& layout = skin_->Equalizer();
    const SIZE size = layout.background.size;
    const HDC canvas = CreateCompatibleDC(dc);
    const HBITMAP buffer = CreateCompatibleBitmap(dc, size.cx, size.cy);
    const HGDIOBJ old_buffer = SelectObject(canvas, buffer);
    const RECT background_bounds{0, 0, size.cx, size.cy};
    FillRect(canvas, &background_bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    layout.background.image.Draw(canvas, 0, 0, size.cx, size.cy, 0, 0, size.cx, size.cy);

    const auto button_state = [this](int hit, bool checked = false) {
        if (equalizer_pressed_ == hit && equalizer_hover_ == hit &&
            EqualizerHasCapture(hit)) return 2;
        if (checked) return 2;
        return equalizer_hover_ == hit ? 1 : 0;
    };
    DrawElementFrame(canvas, layout.title, EqualizerElementBounds(layout.title),
                     0, skin_->TransparentColor());
    DrawAnimatedSkinFrame(canvas, layout.close, EqualizerElementBounds(layout.close),
                     button_state(kEqHitClose), equalizer_window_);
    DrawAnimatedSkinFrame(canvas, layout.enabled, EqualizerElementBounds(layout.enabled),
                     button_state(kEqHitEnabled, settings_.equalizer.profile != -2),
                     equalizer_window_);
    DrawAnimatedSkinFrame(canvas, layout.profile, EqualizerElementBounds(layout.profile),
                     button_state(kEqHitProfile), equalizer_window_);
    DrawAnimatedSkinFrame(canvas, layout.reset, EqualizerElementBounds(layout.reset),
                     button_state(kEqHitReset), equalizer_window_);

    const auto draw_slider = [&](int slider, const skin::SkinElement& element) {
        const RECT bounds = EqualizerElementBounds(element);
        const bool disabled = settings_.equalizer.profile == -2 &&
                              slider >= kEqSliderPreamp;
        if (element.bar_image) {
            // FUN_00451E07 copies the bar as the complete slider client
            // background with SRCCOPY; it is not a color-keyed centered
            // ornament.
            element.bar_image.Draw(canvas, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top,
                0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top);
        }
        const RECT thumb = EqualizerSliderThumbRect(slider);
        if (element.fill_image) {
            const int fill_x = bounds.left +
                (bounds.right - bounds.left - element.fill_size.cx) / 2;
            const int fill_y = bounds.top +
                (bounds.bottom - bounds.top - element.fill_size.cy) / 2;
            if (element.vertical) {
                // 00451E07 clips the native-sized fill at the thumb centre;
                // the lower part is copied without stretching.
                const int centre = (thumb.top + thumb.bottom) / 2;
                const int source_y = std::clamp(centre - fill_y, 0,
                    static_cast<int>(element.fill_size.cy));
                const int height = element.fill_size.cy - source_y;
                if (height > 0) element.fill_image.Draw(canvas, fill_x,
                    fill_y + source_y, element.fill_size.cx, height,
                    0, source_y, element.fill_size.cx, height,
                    skin_->TransparentColor());
            } else {
                // The horizontal fill is likewise kept at bitmap height and
                // cropped from its left edge to the thumb centre.
                const int centre = (thumb.left + thumb.right) / 2;
                const int width = std::clamp(centre - fill_x, 0,
                    static_cast<int>(element.fill_size.cx));
                if (width > 0) element.fill_image.Draw(canvas, fill_x, fill_y,
                    width, element.fill_size.cy, 0, 0, width,
                    element.fill_size.cy, skin_->TransparentColor());
            }
        }
        if (element.thumb_image && thumb.right > thumb.left && thumb.bottom > thumb.top) {
            const int frame_width = std::max(1L, element.thumb_size.cx / 4);
            int frame = disabled ? 3 : equalizer_active_slider_ == slider ? 2 :
                        equalizer_hover_ == slider ? 1 : 0;
            element.thumb_image.Draw(canvas, thumb.left, thumb.top,
                thumb.right - thumb.left, thumb.bottom - thumb.top,
                frame * frame_width, 0, frame_width,
                element.thumb_size.cy, skin_->TransparentColor());
        }
    };
    draw_slider(kEqSliderBalance, layout.balance);
    draw_slider(kEqSliderSurround, layout.surround);
    draw_slider(kEqSliderPreamp, layout.preamp);
    for (size_t index = 0; index < layout.bands.size(); ++index)
        draw_slider(kEqSliderFirstBand + static_cast<int>(index), layout.bands[index]);
    BitBlt(dc, 0, 0, size.cx, size.cy, canvas, 0, 0, SRCCOPY);
    SelectObject(canvas, old_buffer);
    DeleteObject(buffer);
    DeleteDC(canvas);
}

void PlayerWindow::SetEqualizerSliderValue(int slider, int value,
                                            bool user_change) {
    if (slider == kEqSliderBalance) {
        value = std::clamp(value, -10, 10);
        settings_.player.balance = value * 10;
        audio_.SetBalance(settings_.player.balance);
    } else if (slider == kEqSliderSurround) {
        settings_.equalizer.surround = std::clamp(value, 0, 16);
        ApplyEqualizer();
    } else if (slider == kEqSliderPreamp ||
               (slider >= kEqSliderFirstBand && slider <= kEqSliderLastBand)) {
        if (settings_.equalizer.profile == -2) return;
        const size_t index = slider == kEqSliderPreamp ? 0U :
            static_cast<size_t>(slider - kEqSliderFirstBand + 1);
        settings_.equalizer.current[index] = std::clamp(value, -12, 12);
        if (user_change) {
            if (settings_.equalizer.profile != -1)
                settings_.equalizer.profile_last = settings_.equalizer.profile;
            settings_.equalizer.profile = -1;
            settings_.equalizer.custom = settings_.equalizer.current;
        }
        ApplyEqualizer();
    }
    if (equalizer_window_) InvalidateRect(equalizer_window_, nullptr, FALSE);
}

void PlayerWindow::SetEqualizerSliderFromPoint(int slider, POINT point,
                                                bool tracking) {
    if (!skin_) return;
    const auto& layout = skin_->Equalizer();
    const skin::SkinElement* element = nullptr;
    int minimum{};
    int maximum{};
    if (slider == kEqSliderBalance) {
        element = &layout.balance; minimum = -10; maximum = 10;
    } else if (slider == kEqSliderSurround) {
        element = &layout.surround; minimum = 0; maximum = 16;
    } else if (slider == kEqSliderPreamp) {
        element = &layout.preamp; minimum = -12; maximum = 12;
    } else if (slider >= kEqSliderFirstBand && slider <= kEqSliderLastBand) {
        element = &layout.bands[static_cast<size_t>(slider - kEqSliderFirstBand)];
        minimum = -12; maximum = 12;
    }
    if (!element || (!element->thumb_image && !element->fill_image &&
                     !element->bar_image)) return;
    const RECT bounds = EqualizerElementBounds(*element);
    const int thumb_width = element->thumb_image
        ? std::max(1L, element->thumb_size.cx / 4) : 1;
    const int thumb_height = element->thumb_image
        ? std::max(1L, element->thumb_size.cy) : 1;
    // FUN_00452326 first converts the pointer/press offset to the thumb
    // centre, then FUN_00452045 maps that centre through its own client
    // interval.  That input interval is deliberately not the same as the
    // paint travel in FUN_00428F41: vertical input does not apply the
    // one-pixel inset, and the half-thumb arithmetic keeps the odd-pixel
    // rounding.  Reusing the paint travel here makes a default-band drag land
    // at +10 where the 5.7.9 binary lands at +9.
    int value{};
    if (element->vertical) {
        const int half = thumb_height / 2;
        const int first = bounds.top + half;
        const int last = bounds.bottom + half - thumb_height;
        const int interval = last - first < 2 ? 1 : last - first;
        const int centre = point.y + half - equalizer_slider_drag_offset_;
        value = maximum - MulDiv(centre - first,
                                 maximum - minimum, interval);
    } else {
        const int half = thumb_width / 2;
        const int first = bounds.left + 1 + half;
        const int last = bounds.right - 1 + half - thumb_width;
        const int interval = last - first < 2 ? 1 : last - first;
        const int centre = point.x + half - equalizer_slider_drag_offset_;
        value = minimum + MulDiv(centre - first,
                                 maximum - minimum, interval);
    }
    const int previous = EqualizerSliderValue(slider);
    SetEqualizerSliderValue(slider, value, true);
    if (tracking) {
        // FUN_00452326 sends SB_THUMBTRACK only when mapping changed value.
        if (EqualizerSliderValue(slider) != previous)
            SetEqualizerTrackingStatus(slider, true);
    } else {
        // A track click is SB_THUMBPOSITION, so the main window keeps its
        // stored playback status instead of showing a temporary value.
        SetEqualizerTrackingStatus(slider, false);
    }
}

void PlayerWindow::ApplyEqualizer() {
    UpdateEqualizerControlState();
    audio_.SetEqualizer(settings_.equalizer.profile,
                        settings_.equalizer.surround,
                        settings_.equalizer.current);
}

void PlayerWindow::ShowEqualizerProfileMenu(POINT screen_point) {
    if (!equalizer_window_ || context_menu_open_) return;
    // FUN_00429D20/FUN_00429EF4 use menu resource 0x90. Its top-level popup
    // contains Recommended, Custom, the category submenu, Enable EQ and
    // Dolby Surround. Only the preset placeholder is replaced at runtime.
    HMENU menu = DetachFirstPopup(
        LoadMenuW(ResourceModule(), MAKEINTRESOURCEW(kMenuEqualizer)));
    if (!menu) return;
    if (HMENU profiles = GetSubMenu(menu, 2)) {
        DeleteMenu(profiles, 0, MF_BYPOSITION); // resource "<>" placeholder
        for (size_t index = 1; index < std::size(kEqualizerPresets); ++index) {
            UINT flags = MF_BYPOSITION | MF_STRING;
            if (settings_.equalizer.profile == static_cast<int>(index))
                flags |= MF_CHECKED;
            auto name = ResourceListItem(ResourceModule(), 0x8174, index);
            if (name.empty()) name = kEqualizerPresets[index].name;
            InsertMenuW(profiles, static_cast<UINT>(index - 1), flags,
                kEqCommandPresetFirst + static_cast<UINT>(index),
                name.c_str());
        }
    }
    CheckMenuItem(menu, kEqCommandFlat, MF_BYCOMMAND |
        (settings_.equalizer.profile == 0 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, kEqCommandCustom, MF_BYCOMMAND |
        (settings_.equalizer.profile == -1 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, kEqCommandEnable, MF_BYCOMMAND |
        (settings_.equalizer.profile != -2 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, kEqCommandSurround, MF_BYCOMMAND |
        (settings_.equalizer.surround != 0 ? MF_CHECKED : MF_UNCHECKED));
    context_menu_open_ = true;
    SetForegroundWindow(equalizer_window_);
    BeginPopupMenuStyle(menu, true);
    const UINT command = TrackPopupMenuEx(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        screen_point.x, screen_point.y, equalizer_window_, nullptr);
    EndPopupMenuStyle();
    DestroyMenu(menu);
    context_menu_open_ = false;
    if (command) HandleEqualizerCommand(command);
    PostMessageW(equalizer_window_, WM_NULL, 0, 0);
}

bool PlayerWindow::HandleEqualizerCommand(UINT command) {
    if (command >= kEqCommandPresetFirst &&
        command < kEqCommandPresetFirst + std::size(kEqualizerPresets)) {
        const size_t profile = command - kEqCommandPresetFirst;
        std::copy(kEqualizerPresets[profile].bands.begin(),
                  kEqualizerPresets[profile].bands.end(),
                  settings_.equalizer.current.begin() + 1);
        settings_.equalizer.profile = static_cast<int>(profile);
        settings_.equalizer.profile_last = static_cast<int>(profile);
        ApplyEqualizer();
    } else if (command == kEqCommandFlat) {
        return HandleEqualizerCommand(kEqCommandPresetFirst);
    } else if (command == kEqCommandCustom) {
        settings_.equalizer.current = settings_.equalizer.custom;
        settings_.equalizer.profile = -1;
        settings_.equalizer.profile_last = -1;
        ApplyEqualizer();
    } else if (command == kEqCommandEnable) {
        if (settings_.equalizer.profile == -2) {
            settings_.equalizer.profile = settings_.equalizer.profile_last;
            if (settings_.equalizer.profile == -2) settings_.equalizer.profile = -1;
        } else {
            settings_.equalizer.profile_last = settings_.equalizer.profile;
            settings_.equalizer.profile = -2;
        }
        ApplyEqualizer();
    } else if (command == kEqCommandSurround) {
        // FUN_0042A08C toggles the effect between its off value and the
        // original midpoint/default amount 8.
        settings_.equalizer.surround =
            settings_.equalizer.surround != 0 ? 0 : 8;
        ApplyEqualizer();
    } else if (command == kEqCommandLoad || command == kEqCommandSave) {
        const wchar_t filter[] =
            // Literal at VA 0053B49C; the misspelling is present in 5.7.9.
            L"Equlizer Profile (*.tteq_cfg)\0*.tteq_cfg\0\0";
        const auto filters = ParseLegacyDialogFilter(filter);
        std::optional<std::filesystem::path> file;
        if (command == kEqCommandLoad) {
            ModernOpenFileOptions dialog;
            dialog.owner = equalizer_window_;
            dialog.filters = filters;
            dialog.default_extension = L"tteq_cfg";
            file = ModernOpenFile(dialog);
        } else {
            ModernSaveFileOptions dialog;
            dialog.owner = equalizer_window_;
            dialog.filters = filters;
            dialog.initial_path = L"Equalizer.tteq_cfg";
            dialog.default_extension = L"tteq_cfg";
            file = ModernSaveFile(dialog);
        }
        if (!file) return true;
        if (command == kEqCommandLoad) {
            std::array<int, 11> values{};
            if (!ReadEqualizerProfileFile(*file, values)) return true;
            settings_.equalizer.custom = values;
            settings_.equalizer.current = values;
            settings_.equalizer.profile = -1;
            settings_.equalizer.profile_last = -1;
            ApplyEqualizer();
        } else {
            static_cast<void>(WriteEqualizerProfileFile(
                *file, settings_.equalizer.current));
        }
    } else {
        return false;
    }
    if (equalizer_window_) InvalidateRect(equalizer_window_, nullptr, FALSE);
    return true;
}

void PlayerWindow::InvokeEqualizerControl(int control, POINT screen_point) {
    if (control == kEqHitClose) ToggleEqualizerWindow();
    else if (control == kEqHitEnabled) HandleEqualizerCommand(kEqCommandEnable);
    else if (control == kEqHitProfile) {
        // FUN_00429EF4 anchors the button popup at the profile HWND's
        // screen-space left/bottom corner.  Only WM_CONTEXTMENU uses the raw
        // cursor point (FUN_00429D20).
        if (skin_) {
            RECT bounds = EqualizerElementBounds(skin_->Equalizer().profile);
            POINT anchor{bounds.left, bounds.bottom};
            ClientToScreen(equalizer_window_, &anchor);
            screen_point = anchor;
        }
        ShowEqualizerProfileMenu(screen_point);
    }
    else if (control == kEqHitReset) {
        settings_.equalizer.current.fill(0);
        settings_.equalizer.custom = settings_.equalizer.current;
        if (settings_.equalizer.profile != -2) {
            settings_.equalizer.profile_last = settings_.equalizer.profile;
            settings_.equalizer.profile = -1;
        }
        ApplyEqualizer();
        InvalidateRect(equalizer_window_, nullptr, FALSE);
    }
}

LRESULT CALLBACK PlayerWindow::EqualizerWindowProc(HWND window, UINT message,
                                                    WPARAM wparam, LPARAM lparam) {
    PlayerWindow* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<PlayerWindow*>(create->lpCreateParams);
        self->equalizer_window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleEqualizerMessage(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK PlayerWindow::EqualizerControlProc(HWND window, UINT message,
                                                     WPARAM wparam, LPARAM lparam) {
    PlayerWindow* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<PlayerWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    LRESULT result{};
    if (!self) {
        result = DefWindowProcW(window, message, wparam, lparam);
    } else if (window == self->lyric_control_ ||
               GetParent(window) == self->lyric_window_) {
        result = self->HandleLyricControlMessage(window, message,
                                                  wparam, lparam);
    } else if (GetParent(window) == self->playlist_window_) {
        result = self->HandlePlaylistControlMessage(window, message,
                                                     wparam, lparam);
    } else {
        result = self->HandleEqualizerControlMessage(window, message,
                                                     wparam, lparam);
    }
    if (message == WM_NCDESTROY)
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return result;
}

} // namespace ttplayer::ui
