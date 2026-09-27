#pragma once

#include "../app/resource_ids.h"
#include "ttplayer/i18n/i18n.h"
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <windows.h>
#include <commctrl.h>

namespace ttplayer::ui::detail {

// Preserve the resource controls, their IDs and their parent notifications.
// Only the two effect groups become pages; FPS/type/text/profile stay common.
class VisualOptionsTabs {
    struct Control { HWND window; RECT bounds; int height, group; };
    HWND page_{}, tab_{};
    std::vector<Control> controls_;
    static constexpr UINT_PTR kSubclass = 0x54545654;

    void Layout() {
        RECT bounds{7, 68, 273, 169}, client{}, margin{0, 0, 7, 4};
        MapDialogRect(page_, &bounds);
        MapDialogRect(page_, &margin);
        GetClientRect(page_, &client);
        bounds.right = client.right - margin.right;
        SetWindowPos(tab_, HWND_BOTTOM, bounds.left, bounds.top,
                     bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE);
        RECT content{};
        GetClientRect(tab_, &content);
        TabCtrl_AdjustRect(tab_, FALSE, &content);
        MapWindowPoints(tab_, page_, reinterpret_cast<POINT*>(&content), 2);
        for (const auto& control : controls_) {
            RECT origin{13, control.group == 1 ? 130 : 80, 0, 0};
            MapDialogRect(page_, &origin);
            const auto& r = control.bounds;
            const int x = r.left + content.left + margin.bottom - origin.left;
            SetWindowPos(control.window, nullptr, x,
                r.top + content.top + margin.bottom - origin.top,
                std::max(1L, std::min(r.right - r.left, content.right - margin.bottom - x)),
                control.height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
        }
        RedrawWindow(page_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void Select(int group) {
        if (group < 0 || group > 3) return;
        TabCtrl_SetCurSel(tab_, group);
        const HWND focus = GetFocus();
        for (const auto& control : controls_) {
            if (group != control.group && (focus == control.window || IsChild(control.window, focus)))
                SetFocus(tab_);
            ShowWindow(control.window, group == control.group ? SW_SHOW : SW_HIDE);
        }
        RedrawWindow(page_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    static LRESULT CALLBACK PageProc(HWND page, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
        auto* self = reinterpret_cast<VisualOptionsTabs*>(data);
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
        auto* self = new VisualOptionsTabs;
        self->page_ = page;
        const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(page, GWLP_HINSTANCE));
        const auto font = SendMessageW(page, WM_GETFONT, 0, 0);
        self->tab_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE |
            WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, page,
            reinterpret_cast<HMENU>(IDC_VISUAL_SETTINGS_TAB), instance, nullptr);
        if (!self->tab_ || !SetWindowSubclass(page, PageProc, kSubclass, reinterpret_cast<DWORD_PTR>(self))) {
            if (self->tab_) DestroyWindow(self->tab_);
            delete self;
            return;
        }
        SendMessageW(self->tab_, WM_SETFONT, font, FALSE);
        std::array<std::wstring, 4> captions;
        RECT groups{0, 68, 0, 118}, bottom{0, 169, 0, 0};
        MapDialogRect(page, &groups);
        MapDialogRect(page, &bottom);
        for (HWND control = GetWindow(page, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT)) {
            if (control == self->tab_) continue;
            RECT r{};
            GetWindowRect(control, &r);
            MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&r), 2);
            if (r.top < groups.top || r.top >= bottom.top) continue;
            const int group = r.top >= groups.bottom ? 1 : 0;
            wchar_t klass[32]{};
            GetClassNameW(control, klass, 32);
            if (_wcsicmp(klass, WC_BUTTONW) == 0 &&
                (GetWindowLongPtrW(control, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX) {
                std::wstring text(GetWindowTextLengthW(control) + 1, L'\0');
                text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
                captions[group] = std::move(text);
                ShowWindow(control, SW_HIDE);
                continue;
            }
            RECT dropdown{};
            const int height = _wcsicmp(klass, WC_COMBOBOXW) == 0 &&
                SendMessageW(control, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&dropdown))
                ? dropdown.bottom - dropdown.top : r.bottom - r.top;
            self->controls_.push_back({control, r, height, group});
        }
        for (int group : {2, 3}) {
            captions[group] = i18n::ResourceText(GetModuleHandleW(nullptr),
                group == 2 ? IDS_VISUAL_PULSE : IDS_VISUAL_RIPPLE);
            for (int field : {0, 1, 2}) {
                const bool follow = field == 0;
                RECT r = follow ? RECT{13, 80, 260, 94} : field == 1
                    ? RECT{13, 102, 88, 116} : RECT{96, 102, 171, 116};
                MapDialogRect(page, &r);
                const int id = group == 2
                    ? (follow ? IDC_VISUAL_PULSE_FOLLOW : field == 1 ? IDC_VISUAL_PULSE_COLOR : IDC_VISUAL_PULSE_BACKGROUND)
                    : (follow ? IDC_VISUAL_RIPPLE_FOLLOW : field == 1 ? IDC_VISUAL_RIPPLE_COLOR : IDC_VISUAL_RIPPLE_BACKGROUND);
                const auto caption = i18n::ResourceText(GetModuleHandleW(nullptr),
                    follow ? IDS_VISUAL_FOLLOW_LYRIC : field == 1 ? IDS_VISUAL_BASE_COLOR : IDS_VISUAL_BACKGROUND);
                const HWND control = CreateWindowExW(0, WC_BUTTONW, caption.c_str(),
                    WS_CHILD | WS_TABSTOP | (follow ? BS_AUTOCHECKBOX : BS_PUSHBUTTON),
                    r.left, r.top, r.right - r.left, r.bottom - r.top, page,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
                if (control) {
                    SendMessageW(control, WM_SETFONT, font, FALSE);
                    self->controls_.push_back({control, r, r.bottom - r.top, group});
                }
            }
        }
        for (int group = 0; group < 4; ++group) {
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = captions[group].data();
            TabCtrl_InsertItem(self->tab_, group, &item);
        }
        self->Layout();
        self->Select(0);
    }

    static void SelectForType(HWND page, int type) {
        DWORD_PTR data{};
        if (GetWindowSubclass(page, PageProc, kSubclass, &data))
            reinterpret_cast<VisualOptionsTabs*>(data)->Select(type == 3 ? 1 : type == 5 ? 2 : type == 6 ? 3 : 0);
    }
};
} // namespace ttplayer::ui::detail
