#pragma once

#include "../app/resource_ids.h"
#include "ttplayer/i18n/i18n.h"
#include "ttplayer/audio/osz_archive.h"
#include "osu_visual_availability.h"
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <windows.h>
#include <commctrl.h>

namespace ttplayer::ui::detail {
inline constexpr int kVisualOsuLogoLabel = 0xe950;
inline constexpr int kVisualOsuLogoChoice = 0xe951;
inline constexpr int kVisualOsuLogoPath = 0xe952;
inline constexpr int kVisualOsuLogoBrowse = 0xe953;
inline constexpr int kVisualLazerCustomText = 0xe955;
inline constexpr int kVisualLazerText1 = 0xe956; // three consecutive edit IDs
inline constexpr UINT kVisualTabChanged = WM_APP + 0x392;
inline constexpr UINT kVisualAvailabilityChanged = WM_APP + 0x393;

// Preserve the resource controls, their IDs and their parent notifications.
// Effect groups become pages; FPS/type/text/profile stay common.
class VisualOptionsTabs {
    struct Control { HWND window; RECT bounds; int height, group; bool visible{true}; };
    HWND page_{}, tab_{};
    std::vector<Control> controls_;
    std::array<std::wstring,6> captions_;
    static constexpr UINT_PTR kSubclass = 0x54545654;
    static bool InGroup(int control_group,int selected) noexcept {
        return control_group==selected || (control_group==4 && selected==5);
    }

