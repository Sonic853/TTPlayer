#include <ttpres/resource_ids.h>
#include "ttplayer/i18n/i18n.h"
#include "ttplayer/ui/wtl_menu.h"
#include "ttplayer/ui/wtl_window.h"
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
namespace {
std::vector<skin::SkinLayer> EqualizerLayers(const skin::EqualizerSkin& layout) {
    std::vector<skin::SkinLayer> layers;
    const auto add = [&](const skin::SkinElement& e, int id) {
        const auto size = layout.background.size;
        const RECT bounds = ResolveAlignedRect(e.bounds, e.alignment, size, size.cx, size.cy);
        if (!IsRectEmpty(&bounds)) layers.push_back({&e, bounds, id});
    };
    add(layout.enabled, kEqHitEnabled);
    add(layout.profile, kEqHitProfile);
    add(layout.reset, kEqHitReset);
    add(layout.balance, kEqSliderBalance);
    add(layout.surround, kEqSliderSurround);
    add(layout.preamp, kEqSliderPreamp);
    for (size_t i = 0; i < layout.bands.size(); ++i)
        add(layout.bands[i], kEqSliderFirstBand + static_cast<int>(i));
    add(layout.close, kEqHitClose); // 00429AAA calls its base last.
    return layers;
}
}


LRESULT PlayerWindow::HandleEqualizerControlMessage(HWND control, UINT message,
                                                     WPARAM wparam,
                                                     LPARAM lparam) {
    if(DeferSkinRebindPaint(control,message)) return message==WM_ERASEBKGND;
    const HWND parent = GetParent(control);
    const auto item = std::find_if(equalizer_controls_.begin(), equalizer_controls_.end(),
        [control](const auto& value) { return value.second == control; });
    const int hit = item == equalizer_controls_.end() ? 0 : item->first;
    if (hit == kEqHitEnabled || hit == kEqHitProfile || hit == kEqHitReset || hit == kEqHitClose) {
        if (message == WM_NCDESTROY) native_skin_buttons_.erase(control);
        else {
            const auto previous = native_skin_buttons_[control];
            auto& button = NativeSkinButton(control,
                hit == kEqHitEnabled && settings_.equalizer.profile != -2);
            bool invoke{};
            if (skin::HandleSkinButtonInput(control, button, message, wparam, lparam, invoke)) {
                if (previous != button) InvalidateRect(control, nullptr, TRUE);
                if (invoke) {
                    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    ClientToScreen(control, &point);
                    InvokeEqualizerControl(hit, point);
                }
                return 0;
            }
        }
    }
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
        const HDC dc = BeginPaint(control, &paint);
        if (hit == kEqHitEnabled || hit == kEqHitProfile || hit == kEqHitReset || hit == kEqHitClose) {
            const int saved = SaveDC(dc);
            POINT origin{};
            MapWindowPoints(control, parent, &origin, 1);
            SetViewportOrgEx(dc, -origin.x, -origin.y, nullptr);
            PaintEqualizer(dc);
            if (saved) RestoreDC(dc, saved);
        }
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
        return forward_point(message);
    case WM_LBUTTONDBLCLK:
        return forward_point(WM_LBUTTONDOWN);
    case WM_MOUSELEAVE:
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
    case WM_KEYDOWN:
        return SendMessageW(parent, message, wparam, lparam);
    case WM_CONTEXTMENU:
        // WM_CONTEXTMENU already carries screen coordinates.
        return SendMessageW(parent, message, wparam, lparam);
    case WM_SETCURSOR:
        // Both SkinButton and SkinSlider store a hand cursor in the
        // original constructors (0040A156 / 00451CEF).
        if (reinterpret_cast<HWND>(wparam) == control &&
            LOWORD(lparam) == HTCLIENT && IsWindowEnabled(control)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        return DefWindowProcW(control, message, wparam, lparam);
    case WM_ENABLE:
        InvalidateRect(parent, nullptr, FALSE);
        return 0;
    default:
        return DefWindowProcW(control, message, wparam, lparam);
    }
}

