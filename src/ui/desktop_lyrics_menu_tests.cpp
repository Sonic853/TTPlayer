#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"

#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
using namespace ttplayer::ui::detail;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::wstring Resource(HMODULE module, UINT id) {
    wchar_t value[1024]{};
    return {value, static_cast<size_t>(LoadStringW(module, id, value, 1024))};
}
std::wstring Label(HMENU menu, int position) {
    wchar_t value[1024]{};
    return {value, static_cast<size_t>(GetMenuStringW(menu, position, value, 1024, MF_BYPOSITION))};
}
struct Module {
    HMODULE value{};
    ~Module() { if (value) FreeLibrary(value); }
};
struct Window {
    HWND value{};
    ~Window() { if (value && IsWindow(value)) DestroyWindow(value); }
};
struct Menu {
    HMENU value{};
    ~Menu() { if (value) DestroyMenu(value); }
};
struct ToolSearch { HWND bar{}, tooltip{}; };
BOOL CALLBACK FindTip(HWND window, LPARAM data) {
    auto& search = *reinterpret_cast<ToolSearch*>(data);
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    if (GetWindow(window, GW_OWNER) == search.bar && std::wstring_view(name) == TOOLTIPS_CLASSW) {
        search.tooltip = window; return FALSE;
    }
    return TRUE;
}
std::wstring QueryTip(HWND tooltip, HWND bar, UINT command) {
    const HWND control = GetDlgItem(bar, command);
    Require(control != nullptr, "desktop control missing");
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFO_V1_SIZE;
    tool.hwnd = bar; tool.uId = reinterpret_cast<UINT_PTR>(control);
    Require(SendMessageW(tooltip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)) != 0,
            "control is not registered with tooltip");
    Require((tool.uFlags & TTF_IDISHWND) && tool.lpszText == LPSTR_TEXTCALLBACKW,
            "tooltip is not a dynamic HWND callback");
    wchar_t text[1024]{};
    tool.lpszText = text;
    SendMessageW(tooltip, TTM_GETTEXTW, 1024, reinterpret_cast<LPARAM>(&tool));
    return text;
}
HMENU TrackSubmenu(HMENU menu) {
    if (GetMenuItemID(menu, 0) == 0x7ef4) return menu;
    for (int index = 0; index < GetMenuItemCount(menu); ++index)
        if (HMENU child = GetSubMenu(menu, index))
            if (HMENU found = TrackSubmenu(child)) return found;
    return nullptr;
}
} // namespace

