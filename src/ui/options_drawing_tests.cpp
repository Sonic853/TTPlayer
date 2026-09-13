#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"

#include <iostream>
#include <stdexcept>

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
    struct Events {
        unsigned select{}, activate{}, deactivate{}, position{}, destroy{};
    };
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
        Events sheet_events;
        Require(SetWindowSubclass(sheet, Observe, 1,
            reinterpret_cast<DWORD_PTR>(&sheet_events)), "cannot observe options sheet");
        bool repeated_clean = true;
        try {
            for (int page : {14, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}) {
                ClickNavigation(player, page);
                const HWND current = PropSheet_GetCurrentPageHwnd(sheet);
                Require(current && current == player.options_pages_[page], "navigation selected wrong page");
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
        FreeLibrary(resources);
        OleUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "options drawing test: " << error.what() << '\n';
        return 1;
    }
}