LRESULT PlayerWindow::HandleEqualizerMessage(UINT message, WPARAM wparam,
                                              LPARAM lparam) {
    if(DeferSkinRebindPaint(equalizer_window_,message)) return message==WM_ERASEBKGND;
    if (HandleEqualizerSliderMessage(message, wparam, lparam)) return 0;
    const auto is_slider = [](int hit) {
        return hit == kEqSliderBalance || hit == kEqSliderSurround ||
               hit == kEqSliderPreamp ||
               (hit >= kEqSliderFirstBand && hit <= kEqSliderLastBand);
    };
    switch (message) {
    case WM_ACTIVATE:
        RaiseSkinOwnerOnActivation(equalizer_window_, wparam, lparam);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SETCURSOR: {
        if (reinterpret_cast<HWND>(wparam) != equalizer_window_)
            return FALSE;
        if (LOWORD(lparam) != HTCLIENT) break;
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
        int hit = EqualizerHitTest(point);
        if (is_slider(hit)) {
            const RECT thumb = EqualizerSliderThumbRect(hit);
            if (!PtInRect(&thumb, point)) hit = 0;
        }
        // After capture is transferred from a SkinButton to the equalizer
        // parent, native child input no longer reaches that button. Do not
        // manufacture a new child hover/capture from the parent's MOVE.
        if (!equalizer_pressed_ && GetCapture() == equalizer_window_ &&
            hit != 0 && !is_slider(hit) && EqualizerControlWindow(hit))
            return 0;
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
    case WM_CAPTURECHANGED:
        for (auto& [slider, state] : equalizer_slider_states_) state.Cancel();
        if (skin_drag_window_ == equalizer_window_) EndSkinMouseCapture();
        SetEqualizerTrackingStatus(0, false);
        equalizer_active_slider_ = 0;
        equalizer_track_capture_slider_ = 0;
        equalizer_pressed_ = 0;
        InvalidateRect(equalizer_window_, nullptr, FALSE);
        return 0;
    case WM_CANCELMODE:
        for (auto& [slider, state] : equalizer_slider_states_) state.Cancel();
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
    case WM_INITMENUPOPUP:
        // Use the player's shared dynamic submenu dispatcher, just as the
        // original popup notification route through CPlayerWnd does.
        return SendMessageW(window_, message, wparam, lparam);
    case WM_MENUCHAR:
        if (const auto result = PopupMenuChar(wparam, lparam)) return *result;
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

bool PlayerWindow::CreateEqualizerWindow(bool for_provider) {
    if (equalizer_window_) return true;
    if (!skin_ || (!skin_->Equalizer().valid && !for_provider && !external_skin_)) return false;
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
    const int width = layout.valid ? layout.background.size.cx : 1;
    const int height = layout.valid ? layout.background.size.cy : 1;
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
        const RECT bounds = EqualizerElementBounds(element);
        const bool present = !IsRectEmpty(&bounds);
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
        if (!slider) ApplySkinButtonRegion(control, element, skin_->TransparentColor());
    };

    // Creation order and IDs are taken from FUN_00429AAA.  The close button
    // is supplied by the skinned-window base and uses the show/hide command.
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
    create(kEqHitClose, kCmdShowEqualizer, layout.close,
           kEqualizerButtonClass);
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
    if (external_skin_ || !skin_ || !skin_->Equalizer().valid) return;
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
    SetSliderStatus(std::move(text));
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

std::optional<skin::SkinSliderGeometry> PlayerWindow::EqualizerSliderGeometry(int slider) const {
    if (!skin_) return std::nullopt;
    const auto& layout = skin_->Equalizer();
    const skin::SkinElement* element{};
    int minimum{}, maximum{};
    if (slider == kEqSliderBalance) {
        element = &layout.balance; minimum = -10; maximum = 10;
    } else if (slider == kEqSliderSurround) {
        element = &layout.surround; maximum = 16;
    } else if (slider == kEqSliderPreamp) {
        element = &layout.preamp; minimum = -12; maximum = 12;
    } else if (slider >= kEqSliderFirstBand && slider <= kEqSliderLastBand) {
        element = &layout.bands[static_cast<size_t>(slider - kEqSliderFirstBand)];
        minimum = -12; maximum = 12;
    }
    if (!element) return std::nullopt;
    return skin::SkinSliderGeometry{EqualizerElementBounds(*element), minimum, maximum,
        element->thumb_image ? static_cast<int>(element->vertical ? element->thumb_size.cy : element->thumb_size.cx / 4) : 1,
        element->vertical};
}

RECT PlayerWindow::EqualizerSliderThumbRect(int slider) const {
    const auto geometry = EqualizerSliderGeometry(slider);
    return geometry ? geometry->Thumb(EqualizerSliderValue(slider)) : RECT{};
}

bool PlayerWindow::HandleEqualizerSliderMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message != WM_MOUSEMOVE && message != WM_LBUTTONDOWN && message != WM_LBUTTONUP &&
        message != WM_KEYDOWN) return false;
    if (dragging_skin_background_ || equalizer_pressed_) return false;
    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    int slider = equalizer_active_slider_ ? equalizer_active_slider_ : equalizer_track_capture_slider_;
    if (message == WM_KEYDOWN) slider = equalizer_focused_slider_;
    else if (!slider) {
        if (message == WM_LBUTTONUP) return false;
        slider = EqualizerHitTest(point);
    }
    const auto geometry = EqualizerSliderGeometry(slider);
    if (!geometry || IsRectEmpty(&geometry->bounds)) return false;
    const HWND control = EqualizerControlWindow(slider);
    if (control && !IsWindowEnabled(control)) return false;
    auto state = equalizer_slider_states_[slider];
    state.Sync(*geometry, EqualizerSliderValue(slider));
    const bool had_capture = (equalizer_active_slider_ == slider ||
        equalizer_track_capture_slider_ == slider) && EqualizerHasCapture(slider);
    skin::SkinSliderEvent event;
    if (message == WM_LBUTTONDOWN) {
        equalizer_focused_slider_ = slider;
        SetFocus(control ? control : equalizer_window_);
        event = state.Down(*geometry, point);
    } else if (message == WM_MOUSEMOVE) event = state.Move(*geometry, point);
    else if (message == WM_LBUTTONUP) event = state.Up(*geometry, point);
    else {
        event = state.Key(*geometry, static_cast<UINT>(wparam));
        // Consume a handled key even if clamping leaves the value unchanged.
        if (event.notification < 0 && wparam != VK_ESCAPE && wparam != VK_LEFT &&
            wparam != VK_RIGHT && wparam != VK_UP && wparam != VK_DOWN) return false;
    }
    if (state.captured && !EqualizerHasCapture(slider)) CaptureEqualizerControl(slider);
    else if (!state.captured && had_capture) ReleaseEqualizerCapture();
    // Capture can synchronously cancel the old child; commit this child's state after it.
    equalizer_slider_states_[slider] = state;
    equalizer_active_slider_ = state.dragging ? slider : 0;
    equalizer_track_capture_slider_ = state.captured && !state.dragging ? slider : 0;
    equalizer_hover_ = state.captured ? slider : 0;
    equalizer_slider_initial_value_ = state.initial;
    if (event.notification >= 0) {
        SetEqualizerSliderValue(slider, state.value, true);
        SetEqualizerTrackingStatus(slider, event.notification != SB_THUMBPOSITION);
    }
    if (event.redraw) InvalidateRect(equalizer_window_, nullptr, FALSE);
    return true;
}

int PlayerWindow::EqualizerHitTest(POINT point) const {
    if (!skin_ || !skin_->Equalizer().valid) return 0;
    const skin::SkinLayers layers(EqualizerLayers(skin_->Equalizer()), skin_->TransparentColor());
    if (const auto* hit = layers.Hit(point)) {
        // User32 routes input over a disabled child to its parent. The
        // parent's fallback hit test must not capture/invoke that child
        // again, or a click clears its disabled frame and may toggle it.
        const HWND control = EqualizerControlWindow(hit->id);
        return control && !IsWindowEnabled(control) ? 0 : hit->id;
    }
    return 0;
}

void PlayerWindow::PaintEqualizer(HDC dc) const {
    if (!skin_ || !skin_->Equalizer().valid) return;
    const auto& layout = skin_->Equalizer();
    const SIZE size = layout.background.size;
    const skin::SkinLayers layers(EqualizerLayers(layout), skin_->TransparentColor());
    const HDC canvas = CreateCompatibleDC(dc);
    const HBITMAP buffer = CreateCompatibleBitmap(dc, size.cx, size.cy);
    const HGDIOBJ old_buffer = SelectObject(canvas, buffer);
    const RECT background_bounds{0, 0, size.cx, size.cy};
    FillRect(canvas, &background_bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    layout.background.image.Draw(canvas, 0, 0, size.cx, size.cy, 0, 0, size.cx, size.cy);

    const auto button_state = [this](int hit, bool checked = false) {
        if (const HWND control = EqualizerControlWindow(hit);
            control && native_skin_buttons_.contains(control))
            return NativeSkinButton(control, checked).Frame();
        if (equalizer_pressed_ == hit && equalizer_hover_ == hit &&
            EqualizerHasCapture(hit)) return 2;
        if (checked) return 2;
        return equalizer_hover_ == hit ? 1 : 0;
    };
    DrawElementFrame(canvas, layout.title, EqualizerElementBounds(layout.title),
                     0, skin_->TransparentColor());
    {
        const skin::SkinLayerClip clip(canvas, layers, &layout.close);
        if (!skin::IsPngSkinButton(layout.close))
            DrawAnimatedSkinFrame(canvas, layout.close, EqualizerElementBounds(layout.close),
                button_state(kEqHitClose), equalizer_window_);
    }
    {
        const skin::SkinLayerClip clip(canvas, layers, &layout.enabled);
        if (!skin::IsPngSkinButton(layout.enabled))
            DrawAnimatedSkinFrame(canvas, layout.enabled, EqualizerElementBounds(layout.enabled),
                button_state(kEqHitEnabled, settings_.equalizer.profile != -2), equalizer_window_);
    }
    {
        const skin::SkinLayerClip clip(canvas, layers, &layout.profile);
        if (!skin::IsPngSkinButton(layout.profile))
            DrawAnimatedSkinFrame(canvas, layout.profile, EqualizerElementBounds(layout.profile),
                button_state(kEqHitProfile), equalizer_window_);
    }
    {
        const skin::SkinLayerClip clip(canvas, layers, &layout.reset);
        if (!skin::IsPngSkinButton(layout.reset))
            DrawAnimatedSkinFrame(canvas, layout.reset, EqualizerElementBounds(layout.reset),
                button_state(kEqHitReset), equalizer_window_);
    }

    const auto draw_slider = [&](int slider, const skin::SkinElement& element) {
        const skin::SkinLayerClip clip(canvas, layers, &element);
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
        if (element.thumb_image && !IsRectEmpty(&bounds)) {
            const int frame_width = element.thumb_size.cx / 4;
            const int paint_width = !frame_width && !element.thumb_image.IsGdiPlus()
                ? element.thumb_size.cx : frame_width;
            int frame = disabled ? 3 : equalizer_active_slider_ == slider ? 2 :
                        equalizer_hover_ == slider ? 1 : 0;
            element.thumb_image.Draw(canvas,
                thumb.left + (thumb.right - thumb.left - frame_width) / 2,
                thumb.top + (thumb.bottom - thumb.top - element.thumb_size.cy) / 2,
                paint_width, element.thumb_size.cy,
                frame * frame_width, 0, paint_width,
                element.thumb_size.cy, skin_->TransparentColor());
        }
    };
    draw_slider(kEqSliderBalance, layout.balance);
    draw_slider(kEqSliderSurround, layout.surround);
    draw_slider(kEqSliderPreamp, layout.preamp);
    for (size_t index = 0; index < layout.bands.size(); ++index)
        draw_slider(kEqSliderFirstBand + static_cast<int>(index), layout.bands[index]);
    layers.PaintPngButtons(canvas, [&](const skin::SkinElement& element) {
        const int hit = &element == &layout.enabled ? kEqHitEnabled :
            &element == &layout.profile ? kEqHitProfile :
            &element == &layout.reset ? kEqHitReset : kEqHitClose;
        DrawAnimatedSkinFrame(canvas, element, EqualizerElementBounds(element),
            button_state(hit, hit == kEqHitEnabled && settings_.equalizer.profile != -2),
            equalizer_window_);
    });
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
        audio_->SetBalance(settings_.player.balance);
    } else if (slider == kEqSliderSurround) {
        settings_.equalizer.surround = std::clamp(value, 0, 16);
    } else if (slider == kEqSliderPreamp ||
               (slider >= kEqSliderFirstBand && slider <= kEqSliderLastBand)) {
        if (settings_.equalizer.profile == -2) return;
        const size_t index = slider == kEqSliderPreamp ? 0U :
            static_cast<size_t>(slider - kEqSliderFirstBand + 1);
        settings_.equalizer.current[index] = std::clamp(value, -12, 12);
    } else {
        return;
    }
    if (user_change) UpdateEqualizerUserProfile();
    ApplyEqualizer();
    if (equalizer_window_) InvalidateRect(equalizer_window_, nullptr, FALSE);
}

void PlayerWindow::UpdateEqualizerUserProfile() {
    // 00429DDB applies this common state transition to balance/surround
    // notifications too. The first notification from a preset selects
    // Custom; only a subsequent notification copies Current to Custom.
    if (settings_.equalizer.profile == -1) {
        settings_.equalizer.custom = settings_.equalizer.current;
    } else if (settings_.equalizer.profile != -2) {
        settings_.equalizer.profile_last = settings_.equalizer.profile;
        settings_.equalizer.profile = -1;
    }
}

void PlayerWindow::SetEqualizerSliderFromPoint(int slider, POINT point, bool tracking) {
    const auto geometry = EqualizerSliderGeometry(slider);
    if (!geometry) return;
    if (tracking) {
        if (geometry->vertical) point.y += geometry->extent / 2 - equalizer_slider_drag_offset_;
        else point.x += geometry->extent / 2 - equalizer_slider_drag_offset_;
    }
    const int value = geometry->ValueAt(point);
    if (tracking && value == EqualizerSliderValue(slider)) return;
    SetEqualizerSliderValue(slider, value, true);
    SetEqualizerTrackingStatus(slider, tracking);
}

void PlayerWindow::ApplyEqualizer() {
    UpdateEqualizerControlState();
    audio_->SetEqualizer(settings_.equalizer.profile,
                        settings_.equalizer.surround,
                        settings_.equalizer.current);
}

void PlayerWindow::PopulateEqualizerPresetMenu(HMENU menu) const {
    if (!menu) return;
    // FUN_004299CD keeps the final Load/Save pair, then recreates the
    // separator and twelve localized categories. This also handles reopening
    // an already-populated popup without duplicate rows or stale checks.
    for (int position = GetMenuItemCount(menu) - 3; position >= 0; --position)
        DeleteMenu(menu, position, MF_BYPOSITION);
    InsertMenuW(menu, 0, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    for (size_t index = 1; index < std::size(kEqualizerPresets); ++index) {
        UINT flags = MF_BYPOSITION | MF_STRING;
        if (settings_.equalizer.profile == static_cast<int>(index))
            flags |= MF_CHECKED | MF_USECHECKBITMAPS;
        auto name = ResourceListItem(ResourceModule(), 0x8174, index);
        if (name.empty()) name = kEqualizerPresets[index].name;
        InsertMenuW(menu, static_cast<UINT>(index - 1), flags,
            kEqCommandPresetFirst + static_cast<UINT>(index), name.c_str());
    }
}

void PlayerWindow::PrepareEqualizerMenu(HMENU menu) const {
    // Expose the skin's Reset button through both copies of the EQ popup.
    // Reuse its localized resource label and keep repeated preparation safe.
    if (const HMENU commands = FindCommandMenu(menu, kEqCommandEnable);
        commands && !FindCommandMenu(commands, kEqCommandReset)) {
        auto label = ResourceText(0x8176);
        if (label.empty()) label = i18n::ResourceText(GetModuleHandleW(nullptr), IDS_EQUALIZER_RESET);
        InsertMenuW(commands, kEqCommandEnable, MF_BYCOMMAND | MF_STRING,
                    kEqCommandReset, label.c_str());
    }
    HMENU profiles = FindCommandMenu(menu, kEqCommandPresetFirst);
    if (!profiles) profiles = FindCommandMenu(menu, kEqCommandPresetFirst + 1);
    if (profiles) PopulateEqualizerPresetMenu(profiles);
    // CPlayerWnd's common UI update (0045Axxx) owns these states for both
    // menu 0x90 and the copy grafted into the main menu by 0045DFBA.
    CheckCommand(menu, kEqCommandFlat, settings_.equalizer.profile == 0);
    CheckCommand(menu, kEqCommandCustom, settings_.equalizer.profile == -1);
    CheckCommand(menu, kEqCommandEnable, settings_.equalizer.profile != -2);
    CheckCommand(menu, kEqCommandSurround, settings_.equalizer.surround != 0);
}

void PlayerWindow::ShowEqualizerProfileMenu(POINT screen_point) {
    if (!equalizer_window_ || context_menu_open_) return;
    HMENU menu = DetachFirstPopup(
        i18n::LoadMenu(ResourceModule(), MAKEINTRESOURCEW(kMenuEqualizer)));
    if (!menu) return;
    PrepareEqualizerMenu(menu);
    context_menu_open_ = true;
    SetForegroundWindow(equalizer_window_);
    BeginPopupMenuStyle(menu, true);
    const UINT command = TrackPlayerPopupMenuEx(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD,
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
        ApplyEqualizer();
    } else if (command == kEqCommandFlat) {
        return HandleEqualizerCommand(kEqCommandPresetFirst);
    } else if (command == kEqCommandCustom) {
        settings_.equalizer.current = settings_.equalizer.custom;
        settings_.equalizer.profile = -1;
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
    } else if (command == kEqCommandReset) {
        settings_.equalizer.current.fill(0);
        // 0042A0DC clears Current only; selecting Custom can still recall the
        // saved curve until a subsequent custom-slider notification saves it.
        if (settings_.equalizer.profile != -2) {
            settings_.equalizer.profile_last = settings_.equalizer.profile;
            settings_.equalizer.profile = -1;
        }
        ApplyEqualizer();
    } else if (command == kEqCommandLoad || command == kEqCommandSave) {
        // Keep the original spelling as fallback; only translate the label.
        const std::vector<ModernDialogFilter> filters{{
            i18n::Text(L"Equlizer Profile (*.tteq_cfg)"), L"*.tteq_cfg"}};
        std::optional<std::filesystem::path> file;
        if (command == kEqCommandLoad) {
            ModernOpenFileOptions dialog;
            dialog.owner = IsWindowVisible(equalizer_window_) ? equalizer_window_ : window_;
            dialog.filters = filters;
            dialog.default_extension = L"tteq_cfg";
            file = ModernOpenFile(dialog);
        } else {
            ModernSaveFileOptions dialog;
            dialog.owner = IsWindowVisible(equalizer_window_) ? equalizer_window_ : window_;
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
        HandleEqualizerCommand(kEqCommandReset);
    }
}

LRESULT CALLBACK PlayerWindow::EqualizerWindowProc(HWND window, UINT message,
        WPARAM wparam, LPARAM lparam) {
    return WindowBinding<PlayerWindow>::Start(window, message, wparam, lparam,
        &PlayerWindow::equalizer_window_,
        [](PlayerWindow* self, HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
            (void)window;
            return self->HandleEqualizerMessage(message, wp, lp);
        });
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