    void Layout() {
        RECT bounds{7, 68, 273, 169}, client{}, margin{0, 0, 7, 4};
        i18n::MapDialogRect(page_, &bounds);
        i18n::MapDialogRect(page_, &margin);
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
            i18n::MapDialogRect(page_, &origin);
            const auto& r = control.bounds;
            const int x = r.left + content.left + margin.bottom - origin.left;
            RECT browse_offset{0,0,0,18};i18n::MapDialogRect(page_,&browse_offset);
            const int dy=GetDlgCtrlID(control.window)==kVisualOsuLogoBrowse &&
                TabCtrl_GetCurSel(tab_)==5?browse_offset.bottom:0;
            SetWindowPos(control.window, nullptr, x,
                r.top + content.top + margin.bottom - origin.top-dy,
                std::max(1L, std::min(r.right - r.left, content.right - margin.bottom - x)),
                control.height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
        }
        RedrawWindow(page_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void Select(int group) {
        if (group < 0 || group >= TabCtrl_GetItemCount(tab_)) group=0;
        TabCtrl_SetCurSel(tab_, group);
        const HWND focus = GetFocus();
        for (const auto& control : controls_) {
            if (!InGroup(control.group,group) && (focus == control.window || IsChild(control.window, focus)))
                SetFocus(tab_);
            ShowWindow(control.window, InGroup(control.group,group) && control.visible ? SW_SHOW : SW_HIDE);
        }
        SendMessageW(page_,kVisualTabChanged,group,0);
        Layout();
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
        auto& captions=self->captions_;
        RECT groups{0, 68, 0, 118}, bottom{0, 169, 0, 0};
        i18n::MapDialogRect(page, &groups);
        i18n::MapDialogRect(page, &bottom);
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
                i18n::MapDialogRect(page, &r);
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
        const bool osu_available=audio::HasOsuVisualSupport();
        if(osu_available) {
        captions[4] = i18n::ResourceText(GetModuleHandleW(nullptr), IDS_VISUAL_OSU);
        const auto add_osu_control = [&](UINT id, UINT text_id, const wchar_t* klass,
                                          DWORD style, RECT r) {
            i18n::MapDialogRect(page, &r);
            const auto caption = text_id ? i18n::ResourceText(GetModuleHandleW(nullptr), text_id)
                                        : std::wstring{};
            const bool edit=_wcsicmp(klass,WC_EDITW)==0;
            const HWND control = CreateWindowExW(edit?WS_EX_CLIENTEDGE:0, klass, caption.c_str(),
                WS_CHILD | (edit?(style&~WS_BORDER):style), r.left, r.top, r.right - r.left, r.bottom - r.top,
                page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            if (control) {
                SendMessageW(control, WM_SETFONT, font, FALSE);
                self->controls_.push_back({control, r, r.bottom - r.top, 4});
            }
            return control;
        };
        const UINT flag_labels[]{IDS_VISUAL_OSU_SPECTRUM, IDS_VISUAL_OSU_STARS,
            IDS_VISUAL_OSU_FLASHES, IDS_VISUAL_OSU_PARALLAX, IDS_VISUAL_OSU_LOGO};
        for (int field = 0; field < 5; ++field) {
            const LONG x = 13 + field * 49;
            const LONG y = 80;
            add_osu_control(IDC_VISUAL_OSU_SPECTRUM + field, flag_labels[field],
                WC_BUTTONW, WS_TABSTOP | BS_AUTOCHECKBOX, {x, y, x + 48, y + 13});
        }
        if(audio::HasOsuTransitionSupport()) {
            add_osu_control(IDC_VISUAL_OSU_INTRO,IDS_VISUAL_OSU_INTRO,
                WC_BUTTONW,WS_TABSTOP|BS_AUTOCHECKBOX,{13,95,72,108});
            add_osu_control(IDC_VISUAL_OSU_OUTRO,IDS_VISUAL_OSU_OUTRO,
                WC_BUTTONW,WS_TABSTOP|BS_AUTOCHECKBOX,{75,95,134,108});
        }
        add_osu_control(IDC_VISUAL_OSU_BPM_LABEL, IDS_VISUAL_OSU_BPM,
            WC_STATICW, SS_LEFT, {13, 113, 125, 126});
        const HWND bpm = add_osu_control(IDC_VISUAL_OSU_BPM, 0, WC_EDITW,
            WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, {127, 110, 160, 124});
        if (bpm) SendMessageW(bpm, EM_SETLIMITTEXT, 3, 0);
        add_osu_control(IDC_VISUAL_OSU_ACCENT, IDS_VISUAL_BASE_COLOR,
            WC_BUTTONW, WS_TABSTOP | BS_PUSHBUTTON, {137, 95, 196, 109});
        add_osu_control(IDC_VISUAL_OSU_BACKGROUND, IDS_VISUAL_OSU_BACKGROUND,
            WC_BUTTONW, WS_TABSTOP | BS_PUSHBUTTON, {199, 95, 258, 109});
        if(audio::HasOsuLogoSupport()) {
            add_osu_control(kVisualOsuLogoLabel,IDS_OSU_LOGO_CHOICE,
                WC_STATICW,SS_LEFT,{13,131,73,144});
            const HWND choice=add_osu_control(kVisualOsuLogoChoice,0,WC_COMBOBOXW,
                WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,{75,127,190,187});
            for(UINT text:{IDS_OSU_LOGO_TTPLAYER,IDS_OSU_LOGO_OSU,IDS_OSU_LOGO_CUSTOM}) {
                const auto label=i18n::ResourceText(GetModuleHandleW(nullptr),text);
                SendMessageW(choice,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            }
            add_osu_control(kVisualOsuLogoPath,0,WC_EDITW,
                WS_TABSTOP|ES_AUTOHSCROLL|ES_READONLY,{13,145,236,159});
            add_osu_control(kVisualOsuLogoBrowse,0,WC_BUTTONW,
                WS_TABSTOP|BS_PUSHBUTTON,{240,145,258,159});
        }
        }
        if(audio::HasOsuIntroTextSupport()) {
            const auto add_text=[&](int id, const wchar_t* klass, const wchar_t* caption, DWORD style, RECT r) {
                i18n::MapDialogRect(page,&r);
                const bool edit=_wcsicmp(klass,WC_EDITW)==0;
                HWND child=CreateWindowExW(edit?WS_EX_CLIENTEDGE:0,klass,caption,
                    WS_CHILD|WS_TABSTOP|(edit?(style&~WS_BORDER):style),
                    r.left,r.top,r.right-r.left,r.bottom-r.top,page,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
                if(child) {
                    SendMessageW(child,WM_SETFONT,font,FALSE);
                    self->controls_.push_back({child,r,r.bottom-r.top,5});
                }
                return child;
            };
            const auto caption=i18n::ResourceText(GetModuleHandleW(nullptr),IDS_OSU_CUSTOM_INTRO_TEXT);
            add_text(kVisualLazerCustomText,WC_BUTTONW,caption.c_str(),BS_AUTOCHECKBOX,{174,111,258,125});
            for(int i=0;i<3;++i) {
                HWND child=add_text(kVisualLazerText1+i,WC_EDITW,L"",WS_BORDER|ES_AUTOHSCROLL,
                    {13+i*84,145,90+i*84,159});
                if(child)SendMessageW(child,EM_SETLIMITTEXT,64,0);
            }
        }
        const bool lazer_available=audio::HasOsuLazerSupport();
        if(lazer_available)captions[5]=i18n::ResourceText(GetModuleHandleW(nullptr),IDS_VISUAL_OSU_LAZER);
        RefreshAvailability(page);
        self->Layout();
        self->Select(0);
    }

    static void RefreshAvailability(HWND page) {
        DWORD_PTR data{};
        if(!GetWindowSubclass(page,PageProc,kSubclass,&data))return;
        auto* self=reinterpret_cast<VisualOptionsTabs*>(data);
        const int selected=TabCtrl_GetCurSel(self->tab_);
        TabCtrl_DeleteAllItems(self->tab_);
        const int count=OsuLazerAvailable()?6:OsuVisualAvailable()?5:4;
        for(int group=0;group<count;++group) {
            TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=self->captions_[group].data();
            TabCtrl_InsertItem(self->tab_,group,&item);
        }
        self->Select(selected);
    }
    static void SetVisible(HWND page,int id,bool visible) {
        DWORD_PTR data{};
        if(!GetWindowSubclass(page,PageProc,kSubclass,&data))return;
        auto* self=reinterpret_cast<VisualOptionsTabs*>(data);
        for(auto& control:self->controls_)if(GetDlgCtrlID(control.window)==id) {
            control.visible=visible;
            ShowWindow(control.window,visible && InGroup(control.group,TabCtrl_GetCurSel(self->tab_))?SW_SHOW:SW_HIDE);
            return;
        }
    }
    static void SelectForType(HWND page, int type) {
        DWORD_PTR data{};
        if (GetWindowSubclass(page, PageProc, kSubclass, &data))
            reinterpret_cast<VisualOptionsTabs*>(data)->Select(type == 3 ? 1 : type == 5 ? 2 : type == 6 ? 3 : type == 7 ? 4 : type == 8 ? 5 : 0);
    }
    static bool IsLazerSelected(HWND page) {
        DWORD_PTR data{};
        return GetWindowSubclass(page,PageProc,kSubclass,&data) &&
            TabCtrl_GetCurSel(reinterpret_cast<VisualOptionsTabs*>(data)->tab_)==5;
    }
    static bool IsOsuSelected(HWND page) {
        DWORD_PTR data{};
        return GetWindowSubclass(page,PageProc,kSubclass,&data) &&
            TabCtrl_GetCurSel(reinterpret_cast<VisualOptionsTabs*>(data)->tab_)>=4;
    }
};
} // namespace ttplayer::ui::detail