namespace ttplayer::testing {
struct SkinRebindAccess {
    static inline UINT dispatched{};
    static inline int initialized_track_count{};
    static LRESULT CALLBACK Owner(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* player = reinterpret_cast<ui::PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            player = static_cast<ui::PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(player));
        }
        if (message == WM_COMMAND) {
            // Verify the returned menu ID reaches the main window without
            // opening a real decoder, playing audio or editing user data.
            dispatched = LOWORD(wp); return 0;
        }
        if (player && message == WM_INITMENUPOPUP) {
            const auto menu = reinterpret_cast<HMENU>(wp);
            const UINT first = GetMenuItemID(menu, 0);
            const auto result = player->HandleMessage(message, wp, lp);
            if (first == 0x7ef4 || first == 10000) initialized_track_count = GetMenuItemCount(menu);
            return result;
        }
        if (player && (message == WM_MEASUREITEM || message == WM_DRAWITEM))
            return player->HandleMessage(message, wp, lp);
        return DefWindowProcW(window, message, wp, lp);
    }
    static void Run(HMODULE resources) {
        settings::Settings settings;
        settings.general.send_title_to_msn = false;
        settings.general.tray_icon = false;
        settings.general.fade_windows = false;
        settings.lyric.auto_download = false;
        settings.playlist.tag_title_format = L"%T";
        ui::PlayerWindow p(settings);
        p.SetSkinResourceModule(resources);
        p.instance_ = GetModuleHandleW(nullptr);
        WNDCLASSEXW type{sizeof(type)};
        type.hInstance = p.instance_; type.lpfnWndProc = Owner;
        type.lpszClassName = L"TTPlayerDesktopMenuTest";
        Require(RegisterClassExW(&type) != 0, "cannot register fixture window");
        Window owner{CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Desktop menu test",
            WS_POPUP, 0, 0, 327, 141, nullptr, nullptr, p.instance_, &p)};
        Require(owner.value != nullptr, "cannot create fixture window");
        p.window_ = owner.value;
        p.playlists_.NewList(L"Test playlist");
        UINT selection{};
        int opens{};
        bool native_tracking{};
        std::function<void(HMENU)> verify;
        auto tracker = [&](HMENU menu, POINT point, HWND bar) {
            Require(bar == p.desktop_lyrics_.BarHandle(), "menu owner is not DeskLrcBar");
            Require(GetMenuItemID(menu, 0) == 0x7ef4, "resource track placeholder changed");
            // Exercise the same ordering as PlayerWindow's real tracker:
            // style the skeleton, then let the bar forward WM_INITMENUPOPUP.
            p.BeginPopupMenuStyle(menu, true);
            if (native_tracking) {
                // Real Win32 modal menu: no synthetic INIT notification. A
                // thread timer ends only our test popup without user input.
                const UINT_PTR timer = SetTimer(nullptr, 0, 150,
                    [](HWND, UINT, UINT_PTR, DWORD) { EndMenu(); });
                Require(timer != 0, "cannot install menu timeout");
                TrackPopupMenuEx(menu, TPM_RETURNCMD,
                                 point.x, point.y, bar, nullptr);
                KillTimer(nullptr, timer);
            } else {
                SendMessageW(bar, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0);
            }
            for (int index = 0; index < GetMenuItemCount(menu); ++index) {
                MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_FTYPE | MIIM_DATA;
                Require(GetMenuItemInfoW(menu, index, TRUE, &item) &&
                    (item.fType & MFT_OWNERDRAW) && p.FindPopupMenuItem(item.dwItemData),
                    "dynamic menu row lost owner-draw style");
            }
            p.EndPopupMenuStyle();
            if (verify) verify(menu);
            ++opens;
            return selection;
        };
        RECT saved{100, 150, 740, 216};
        try {
            Require(p.desktop_lyrics_.Create(p.instance_, owner.value, owner.value, resources,
                &p.settings_.desktop_lyric, &saved, tracker), "cannot create desktop lyrics");
            const HWND bar = p.desktop_lyrics_.BarHandle();
            ToolSearch search{bar};
            EnumThreadWindows(GetCurrentThreadId(), FindTip, reinterpret_cast<LPARAM>(&search));
            Require(search.tooltip && SendMessageW(search.tooltip, TTM_GETTOOLCOUNT, 0, 0) == 12,
                    "expected 12 desktop tooltip registrations");
            Require(SendMessageW(search.tooltip, TTM_GETMAXTIPWIDTH, 0, 0) == -1,
                    "desktop tooltip has non-native wrapping width");
            Require((GetWindowLongPtrW(search.tooltip, GWL_STYLE) & (TTS_ALWAYSTIP | TTS_NOPREFIX)) == 0,
                    "desktop tooltip has non-native style");
            for (const auto command : {0x7dd8U, 0x7d05U, 0x7d06U, 0x803eU, 0x803fU,
                                        0x8040U, 0x8038U, 8U}) {
                auto expected = Resource(resources, command);
                if (auto split = expected.find(L'\n'); split != std::wstring::npos)
                    expected.erase(0, split + 1);
                Require(!expected.empty() && QueryTip(search.tooltip, bar, command) == expected,
                        "static resource tip differs from 5.7.9");
            }
            for (const bool playing : {false, true, false}) {
                p.desktop_lyrics_.UpdatePlayback(std::chrono::milliseconds(0), playing);
                Require(QueryTip(search.tooltip, bar, 0x7d00) == (playing ? L"暂停" : L"播放"),
                        "play/pause tooltip did not follow playback state");
            }
            for (const int lines : {1, 2, 1}) {
                p.settings_.desktop_lyric.lines = lines;
                Require(QueryTip(search.tooltip, bar, 0x8d1) == (lines == 1 ? L"双行显示" : L"单行显示"),
                        "line tooltip did not describe the next action");
            }
            for (const bool enabled : {false, true, false}) {
                p.settings_.desktop_lyric.karaoke_mode = enabled;
                p.settings_.desktop_lyric.topmost = enabled;
                Require(QueryTip(search.tooltip, bar, 0x866) == (enabled ? L"非卡拉OK模式" : L"卡拉OK模式"),
                        "karaoke tooltip did not split resource state");
                Require(QueryTip(search.tooltip, bar, 0x8042) == (enabled ? L"取消总在最前" : L"总在最前"),
                        "topmost tooltip always advertised cancellation");
            }
            NMTTDISPINFOA ansi{};
            ansi.hdr = {search.tooltip, reinterpret_cast<UINT_PTR>(GetDlgItem(bar, 0x803e)), TTN_GETDISPINFOA};
            SendMessageW(bar, WM_NOTIFY, ansi.hdr.idFrom, reinterpret_cast<LPARAM>(&ansi));
            wchar_t converted[80]{};
            MultiByteToWideChar(CP_ACP, 0, ansi.lpszText, -1, converted, 80);
            Require(std::wstring_view(converted) == L"播放曲目", "ANSI tooltip notification failed");
            std::cout << "12 resource tips, dynamic actions, ANSI/Unicode and native tooltip style passed\n";

            verify = [&](HMENU menu) {
                Require(GetMenuItemCount(menu) == 1 && GetMenuItemID(menu, 0) == 0x7ef4 &&
                    Label(menu, 0) == Resource(resources, 0x7ef4) &&
                    (GetMenuState(menu, 0, MF_BYPOSITION) & MF_DISABLED), "empty list placeholder differs");
            };
            SendMessageW(bar, WM_COMMAND, 0x803e, 0);
            for (int index = 0; index < 12; ++index) {
                playlist::Track track;
                track.path = L"test.wav"; track.title = "Song " + std::to_string(index + 1);
                track.metadata.emplace_back("Title", track.title);
                track.duration_ms = index == 1 ? -1 : 61000;
                p.ActivePlaylist().Add(std::move(track));
            }
            p.current_ = 1; p.playing_playlist_index_ = p.playlists_.ActiveIndex();
            verify = [&](HMENU menu) {
                Require(GetMenuItemCount(menu) == 12 && GetMenuItemID(menu, 0) == 10000 &&
                    GetMenuItemID(menu, 11) == 10011, "track menu was not filled from active list");
                Require(Label(menu, 0) == L"1.  Song 1\t[1:01]" && Label(menu, 1) == L"2.  Song 2" &&
                    Label(menu, 11) == L"12. Song 12\t[1:01]", "track number/title/duration formatting differs");
                Require((GetMenuState(menu, 1, MF_BYPOSITION) & MF_CHECKED) &&
                    !(GetMenuState(menu, 0, MF_BYPOSITION) & MF_CHECKED), "playing item marker differs");
            };
            selection = 10001; dispatched = 0;
            SendMessageW(bar, WM_COMMAND, 0x803e, 0);
            Require(dispatched == 10001, "selected track was not routed to main player");
            // The main context menu now uses the same lazy entry point.
            Menu main{p.BuildContextMenu()};
            const HMENU tracks = TrackSubmenu(main.value);
            Require(tracks != nullptr, "main menu track placeholder missing");
            p.HandleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(tracks), 0);
            p.EndPopupMenuStyle();
            verify(tracks);
            p.settings_.playlist.title_number = false;
            p.playlists_.NewList(L"Other list");
            // A playing index in a different playlist must not check this row.
            playlist::Track other; other.path = L"other.wav"; other.title = "Other";
            other.metadata.emplace_back("Title", other.title);
            other.duration_ms = -1; p.ActivePlaylist().Add(std::move(other));
            verify = [&](HMENU menu) {
                Require(GetMenuItemCount(menu) == 1 && Label(menu, 0) == L"Other" &&
                    !(GetMenuState(menu, 0, MF_BYPOSITION) & MF_CHECKED), "menu did not refresh after list change");
            };
            selection = 0; SendMessageW(bar, WM_COMMAND, 0x803e, 0);
            Require(opens == 3, "desktop list button did not invoke tracker each time");

            // Let TrackPopupMenuEx itself drive the forwarded notification.
            native_tracking = true;
            SendMessageW(bar, WM_COMMAND, 0x803e, 0);
            Require(opens == 4, "real Win32 popup did not open");
            native_tracking = false;
            p.settings_.general.menu_bar_playlist = true;
            for (int index = 1; index < 80; ++index) {
                playlist::Track item; item.path = L"entry.wav";
                p.ActivePlaylist().Add(std::move(item));
            }
            // Check optional column breaks without exhausting the system's
            // desktop menu heap with tens of thousands of native items.
            Menu large{CreatePopupMenu()};
            p.PopulateTrackMenu(large.value);
            Require(GetMenuItemCount(large.value) == 80 && GetMenuItemID(large.value, 79) == 10079,
                    "track command IDs lost their playlist indices");
            RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            const int rows = std::max(20L, (work.bottom - work.top) / 22);
            Require((GetMenuState(large.value, rows, MF_BYPOSITION) & MF_MENUBARBREAK) &&
                !(GetMenuState(large.value, rows - 1, MF_BYPOSITION) & MF_MENUBARBREAK),
                "multi-column track menu did not follow the configured row limit");
            // Use the application's actual popup callback too, so adding
            // TPM_NONOTIFY there cannot pass just a synthetic message test.
            p.desktop_lyrics_.Destroy();
            p.lyric_window_ = owner.value;
            Require(p.CreateDesktopLyrics(), "production desktop callback fixture failed");
            initialized_track_count = 0;
            const UINT_PTR timer = SetTimer(nullptr, 0, 150,
                [](HWND, UINT, UINT_PTR, DWORD) { EndMenu(); });
            Require(timer != 0, "production menu timeout failed");
            SendMessageW(p.desktop_lyrics_.BarHandle(), WM_COMMAND, 0x803e, 0);
            KillTimer(nullptr, timer);
            Require(initialized_track_count == 80 && !p.context_menu_open_,
                    "production popup suppressed initialization or left menu state active");
            std::cout << "empty/populated/refreshed menus, shared initialization, styling and dispatch passed\n";
            std::cout << "real TrackPopupMenuEx, production callback and optional menu columns passed\n";
            p.desktop_lyrics_.Destroy(); p.window_ = p.lyric_window_ = nullptr;
        } catch (...) {
            p.EndPopupMenuStyle(); p.desktop_lyrics_.Destroy(); p.window_ = p.lyric_window_ = nullptr;
            throw;
        }
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2, "expected repository root");
        const auto path = fs::path(argv[1]) / L"ttpres.dll";
        if (!fs::exists(path)) { std::cout << "5.7.9 ttpres.dll fixture unavailable\n"; return 77; }
        Require(SUCCEEDED(OleInitialize(nullptr)), "OLE init failed");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&controls) != FALSE, "common controls init failed");
        Module resources{LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)};
        Require(resources.value != nullptr, "cannot load resource-only DLL");
        testing::SkinRebindAccess::Run(resources.value);
        OleUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "desktop lyric menu test: " << error.what() << '\n'; return 1;
    }
}
