#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "../app/resource_ids.h"

#include <iostream>
#include <stdexcept>
#include <uxtheme.h>
#include <vsstyle.h>

namespace fs = std::filesystem;

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
    static void Pump() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    struct Surface {
        HDC dc{CreateCompatibleDC(nullptr)};
        HBITMAP bitmap{};
        HGDIOBJ previous{};
        unsigned* pixels{};
        int width{}, height{};
        Surface(int w, int h) : width(w), height(h) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                reinterpret_cast<void**>(&pixels), nullptr, 0);
            Require(dc && bitmap, "cannot allocate button capture");
            previous = SelectObject(dc, bitmap);
            std::fill_n(pixels, w*h, 0x00123456U);
        }
        ~Surface() { SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc); }
    };
    static void CheckImageButton(HWND button) {
        BUTTON_IMAGELIST layout{};
        Require(button && SendMessageW(button, BCM_GETIMAGELIST, 0,
            reinterpret_cast<LPARAM>(&layout)), "missing image-only button");
        int width{}, height{};
        Require(ImageList_GetIconSize(layout.himl, &width, &height) && width == 16 && height == 15,
            "original 16x15 bitmap was stretched");
        Require(layout.uAlign == BUTTON_IMAGELIST_ALIGN_CENTER && IsRectEmpty(&layout.margin),
            "image-only button uses text/image left padding");
        RECT bounds{}; GetClientRect(button, &bounds);
        const bool enabled = IsWindowEnabled(button) != FALSE;
        for (const int visual_state : {PBS_NORMAL, PBS_PRESSED, PBS_DISABLED}) {
            const bool pressed = visual_state == PBS_PRESSED;
            const bool disabled = visual_state == PBS_DISABLED;
            EnableWindow(button, !disabled);
            SendMessageW(button, BM_SETSTATE, pressed, 0);
            Surface reference(width, height);
            const RECT image_rect{0, 0, width, height};
            const HTHEME theme = GetWindowTheme(button);
            if (!disabled) ImageList_Draw(layout.himl, 0, reference.dc, 0, 0, ILD_NORMAL);
            else if (!theme || FAILED(DrawThemeIcon(theme, reference.dc, BP_PUSHBUTTON,
                PBS_DISABLED, &image_rect, layout.himl, 0))) {
                const HICON icon = ImageList_GetIcon(layout.himl, 0, ILD_NORMAL);
                Require(icon != nullptr, "cannot get disabled reference icon");
                DrawStateW(reference.dc, nullptr, nullptr, reinterpret_cast<LPARAM>(icon), 0,
                    0, 0, width, height, DST_ICON | DSS_DISABLED);
                DestroyIcon(icon);
            }
            GdiFlush();
            Surface capture(bounds.right, bounds.bottom);
            SendMessageW(button, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(capture.dc), PRF_CLIENT);
            GdiFlush();
            const int x = (bounds.right-width+1)/2 + pressed;
            const int y = (bounds.bottom-height+1)/2 + pressed;
            const auto matches = [&](int left, int top) {
                int count = 0;
                for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col) {
                    const unsigned pixel = reference.pixels[row*width+col] & 0xffffff;
                    if (pixel == 0x123456) continue;
                    if (left+col >= 0 && left+col < capture.width && top+row >= 0 && top+row < capture.height &&
                        (capture.pixels[(top+row)*capture.width+left+col] & 0xffffff) == pixel) ++count;
                }
                return count;
            };
            int opaque = 0;
            for (int i = 0; i < width*height; ++i)
                if ((reference.pixels[i] & 0xffffff) != 0x123456) ++opaque;
            const int exact = matches(x,y);
            if (exact != opaque) {
                int best = 0, best_x = 0, best_y = 0;
                for (int py = 0; py <= bounds.bottom-height; ++py)
                    for (int px = 0; px <= bounds.right-width; ++px)
                        if (const int score = matches(px,py); score > best) { best=score; best_x=px; best_y=py; }
                std::cerr << "button=" << GetDlgCtrlID(button) << " size=" << bounds.right << 'x' << bounds.bottom
                    << " state=" << visual_state << " expected=" << x << ',' << y << " matches=" << exact << '/' << opaque
                    << " best=" << best_x << ',' << best_y << " matches=" << best << '\n';
            }
            SendMessageW(button, BM_SETSTATE, FALSE, 0);
            Require(opaque > 0 && exact == opaque, "button pixels do not follow 0046E0C9 placement");
        }
        EnableWindow(button, enabled);
    }
    static void CheckImageButtonVariants(HWND button) {
        RECT original{}; GetClientRect(button, &original);
        CheckImageButton(button);
        // Different resource/font/DPI mappings produce both odd and even
        // client extents. Exercise layout on native HWND resize, not a model.
        for (const bool classic : {false, true}) {
            SetWindowTheme(button, classic ? L"" : nullptr, classic ? L"" : nullptr);
            for (const SIZE size : {SIZE{22,20}, SIZE{23,21}, SIZE{30,27}, SIZE{31,28}}) {
                SetWindowPos(button, nullptr, 0, 0, size.cx, size.cy,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                CheckImageButton(button);
            }
        }
        SetWindowTheme(button, nullptr, nullptr);
        SetWindowPos(button, nullptr, 0, 0, original.right, original.bottom,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    struct Events {
        unsigned select{}, activate{}, deactivate{}, position{}, destroy{};
    };
    static void CheckAssociationImages(HWND page, HMODULE resources) {
        for (const auto [control, resource] : {std::pair{2031, 0x162}, std::pair{2032, 0x163}}) {
            BUTTON_IMAGELIST layout{};
            Require(SendDlgItemMessageW(page, control, BCM_GETIMAGELIST, 0,
                reinterpret_cast<LPARAM>(&layout)), "missing association button image");
            const auto bitmap = static_cast<HBITMAP>(LoadImageW(resources,
                MAKEINTRESOURCEW(resource), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
            BITMAP source{};
            Require(bitmap && GetObjectW(bitmap, sizeof(source), &source), "missing original bitmap");
            const auto reference = ImageList_Create(source.bmWidth, source.bmHeight,
                ILC_COLOR32 | ILC_MASK, 1, 0);
            Require(reference && ImageList_AddMasked(reference, bitmap, RGB(192,192,192)) == 0,
                "cannot prepare original 00434F8C bitmap");
            DeleteObject(bitmap);
            int width{}, height{};
            Require(ImageList_GetIconSize(layout.himl, &width, &height) &&
                width == source.bmWidth && height == source.bmHeight &&
                ImageList_GetImageCount(layout.himl) == 1, "button bitmap was resized or split");
            Surface expected(width, height), actual(width, height);
            ImageList_Draw(reference, 0, expected.dc, 0, 0, ILD_NORMAL);
            ImageList_Draw(layout.himl, 0, actual.dc, 0, 0, ILD_NORMAL);
            GdiFlush();
            int mismatches = 0;
            for (int pixel=0; pixel<width*height; ++pixel)
                mismatches += (expected.pixels[pixel] & 0xffffff) != (actual.pixels[pixel] & 0xffffff);
            ImageList_Destroy(reference);
            std::cout << "association button " << control << " original bitmap RGB mismatches=" << mismatches << '\n';
            Require(mismatches == 0, "association button bitmap palette/transparency changed");
        }
        for (const int control : {2030,2031,2032,2106}) {
            const HWND button = GetDlgItem(page, control);
            BUTTON_IMAGELIST layout{};
            Require(SendMessageW(button, BCM_GETIMAGELIST, 0, reinterpret_cast<LPARAM>(&layout)),
                "association icon missing");
            int width{}, height{};
            Require(ImageList_GetIconSize(layout.himl, &width, &height), "association image size missing");
            RECT client{}; GetClientRect(button, &client);
            wchar_t caption[128]{}; GetWindowTextW(button, caption, 128);
            const bool was_enabled = IsWindowEnabled(button) != FALSE;
            for (const bool classic : {false,true}) {
                SetWindowTheme(button, classic ? L"" : nullptr, classic ? L"" : nullptr);
                for (const int state : {PBS_NORMAL,PBS_PRESSED,PBS_DISABLED}) {
                    const bool pressed = state == PBS_PRESSED, disabled = state == PBS_DISABLED;
                    EnableWindow(button, !disabled);
                    SendMessageW(button, BM_SETSTATE, pressed, 0);
                    Surface expected(width,height), alternate(width,height), actual(client.right,client.bottom);
                    std::fill_n(alternate.pixels, width*height, 0x00fdfdfdU);
                    RECT text{};
                    const auto old = SelectObject(actual.dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
                    DrawTextW(actual.dc, caption, -1, &text, DT_CALCRECT | DT_SINGLELINE);
                    SelectObject(actual.dc, old);
                    const int x = (client.right-width-text.right)/3 + pressed;
                    const int y = (client.bottom-height+1)/2 + pressed;
                    const HTHEME theme = GetWindowTheme(button);
                    const RECT icon{0,0,width,height};
                    const auto draw_reference = [&](HDC dc) {
                    if (!theme || FAILED(DrawThemeIcon(theme, dc, BP_PUSHBUTTON, state, &icon, layout.himl, 0))) {
                        if (disabled) {
                            const auto hicon = ImageList_GetIcon(layout.himl, 0, ILD_NORMAL);
                            DrawStateW(dc, nullptr, nullptr, reinterpret_cast<LPARAM>(hicon), 0,
                                0, 0, width, height, DST_ICON | DSS_DISABLED);
                            DestroyIcon(hicon);
                        } else ImageList_Draw(layout.himl, 0, dc, 0, 0, ILD_NORMAL);
                    }
                    };
                    draw_reference(expected.dc);
                    draw_reference(alternate.dc);
                    SendMessageW(button, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(actual.dc), PRF_CLIENT);
                    GdiFlush();
                    int mismatch = 0, opaque = 0;
                    for (int py=0; py<height; ++py) for (int px=0; px<width; ++px) {
                        const auto color = expected.pixels[py*width+px] & 0xffffff;
                        // HICON edges can be partially alpha blended; compare
                        // only foreground pixels independent of background.
                        if (color != (alternate.pixels[py*width+px] & 0xffffff)) continue;
                        ++opaque;
                        mismatch += color != (actual.pixels[(y+py)*actual.width+x+px] & 0xffffff);
                    }
                    if (mismatch) std::cerr << "captioned control=" << control << " classic=" << classic << " state=" << state
                        << " expected=" << x << ',' << y << " mismatch=" << mismatch << '/' << opaque << '\n';
                    Require(opaque > 0 && mismatch == 0, "association image does not follow 0046E0C9 caption layout");
                }
            }
            SendMessageW(button, BM_SETSTATE, FALSE, 0);
            EnableWindow(button, was_enabled);
            SetWindowTheme(button, nullptr, nullptr);
        }
    }
    static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM wp,
                                    LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& events = *reinterpret_cast<Events*>(data);
        if (message == PSM_SETCURSEL || message == PSM_SETCURSELID) ++events.select;
        if (message == WM_WINDOWPOSCHANGING) ++events.position;
        if (message == WM_DESTROY) ++events.destroy;
        if (message == WM_NOTIFY && lp) {
            const auto code = reinterpret_cast<NMHDR*>(lp)->code;
            if (code == PSN_SETACTIVE) ++events.activate;
            if (code == PSN_KILLACTIVE) ++events.deactivate;
        }
        return DefSubclassProc(window, message, wp, lp);
    }
    static void ClickNavigation(ui::PlayerWindow& player, int page) {
        RECT item{};
        Require(SendMessageW(player.options_navigation_, LB_GETITEMRECT, page,
            reinterpret_cast<LPARAM>(&item)) != LB_ERR, "missing navigation item");
        const auto point = MAKELPARAM((item.left + item.right) / 2,
                                     (item.top + item.bottom) / 2);
        // Real listbox input path without moving or injecting the host cursor.
        SendMessageW(player.options_navigation_, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(player.options_navigation_, WM_LBUTTONUP, 0, point);
        Pump();
    }
    static void CheckOptionsChrome(ui::PlayerWindow& player) {
        const HWND sheet = player.options_window_;
        const HWND list = player.options_navigation_;
        const HWND header = player.options_header_;
        const auto bounds_in_sheet = [&](HWND child) {
            RECT rect{}; GetWindowRect(child, &rect);
            MapWindowPoints(nullptr, sheet, reinterpret_cast<POINT*>(&rect), 2);
            return rect;
        };
        const RECT nav = bounds_in_sheet(list), title = bounds_in_sheet(header);
        const RECT page = bounds_in_sheet(PropSheet_GetCurrentPageHwnd(sheet));
        Require(nav.left == 14 && nav.top == 14 && nav.right == page.left-12 &&
            nav.bottom == page.bottom, "004A2F66 navigation/page spacing changed");
        Require(title.left == page.left && title.right == page.right &&
            title.top == nav.top && title.bottom == page.top,
            "header must contain gradient and the 8px separator strip");
        Require((GetWindowLongPtrW(list, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == 0,
            "navigation added a sunken client edge instead of the sheet's Tab pane frame");
        RECT client{}, window{}; GetClientRect(sheet, &client); GetWindowRect(sheet, &window);
        POINT origin{}; ClientToScreen(sheet, &origin);
        Require(origin.y-window.top == GetSystemMetrics(SM_CYCAPTION)+GetSystemMetrics(SM_CYDLGFRAME),
            "modern padded border inflated the fixed options caption");
        std::cout << "options client=" << client.right << 'x' << client.bottom
            << " page=" << page.left << ',' << page.top << ',' << page.right-page.left
            << ',' << page.bottom-page.top << " caption=" << origin.y-window.top << '\n';

        const int selected = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        const int other = selected == 0 ? 1 : 0;
        RECT row{}, active{};
        SendMessageW(list, LB_GETITEMRECT, other, reinterpret_cast<LPARAM>(&row));
        SendMessageW(list, LB_GETITEMRECT, selected, reinterpret_cast<LPARAM>(&active));
        Require(row.bottom-row.top == 24, "0048DB49 navigation row height changed");
        Surface capture(nav.right-nav.left, nav.bottom-nav.top);
        const auto print = [&] {
            SendMessageW(list, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(capture.dc), PRF_CLIENT);
            GdiFlush();
        };
        const auto pixel = [&](int x, int y) { return GetPixel(capture.dc, x, y); };
        constexpr COLORREF blue = RGB(48,106,198), accent = RGB(192,192,255);
        const auto mix = [](COLORREF a, COLORREF b) {
            return RGB((GetRValue(a)+GetRValue(b))/2,
                       (GetGValue(a)+GetGValue(b))/2, (GetBValue(a)+GetBValue(b))/2);
        };
        const auto white = GetSysColor(COLOR_WINDOW);
        const auto edge = mix(blue,accent);
        SendMessageW(list, WM_MOUSELEAVE, 0, 0);
        print();
        Require(pixel(2,row.top+2) == white && pixel(2,active.top+2) == blue,
            "normal/selected navigation backgrounds changed");
        Require(pixel(capture.width/2,row.bottom-1) == accent && pixel(0,row.bottom-1) == white,
            "navigation separator is not a white-accent-white gradient");
        Require(pixel(8,active.top+12) == accent && pixel(12,active.top+13) == edge,
            "0048D596 triangle foreground/shadow does not use accent/blended colours");
        Require(pixel(1,active.top) == edge && pixel(capture.width-1,active.top+1) == accent,
            "selected row bevel colours are reversed or missing");
        const auto hover = MAKELPARAM(30,row.top+12);
        SendMessageW(list, WM_MOUSEMOVE, 0, hover);
        print();
        Require(SendMessageW(list, LB_GETCURSEL, 0, 0) == selected,
            "hover unexpectedly selects another options page");
        Require(pixel(2,row.top+2) == mix(blue,white) && pixel(1,row.top) == accent &&
            pixel(capture.width-1,row.top+1) == edge, "0048D870 hover fill/frame is missing");
        SendMessageW(list, WM_MOUSELEAVE, 0, 0);
        print();
        Require(pixel(2,row.top+2) == white && pixel(capture.width/2,row.bottom-1) == accent,
            "mouse leave left stale hover pixels");
        SendMessageW(list, WM_MOUSEMOVE, 0, MAKELPARAM(30,active.top+12));
        print();
        Require(pixel(2,active.top+2) == mix(blue,white) && pixel(8,active.top+12) == accent,
            "hover on the selected row lost its arrow or kept the non-hover fill");
        SendMessageW(list, WM_MOUSELEAVE, 0, 0);
        const auto blank = MAKELPARAM(30,capture.height-1);
        SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, blank);
        SendMessageW(list, WM_LBUTTONUP, 0, blank);
        Require(SendMessageW(list, LB_GETCURSEL, 0, 0) == selected,
            "0048D8F0 blank-area click must not select the nearest navigation row");

        Surface banner(title.right-title.left, title.bottom-title.top);
        SendMessageW(header, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(banner.dc), PRF_CLIENT);
        GdiFlush();
        COLORREF strip_color = GetSysColor(COLOR_3DFACE);
        if (const auto theme = OpenThemeData(sheet,L"Tab")) {
            Surface reference(banner.width,banner.height);
            RECT pane{0,0,banner.width,page.bottom-title.top}; InflateRect(&pane,4,4);
            DrawThemeBackground(theme,reference.dc,TABP_PANE,0,&pane,nullptr);
            strip_color = GetPixel(reference.dc,0,banner.height-5);
            CloseThemeData(theme);
        }
        Require(GetPixel(banner.dc,0,0) == blue &&
            GetPixel(banner.dc,0,banner.height-5) == strip_color,
            "gradient height/separator spacing does not follow 004A3209");
        int shadow_pixels = 0;
        for (int y=4; y<banner.height-8; ++y) for (int x=4; x<banner.width; ++x)
            shadow_pixels += GetPixel(banner.dc,x,y) == RGB(32,32,32);
        Require(shadow_pixels > 0, "header lost the original 1px text shadow");
    }
    static void CheckAboutBackground(HWND page) {
        RECT bounds{}; GetClientRect(page, &bounds);
        Surface capture(bounds.right,bounds.bottom);
        // WM_PRINTCLIENT paints the same page background before its custom
        // title/edition overlay. Empty label corners must retain that colour.
        SendMessageW(page, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(capture.dc),
                     PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        GdiFlush();
        for (const int id : {1009,1020}) {
            RECT label{}; GetWindowRect(GetDlgItem(page,id), &label);
            MapWindowPoints(nullptr,page,reinterpret_cast<POINT*>(&label),2);
            Require(GetPixel(capture.dc,label.right-2,label.top+2) ==
                GetPixel(capture.dc,label.right-2,label.top-2),
                "About title/edition still fills a white rectangle over the page");
        }
    }
    struct RegistrationCheck {
        ui::PlayerWindow* player{};
        HMODULE resources{};
        int close_mode{};
        bool entered{};
        bool checked_inert_close{};
        UINT_PTR close_timer{};
        std::exception_ptr error;
    };
    inline static RegistrationCheck* registration{};
    static void CALLBACK CheckRegistration(HWND, UINT, UINT_PTR timer, DWORD) {
        auto& check = *registration;
        auto& player = *check.player;
        if (!player.options_navigation_) return;
        KillTimer(nullptr, timer);
        if (check.entered) {
            // Original /reg sends PSM_CANCELTOCLOSE: Esc and WM_CLOSE are
            // inert (confirmed against 5.7.9), then title-bar close exits.
            if (check.close_mode != 1 && check.close_mode != 3 && !check.error)
                check.error = std::make_exception_ptr(std::runtime_error("modal close did not finish"));
            check.checked_inert_close = true;
            PostMessageW(player.options_window_, WM_SYSCOMMAND, SC_CLOSE, 0);
            return;
        }
        check.entered = true;
        const HWND sheet = player.options_window_;
        SetWindowPos(sheet, nullptr, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        try {
            Require(player.options_registration_mode_ && !player.window_, "/reg constructed a player window");
            Require(SendMessageW(player.options_navigation_, LB_GETCOUNT, 0, 0) == 2,
                "/reg did not use the reduced two-page navigation");
            Require(TabCtrl_GetItemCount(PropSheet_GetTabControl(sheet)) == 2,
                "/reg created more than two native property pages");
            Require(!IsWindowVisible(PropSheet_GetTabControl(sheet)), "/reg still shows stock tabs");
            Require(IsWindowVisible(GetDlgItem(sheet, IDOK)) && !IsWindowVisible(GetDlgItem(sheet, IDCANCEL)),
                "/reg lost the common Close button or exposed Cancel");
            RECT close{}, client{};
            GetWindowRect(GetDlgItem(sheet, IDOK), &close);
            MapWindowPoints(nullptr, sheet, reinterpret_cast<POINT*>(&close), 2);
            GetClientRect(sheet, &client);
            Require(close.left >= 0 && close.right <= client.right && close.bottom <= client.bottom,
                "comctl32 overwrote registration shell geometry after initialization");
            Require(player.options_page_index_ == 14 &&
                PropSheet_GetCurrentPageHwnd(sheet) == player.options_pages_[14], "/reg did not select association");
            CheckOptionsChrome(player);
            for (int row : {0,1,1,0,0,1}) {
                ClickNavigation(player, row);
                const int logical = row == 0 ? 0 : 14;
                Require(PropSheet_GetCurrentPageHwnd(sheet) == player.options_pages_[logical] &&
                    player.options_page_index_ == logical &&
                    SendMessageW(player.options_navigation_, LB_GETCURSEL, 0, 0) == row,
                    "registration navigation confused physical and logical page indices");
            }
            CheckAssociationImages(player.options_pages_[14], check.resources);
            Require(player.options_skin_entries_.empty() && !player.options_pages_[12],
                "registration mode initialized skin options");
        } catch (...) { check.error = std::current_exception(); }
        check.close_timer = SetTimer(nullptr, 0, 200, CheckRegistration);
        if (check.error || check.close_mode == 0) PostMessageW(sheet, WM_COMMAND, IDOK, 0);
        else if (check.close_mode == 1) PostMessageW(sheet, WM_KEYDOWN, VK_ESCAPE, 0);
        else if (check.close_mode == 2) PostMessageW(sheet, WM_SYSCOMMAND, SC_CLOSE, 0);
        else PostMessageW(sheet, WM_CLOSE, 0, 0);
    }
    static void RunRegistration(HMODULE resources, const fs::path& directory) {
        fs::create_directories(directory);
        const auto file = directory / L"registration-test-only.xml";
        for (int mode=0; mode<4; ++mode) {
            settings::Settings settings;
            settings.source_path = file;
            settings.player.player_window = RECT{123,234,543,374};
            settings.player.mini_mode = true;
            settings.history.last_active_page = 8;
            settings.general.send_title_to_msn = false;
            ui::PlayerWindow player(settings);
            player.SetSkinResourceModule(resources);
            RegistrationCheck check{&player, resources, mode};
            registration = &check;
            std::cout << "checking /reg close mode " << mode << std::endl;
            const auto timer = SetTimer(nullptr, 0, 40, CheckRegistration);
            Require(timer != 0, "cannot automate modal registration sheet");
            const int result = player.ShowRegistrationOptions(GetModuleHandleW(nullptr));
            KillTimer(nullptr, timer);
            if (check.close_timer) KillTimer(nullptr, check.close_timer);
            registration = nullptr;
            if (check.error) std::rethrow_exception(check.error);
            Require((mode != 1 && mode != 3) || check.checked_inert_close,
                "registration Esc/WM_CLOSE differs from original PSM_CANCELTOCLOSE behavior");
            Require(check.entered && result != -1 && !player.options_window_ && !player.options_registration_mode_,
                "registration modal loop did not close cleanly");
            const auto saved = settings::LoadLegacyXml(file);
            Require(EqualRect(&saved.player.player_window, &settings.player.player_window) &&
                saved.player.mini_mode && saved.history.last_active_page == 8,
                "/reg overwrote stored window geometry/mode or full-sheet page history");
            // Reusing the object must return to the ordinary 15-page modeless sheet.
            player.ShowOptions(14);
            Require(SendMessageW(player.options_navigation_, LB_GETCOUNT, 0, 0) == 15,
                "registration mode leaked into normal options");
            player.CloseOptions();
        }
        fs::remove(file);
        fs::remove(directory);
        std::cout << "/reg: two-page shell, native Close/title-bar, inert Esc/WM_CLOSE, geometry persistence passed\n";
    }
    static void Run(HMODULE resources, const fs::path& directory) {
        settings::Settings settings;
        settings.source_path = directory / L"test-only.xml";
        settings.general.tray_icon = false;
        settings.general.send_title_to_msn = false;
        settings.general.fade_windows = false;
        settings.lyric.auto_download = false;
        settings.hotkey.global = false;
        ui::PlayerWindow player(settings);
        player.SetSkinResourceModule(resources);
        player.instance_ = GetModuleHandleW(nullptr);
        player.ShowOptions(14);
        const HWND sheet = player.options_window_;
        Require(sheet && IsWindow(sheet), "options sheet did not open");
        // Keep the native sheet alive but out of the user's working area.
        SetWindowPos(sheet, nullptr, -20000, -20000, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Pump();
        CheckOptionsChrome(player);
        CheckAssociationImages(player.options_pages_[14], resources);
        Events sheet_events;
        Require(SetWindowSubclass(sheet, Observe, 1,
            reinterpret_cast<DWORD_PTR>(&sheet_events)), "cannot observe options sheet");
        bool repeated_clean = true;
        try {
            for (int page : {14, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}) {
                ClickNavigation(player, page);
                const HWND current = PropSheet_GetCurrentPageHwnd(sheet);
                Require(current && current == player.options_pages_[page], "navigation selected wrong page");
                if (page == 0) {
                    CheckAboutBackground(current);
                    struct Captions { int build{}, completion{}; } captions;
                    EnumChildWindows(current, [](HWND control, LPARAM data) -> BOOL {
                        auto& found = *reinterpret_cast<Captions*>(data);
                        wchar_t text[64]{};
                        GetWindowTextW(control, text, static_cast<int>(std::size(text)));
                        found.build += std::wstring_view(text) == L"构建日期:";
                        found.completion += std::wstring_view(text) == L"完成日期:";
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&captions));
                    Require(captions.build == 1 && captions.completion == 0,
                        "About page did not replace only the completion-date caption");
                    wchar_t date[32]{};
                    Require(GetDlgItemTextW(current, 1040, date, static_cast<int>(std::size(date))) > 0,
                        "renaming the date caption removed the generated date value");
                }
                Events page_events;
                Require(SetWindowSubclass(current, Observe, 1,
                    reinterpret_cast<DWORD_PTR>(&page_events)), "cannot observe options page");
                RECT before{};
                GetWindowRect(current, &before);
                sheet_events = {};
                for (int repeat = 0; repeat < 3; ++repeat) ClickNavigation(player, page);
                RECT after{};
                GetWindowRect(current, &after);
                RemoveWindowSubclass(current, Observe, 1);
                const bool unchanged = sheet_events.select == 0 && page_events.activate == 0 &&
                    page_events.deactivate == 0 && page_events.position == 0 && page_events.destroy == 0;
                if (!unchanged) {
                    std::cerr << "page=" << page << " repeated PSM=" << sheet_events.select
                        << " active=" << page_events.activate << " inactive=" << page_events.deactivate
                        << " position=" << page_events.position << '\n';
                }
                repeated_clean = repeated_clean && unchanged;
                Require(EqualRect(&before, &after) && PropSheet_GetCurrentPageHwnd(sheet) == current,
                    "repeated navigation replaced or moved the page");
                unsigned visible_pages = 0;
                for (HWND candidate : player.options_pages_)
                    if (candidate && IsWindowVisible(candidate)) ++visible_pages;
                Require(visible_pages == 1, "page switch left overlapping visible property pages");
            }
            std::cout << "sheet WS_CLIPCHILDREN=" <<
                ((GetWindowLongPtrW(sheet, GWL_STYLE) & WS_CLIPCHILDREN) != 0) << '\n';
            Require(repeated_clean, "same-page clicks reselect/reposition the page");
            Require((GetWindowLongPtrW(sheet, GWL_STYLE) & WS_CLIPCHILDREN) != 0,
                "sheet paints over child controls (original 004A2F66 sets WS_CLIPCHILDREN)");
            player.SelectOptionsPage(8);
            for (int id : {1027, 1039, 1042, 1045}) CheckImageButtonVariants(GetDlgItem(player.options_pages_[8], id));
            player.ShowLyricServiceEditor();
            for (int id : {IDC_LYRIC_SERVICES_ADD, IDC_LYRIC_SERVICES_DELETE,
                           IDC_LYRIC_SERVICES_UP, IDC_LYRIC_SERVICES_DOWN})
                CheckImageButtonVariants(GetDlgItem(player.lyric_service_editor_, id));
            player.CloseLyricServiceEditor();
            BUTTON_IMAGELIST close_image{};
            Require(SendDlgItemMessageW(sheet, IDOK, BCM_GETIMAGELIST, 0,
                reinterpret_cast<LPARAM>(&close_image)) && close_image.uAlign == BUTTON_IMAGELIST_ALIGN_LEFT &&
                close_image.margin.left == 3 && close_image.margin.right == 3,
                "image-only fix changed the captioned Close button layout");
            // A targeted request on an already active page must still change
            // the nested lyric/network tab rather than returning too early.
            for (const auto target : {std::pair{7,384U}, {7,385U}, {7,384U},
                                       {9,381U}, {9,382U}, {9,381U}}) {
                const bool same_page = player.options_page_index_ == target.first;
                sheet_events = {};
                player.SelectOptionsPage(target.first, target.second);
                Require(!same_page || sheet_events.select == 0,
                    "targeted entry reselected an already active outer page");
                const HWND nested = target.first == 7 ? player.options_lyric_child_
                                                       : player.options_network_child_;
                Require(static_cast<UINT>(reinterpret_cast<ULONG_PTR>(GetPropW(
                    nested, L"TTPlayer.Options.Template"))) == target.second,
                    "same-page targeted entry did not switch nested options");
            }
            RemoveWindowSubclass(sheet, Observe, 1);
            player.CloseOptions();
            Require(!IsWindow(sheet), "options did not close");
            Require(!fs::exists(settings.source_path), "test wrote user settings");
        } catch (...) {
            RemoveWindowSubclass(sheet, Observe, 1);
            player.CloseOptions();
            throw;
        }
        std::cout << "all 15 pages: same-page input is inert; page HWND/geometry, child clipping,\n"
                     "nested targeted entries and close preserved\n";
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    using Access = ttplayer::testing::SkinRebindAccess;
    try {
        Access::Require(argc == 2, "expected original repository root");
        Access::Require(SUCCEEDED(OleInitialize(nullptr)), "COM initialization failed");
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES};
        Access::Require(InitCommonControlsEx(&common), "common controls unavailable");
        const HMODULE resources = LoadLibraryExW((fs::path(argv[1]) / L"ttpres.dll").c_str(),
            nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Access::Require(resources != nullptr, "original 5.7.9 resources unavailable");
        const auto directory = fs::temp_directory_path() /
            (L"TTPlayer-options-drawing-" + std::to_wstring(GetCurrentProcessId()));
        Access::Run(resources, directory);
        Access::RunRegistration(resources, directory);
        FreeLibrary(resources);
        OleUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "options drawing test: " << error.what() << '\n';
        return 1;
    }
}
