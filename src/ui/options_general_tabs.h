#pragma once

#include <algorithm>
#include <array>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>
#include <commctrl.h>
#include "update_controls.h"

namespace ttplayer::ui::detail {

// Keep the original controls and their parent HWND: switching tabs changes
// visibility, while the existing notification and settings handlers stay intact.
class GeneralOptionsTabs {
    struct Control { HWND window; RECT bounds; int height, group; bool visible; };
    HWND page_{}, tab_{};
    std::vector<Control> controls_;
    int selected_{};
    bool laying_out_{};
    static constexpr UINT_PTR kSubclass = 0x54544754;
    static constexpr int kTabId = 0xe915;

    void Layout() {
        if (laying_out_) return;
        laying_out_ = true;
        // Match InitializeOptionsSkinTabs: the tab frame fills the page,
        // including its top/left origin, without a second outer gutter.
        RECT client{}, inset{0, 0, 4, 4};
        GetClientRect(page_, &client);
        MapDialogRect(page_, &inset);
        SetWindowPos(tab_, HWND_BOTTOM, client.left, client.top,
                     client.right - client.left, client.bottom - client.top,
                     SWP_NOACTIVATE);
        RECT content{};
        GetClientRect(tab_, &content);
        TabCtrl_AdjustRect(tab_, FALSE, &content);
        MapWindowPoints(tab_, page_, reinterpret_cast<POINT*>(&content), 2);
        constexpr std::array<int, 3> first_row{18, 143, 189};
        for (const auto& control : controls_) {
            RECT origin{0, first_row[control.group], 0, 0};
            MapDialogRect(page_, &origin);
            const auto& rect = control.bounds;
            const int right = std::min<int>(rect.right, content.right - inset.right);
            SetWindowPos(control.window, nullptr, rect.left,
                         rect.top + content.top + inset.bottom - origin.top,
                         std::max<int>(1, right - rect.left), control.height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
        }
        RedrawWindow(page_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        laying_out_ = false;
    }

    void Select(int group) {
        if (group < 0 || group >= 3) return;
        selected_ = group;
        TabCtrl_SetCurSel(tab_, group);
        const HWND focus = GetFocus();
        for (const auto& control : controls_) {
            const bool show = control.group == group && control.visible;
            if (!show && (focus == control.window || IsChild(control.window, focus)))
                SetFocus(tab_);
            ShowWindow(control.window, show ? SW_SHOW : SW_HIDE);
        }
        RedrawWindow(page_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    static LRESULT CALLBACK PageProc(HWND page, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
        auto* self = reinterpret_cast<GeneralOptionsTabs*>(data);
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
        auto* self = new GeneralOptionsTabs;
        self->page_ = page;
        RECT groups{0, 130, 0, 179};
        MapDialogRect(page, &groups);
        std::array<std::wstring, 3> captions;
        for (HWND control = GetWindow(page, GW_CHILD); control;
             control = GetWindow(control, GW_HWNDNEXT)) {
            RECT rect{};
            GetWindowRect(control, &rect);
            MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&rect), 2);
            const int id = GetDlgCtrlID(control);
            // New Discord/language controls belong to Options, regardless of
            // their Y position. Resource controls retain their original groups.
            const int group = id >= kUpdateSourceLabel && id <= kUpdateStatus ? 2 :
                id >= 0xe900 ? 0 : rect.top >= groups.bottom ? 2 :
                rect.top >= groups.top ? 1 : 0;
            const auto style = GetWindowLongPtrW(control, GWL_STYLE);
            wchar_t klass[32]{};
            GetClassNameW(control, klass, static_cast<int>(std::size(klass)));
            if (_wcsicmp(klass, WC_BUTTONW) == 0 && (style & BS_TYPEMASK) == BS_GROUPBOX) {
                std::wstring caption(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
                caption.resize(GetWindowTextW(control, caption.data(), static_cast<int>(caption.size())));
                // Reuse the already translated resource captions. The command
                // line group's parenthetical explanation is too long for a tab.
                if (group == 1) {
                    const auto explanation = caption.find_first_of(L"(（");
                    if (explanation != caption.npos) caption.resize(explanation);
                    while (!caption.empty() && caption.back() == L' ') caption.pop_back();
                }
                captions[group] = std::move(caption);
                ShowWindow(control, SW_HIDE);
                continue;
            }
            int height = rect.bottom - rect.top;
            RECT dropdown{};
            if (_wcsicmp(klass, WC_COMBOBOXW) == 0 &&
                SendMessageW(control, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&dropdown)))
                height = dropdown.bottom - dropdown.top;
            self->controls_.push_back({control, rect, height, group, (style & WS_VISIBLE) != 0});
        }
        self->tab_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE |
            WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, page,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabId)),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(page, GWLP_HINSTANCE)), nullptr);
        if (!self->tab_ || !SetWindowSubclass(page, PageProc, kSubclass, reinterpret_cast<DWORD_PTR>(self))) {
            if (self->tab_) DestroyWindow(self->tab_);
            delete self;
            return;
        }
        SendMessageW(self->tab_, WM_SETFONT, SendMessageW(page, WM_GETFONT, 0, 0), FALSE);
        for (int group = 0; group < 3; ++group) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = captions[group].data();
            TabCtrl_InsertItem(self->tab_, group, &item);
        }
        self->Layout();
        self->Select(0);
    }

    static void ShowControl(HWND page, HWND control) {
        DWORD_PTR data{};
        if (!GetWindowSubclass(page, PageProc, kSubclass, &data)) return;
        auto* self = reinterpret_cast<GeneralOptionsTabs*>(data);
        for (const auto& entry : self->controls_)
            if (entry.window == control || IsChild(entry.window, control)) {
                if (entry.group != self->selected_) self->Select(entry.group);
                return;
            }
    }
};

} // namespace ttplayer::ui::detail
