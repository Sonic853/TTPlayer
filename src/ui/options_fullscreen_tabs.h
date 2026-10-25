#pragma once

#include "../app/resource_ids.h"
#include "ttplayer/i18n/i18n.h"
#include <algorithm>
#include <array>
#include <vector>
#include <windows.h>
#include <commctrl.h>

namespace ttplayer::ui::detail {
// Resource 263 remains byte-identical to 5.7.9. Move its controls as groups,
// retaining their HWNDs, dialog parent and existing settings notifications.
class FullscreenOptionsTabs {
    struct Control { HWND window; RECT bounds; int height, group; bool visible; };
    HWND page_{}, tab_{};
    std::vector<Control> controls_;
    bool laying_out_{};
    static constexpr UINT_PTR kSubclass = 0x54544654;

    void Layout() {
        if (laying_out_) return;
        laying_out_ = true;
        RECT client{}, inset{0, 0, 4, 4};
        GetClientRect(page_, &client);
        MapDialogRect(page_, &inset);
        SetWindowPos(tab_, HWND_BOTTOM, 0, 0, client.right, client.bottom, SWP_NOACTIVATE);
        RECT content{};
        GetClientRect(tab_, &content);
        TabCtrl_AdjustRect(tab_, FALSE, &content);
        MapWindowPoints(tab_, page_, reinterpret_cast<POINT*>(&content), 2);
        constexpr std::array<int, 3> origins{162, 7, 63};
        for (const auto& control : controls_) {
            RECT origin{7, origins[control.group], 0, 0};
            MapDialogRect(page_, &origin);
            const auto& r = control.bounds;
            const int x = content.left + inset.right + r.left - origin.left;
            SetWindowPos(control.window, nullptr, x,
                content.top + inset.bottom + r.top - origin.top,
                std::max(1L, std::min(r.right - r.left, content.right - inset.right - x)),
                control.height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
        }
        RedrawWindow(page_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        laying_out_ = false;
    }

    void Select(int group) {
        if (group < 0 || group >= 3) return;
        TabCtrl_SetCurSel(tab_, group);
        const HWND focus = GetFocus();
        for (const auto& control : controls_) {
            const bool show = group == control.group && control.visible;
            if (!show && (focus == control.window || IsChild(control.window, focus)))
                SetFocus(tab_);
            ShowWindow(control.window, show ? SW_SHOW : SW_HIDE);
        }
        RedrawWindow(page_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    static LRESULT CALLBACK PageProc(HWND page, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
        auto* self = reinterpret_cast<FullscreenOptionsTabs*>(data);
        if (message == WM_NOTIFY) {
            const auto* header = reinterpret_cast<const NMHDR*>(lparam);
            if (header && header->hwndFrom == self->tab_ && header->code == TCN_SELCHANGE) {
                self->Select(TabCtrl_GetCurSel(self->tab_));
                return 0;
            }
        } else if (message == WM_SIZE) {
            const auto result = DefSubclassProc(page, message, wparam, lparam);
            self->Layout();
            return result;
        } else if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(page, PageProc, id);
            delete self;
        }
        return DefSubclassProc(page, message, wparam, lparam);
    }

public:
    static void Attach(HWND page) {
        DWORD_PTR existing{};
        if (GetWindowSubclass(page, PageProc, kSubclass, &existing)) return;
        auto* self = new FullscreenOptionsTabs;
        self->page_ = page;
        self->tab_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE |
            WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, page,
            reinterpret_cast<HMENU>(IDC_FULLSCREEN_SETTINGS_TAB),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(page, GWLP_HINSTANCE)), nullptr);
        if (!self->tab_ || !SetWindowSubclass(page, PageProc, kSubclass, reinterpret_cast<DWORD_PTR>(self))) {
            if (self->tab_) DestroyWindow(self->tab_);
            delete self;
            return;
        }
        SendMessageW(self->tab_, WM_SETFONT, SendMessageW(page, WM_GETFONT, 0, 0), FALSE);
        RECT boundaries{0, 63, 0, 162};
        MapDialogRect(page, &boundaries);
        std::vector<HWND> group_boxes;
        for (HWND control = GetWindow(page, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT)) {
            if (control == self->tab_) continue;
            RECT r{};
            GetWindowRect(control, &r);
            MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&r), 2);
            const int group = r.top >= boundaries.bottom ? 0 : r.top >= boundaries.top ? 2 : 1;
            wchar_t klass[32]{};
            GetClassNameW(control, klass, 32);
            const auto style = GetWindowLongPtrW(control, GWL_STYLE);
            if (_wcsicmp(klass, WC_BUTTONW) == 0 && (style & BS_TYPEMASK) == BS_GROUPBOX)
                group_boxes.push_back(control);
            RECT dropdown{};
            const int height = _wcsicmp(klass, WC_COMBOBOXW) == 0 &&
                SendMessageW(control, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&dropdown))
                ? dropdown.bottom - dropdown.top : r.bottom - r.top;
            self->controls_.push_back({control, r, height, group,
                (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0});
        }
        // Dynamic group boxes may precede their controls in the Z order.
        // Keep them behind controls, with the tab frame behind both, so a
        // switch/repaint cannot cover an edit or intercept its mouse input.
        for (const HWND group : group_boxes) {
            SetWindowLongPtrW(group, GWL_STYLE, GetWindowLongPtrW(group, GWL_STYLE) | WS_CLIPSIBLINGS);
            SetWindowPos(group, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        int index = 0;
        for (const UINT id : {IDS_FULLSCREEN_VISUAL_TAB, IDS_FULLSCREEN_COMBINED_TAB, IDS_FULLSCREEN_LYRIC_TAB}) {
            auto caption = i18n::ResourceText(GetModuleHandleW(nullptr), id);
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = caption.data();
            TabCtrl_InsertItem(self->tab_, index++, &item);
        }
        self->Layout();
        self->Select(0);
    }
};
} // namespace ttplayer::ui::detail
