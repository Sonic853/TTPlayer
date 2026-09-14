#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "ttplayer/core/text.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Policy() {
    using ttplayer::ui::PlaylistMarquee;
    PlaylistMarquee m;
    const RECT area{10, 20, 210, 220};
    const SIZE threshold{4, 4};
    const POINT start{180, 180};
    for (int mode = 0; mode < 3; ++mode) {
        m.Begin(start, area, 0, 16, {0, 5}, mode == 1, mode == 2);
        const auto initial = m.Update(start, area, 0, 16, 8, threshold);
        Check(initial == (mode ? std::set<size_t>{0, 5} : std::set<size_t>{}), "blank-click selection");
        Check(!m.Active(), "click became a drag");
        auto rows = m.Update({80, 85}, area, 0, 16, 8, threshold);
        const auto expected = mode == 1 ? std::set<size_t>{0, 4, 6, 7}
            : mode == 2 ? std::set<size_t>{0, 4, 5, 6, 7} : std::set<size_t>{4, 5, 6, 7};
        Check(rows == expected, "marquee replacement/Control XOR/Shift union");
        Check(m.Update({80, 85}, area, 0, 16, 8, threshold) == rows, "stationary Ctrl drag toggled twice");
        rows = m.Update(start, area, 0, 16, 8, threshold);
        Check(rows == initial, "shrinking rectangle did not restore original selection");
        m.Reset(); Check(!m.Pending() && !m.Active(), "gesture reset");
    }
    m.Begin({180, 130}, area, 20, 16, {}, false, false);
    auto rows = m.Update({80, -100}, area, 17, 16, 40, threshold);
    Check(rows.size() == 10 && *rows.begin() == 17 && *rows.rbegin() == 26, "scroll changed content anchor");
    Check(m.Bounds().top == area.top, "marquee escaped viewport");
    rows = m.Update({80, 300}, area, 20, 16, 3, threshold);
    Check(rows.empty(), "stale gesture selected out-of-range rows");
    m.Begin(start, area, 0, 16, {}, false, false);
    Check(m.Update({80, 20}, area, 0, 16, 0, threshold).empty(), "empty list marquee");
}
// Optional host baseline: the same SysListView32 styles used by 00482BAF.
// Real input is required because comctl32 runs its own tracking message loop.
void NativeProbe() {
    POINT previous{}; GetCursorPos(&previous);
    HWND parent = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"TTPlayer native marquee baseline",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 380, 380, nullptr, nullptr, nullptr, nullptr);
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"Files", 0x50015001,
        10, 10, 330, 310, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(list != nullptr, "native ListView creation");
    ListView_SetExtendedListViewStyle(list, 0x4420);
    LVCOLUMNW column{}; column.mask = LVCF_WIDTH; column.cx = 325;
    ListView_InsertColumn(list, 0, &column); ListView_SetItemCount(list, 8);
    ShowWindow(parent, SW_SHOW); SetForegroundWindow(parent); SetFocus(list); UpdateWindow(parent);
    RECT row{}, last{}; ListView_GetItemRect(list, 2, &row, LVIR_BOUNDS); ListView_GetItemRect(list, 7, &last, LVIR_BOUNDS);
    POINT start{300, last.bottom + 28}, end{80, (row.top + row.bottom) / 2};
    ClientToScreen(list, &start); ClientToScreen(list, &end);
    const auto input = [](DWORD type, WORD key, DWORD flags) {
        INPUT event{}; event.type = type;
        if (type == INPUT_MOUSE) event.mi.dwFlags = flags;
        else { event.ki.wVk = key; event.ki.dwFlags = flags; }
        SendInput(1, &event, sizeof(event));
    };
    bool compatible = true;
    for (int mode = 0; mode < 3; ++mode) {
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, 4, LVIS_SELECTED, LVIS_SELECTED);
        std::atomic_bool finished{};
        std::thread sender([&] {
            using namespace std::chrono_literals;
            // Each scenario presses at the same empty-space point. Without
            // this interval Windows can coalesce the next scenario into
            // WM_LBUTTONDBLCLK instead of beginning a fresh marquee.
            SetCursorPos(start.x, start.y);
            std::this_thread::sleep_for(std::chrono::milliseconds(GetDoubleClickTime() + 100));
            if (mode) input(INPUT_KEYBOARD, mode == 1 ? VK_CONTROL : VK_SHIFT, 0);
            input(INPUT_MOUSE, 0, MOUSEEVENTF_LEFTDOWN); std::this_thread::sleep_for(90ms);
            for (int step = 1; step <= 8; ++step) {
                SetCursorPos(start.x + (end.x-start.x)*step/8, start.y + (end.y-start.y)*step/8);
                std::this_thread::sleep_for(30ms);
            }
            input(INPUT_MOUSE, 0, MOUSEEVENTF_LEFTUP);
            if (mode) input(INPUT_KEYBOARD, mode == 1 ? VK_CONTROL : VK_SHIFT, KEYEVENTF_KEYUP);
            std::this_thread::sleep_for(50ms); finished = true;
        });
        while (!finished) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            Sleep(5);
        }
        sender.join();
        std::set<size_t> selected;
        for (int i=0; i<8; ++i) if (ListView_GetItemState(list, i, LVIS_SELECTED)) selected.insert(i);
        std::cout << "NATIVE mode=" << mode << " selected=";
        for (auto i:selected) std::cout << i << ',';
        std::cout << " focused=" << ListView_GetNextItem(list, -1, LVNI_FOCUSED) << '\n';
        auto expected = std::set<size_t>{2,3,4,5,6,7};
        if (mode) expected.insert(0);
        if (mode == 1) expected.erase(4);
        compatible = compatible && selected == expected &&
            ListView_GetNextItem(list, -1, LVNI_FOCUSED) == 0;
    }
    DestroyWindow(parent); SetCursorPos(previous.x, previous.y);
    Check(compatible, "host native input differed from recovered marquee policy");
}
}

namespace ttplayer::testing {
struct SkinRebindAccess {
    static LRESULT CALLBACK Window(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* p = reinterpret_cast<ui::PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (p && message != WM_DESTROY && message != WM_NCDESTROY)
            return p->HandlePlaylistMessage(message, wp, lp, window);
        return DefWindowProcW(window, message, wp, lp);
    }
    static LRESULT CALLBACK Child(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* p = reinterpret_cast<ui::PlayerWindow*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
        if (p) return p->HandlePlaylistControlMessage(window, message, wp, lp);
        return DefWindowProcW(window, message, wp, lp);
    }
    static void Send(HWND window, UINT message, POINT point, WPARAM keys = MK_LBUTTON) {
        SendMessageW(window, message, keys, MAKELPARAM(point.x, point.y));
    }
    static void Menus(ui::PlayerWindow& p, HMODULE resources) {
        using namespace ui::detail;
        // Exhaustively enumerate the actual 5.7.9 resources, not a second menu
        // definition. Print IDs/captions for the auditable static command map.
        const auto walk = [&](auto&& self, HMENU menu) -> void {
            for (int i = 0; i < GetMenuItemCount(menu); ++i) {
                if (const HMENU sub = GetSubMenu(menu, i)) { self(self, sub); continue; }
                const UINT id = GetMenuItemID(menu, i);
                if (!id || id == UINT_MAX) continue;
                const bool known = (id >= 0x7ef5 && id <= 0x7efa) || id == 0x7efc || id == 0x7efd ||
                    (id >= 0x7eff && id <= 0x7f06) || (id >= 0x7f09 && id <= 0x7f0d) ||
                    (id >= 0x7f13 && id <= 0x7f17) || (id >= 0x7f1d && id <= 0x7f26) ||
                    (id >= 0x7f30 && id <= 0x7f37) || (id >= 0x7f39 && id <= 0x7f3b) ||
                    (id >= 0x7f45 && id <= 0x7f4c) || (id >= 0x7f4e && id <= 0x7f51) ||
                    (id >= 0x7f58 && id <= 0x7fbb) || (id >= 0x7fe5 && id <= 0x7feb) || id == 0x80d1;
                wchar_t text[256]{}; GetMenuStringW(menu, i, text, 256, MF_BYPOSITION);
                std::cout << std::hex << id << std::dec << " " << core::WideToUtf8(text) << '\n';
                Check(known, "unmapped resource command (see ID above)");
            }
        };
        for (WORD resource : {WORD(139), WORD(152), WORD(153), WORD(156)}) {
            HMENU menu = LoadMenuW(resources, MAKEINTRESOURCEW(resource));
            Check(menu != nullptr, "playlist resource missing");
            std::cout << "RESOURCE " << resource << '\n';
            walk(walk, menu);
            DestroyMenu(menu);
        }
        HMENU menu = LoadMenuW(resources, MAKEINTRESOURCEW(139));
        Check(GetMenuItemCount(menu) == 7, "seven toolbar menus");
        p.settings_.player.play_mode = 4;
        p.PreparePlaylistMenu(menu);
        const HMENU modes = FindCommandMenu(menu, kPlaylistModeShuffle);
        Check((GetMenuState(modes, kPlaylistModeShuffle, MF_BYCOMMAND) & MF_CHECKED) != 0, "nested play-mode radio missing");
        p.settings_.player.play_mode = 1; p.PreparePlaylistMenu(menu);
        Check((GetMenuState(modes, kPlaylistModeShuffle, MF_BYCOMMAND) & MF_CHECKED) == 0, "old play-mode radio retained");
        Check((GetMenuState(modes, kPlaylistModeRepeatOne, MF_BYCOMMAND) & MF_CHECKED) != 0, "new play-mode radio missing");
        // Empty-track context menus deliberately skip PreparePlaylistMenu:
        // the shared WM_INITMENUPOPUP contract must still update Mode.
        p.settings_.player.play_mode = 3;
        p.settings_.player.auto_switch_list = true;
        SendMessageW(p.playlist_window_, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(modes), 0);
        Check((GetMenuState(modes, kPlaylistModeRepeatAll, MF_BYCOMMAND) & MF_CHECKED) != 0, "blank-context mode state stale");
        Check((GetMenuState(modes, kPlaylistAutoSwitchList, MF_BYCOMMAND) & MF_CHECKED) != 0, "blank-context auto-switch state stale");
        DestroyMenu(menu);
        menu = LoadMenuW(resources, MAKEINTRESOURCEW(156));
        p.playlist_list_selection_ = 0; p.PreparePlaylistMenu(menu);
        const HMENU lists = FindCommandMenu(menu, kPlaylistRenameList);
        Check(!(GetMenuState(lists, kPlaylistRenameList, MF_BYCOMMAND) & MF_GRAYED), "selected catalogue rename incorrectly disabled");
        p.playlist_list_selection_.reset(); p.PreparePlaylistMenu(menu);
        Check((GetMenuState(lists, kPlaylistRenameList, MF_BYCOMMAND) & MF_GRAYED) != 0, "rename without catalogue selection");
        DestroyMenu(menu);
        p.playlist_selected_rows_ = {2}; p.ActivePlaylist().SetRating(2, 4);
        menu = LoadMenuW(resources, MAKEINTRESOURCEW(152)); p.PreparePlaylistMenu(menu);
        const HMENU ratings = FindCommandMenu(menu, kPlaylistRatingFirst);
        Check((GetMenuState(ratings, kPlaylistRatingFirst + 3, MF_BYCOMMAND) & MF_CHECKED) != 0, "nested rating radio missing");
        DestroyMenu(menu);
        p.playlist_selection_ = 5;
        Check(p.HandlePlaylistCommand(kPlaylistSelectAll) && p.playlist_selected_rows_.size() == 8 && !p.playlist_selection_, "menu select-all clears caret");
        Check(p.HandlePlaylistCommand(kPlaylistSelectInvert) && p.playlist_selected_rows_.empty(), "menu invert");
        Check(p.HandlePlaylistCommand(kPlaylistSelectInvert) && p.playlist_selection_ == size_t{7}, "menu inverse focus");
        p.playlist_selection_ = 5;
        Check(p.HandlePlaylistCommand(kPlaylistSelectNone) && !p.playlist_selection_, "menu cancel selection retains caret");
    }
    static void Run(const std::filesystem::path& root, HMODULE resources) {
        using namespace ui::detail;
        settings::Settings settings;
        settings.general.send_title_to_msn = false;
        settings.general.fade_windows = false;
        settings.lyric.auto_download = false;
        settings.playlist.read_info_mode = 2;
        settings.playlist.enable_drag_drop = false; // marquee is NOT an OLE setting
        settings.player.play_follow_cursor = true; // selection must still not start playback
        ui::PlayerWindow p(settings);
        p.SetSkinResourceModule(resources);
        p.playlists_.NewList(L"Marquee fixture (memory only)");
        for (int i = 0; i < 8; ++i) {
            playlist::Track track;
            track.path = L"marquee-fixture-" + std::to_wstring(i) + L".wav";
            track.title = "Marquee fixture";
            p.ActivePlaylist().Add(std::move(track));
        }
        p.skin_.emplace(skin::LegacySkin::Load(root / L"reverse/ttpres/root/verified/default_skin"));
        Check(p.skin_->Valid(), "default skin missing");
        p.instance_ = GetModuleHandleW(nullptr);
        WNDCLASSW type{}; type.hInstance = p.instance_; type.lpfnWndProc = Window;
        type.lpszClassName = L"TTPlayerMarqueeTest"; Check(RegisterClassW(&type) != 0, "register parent");
        type.lpfnWndProc = Child; type.lpszClassName = kPlaylistListClass;
        Check(RegisterClassW(&type) != 0, "register child");
        p.playlist_window_ = CreateWindowExW(0, L"TTPlayerMarqueeTest", L"", WS_POPUP,
            0, 0, 400, 400, nullptr, nullptr, p.instance_, nullptr);
        Check(p.playlist_window_ != nullptr, "create parent");
        SetWindowLongPtrW(p.playlist_window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&p));
        p.CreatePlaylistListControls();
        Check(p.playlist_track_control_ != nullptr, "create transparent ListCtrl");
        const auto metrics = MakePlaylistGeometry(p.skin_->Playlist(), p.settings_.playlist.split_on_lists, 400, 400, 8);
        const POINT blank{metrics.tracks.right - 15, metrics.tracks.top + 9 * metrics.row_height};
        const POINT row2{metrics.tracks.left + 35, metrics.tracks.top + 2 * metrics.row_height + 8};
        Check(PtInRect(&metrics.tracks, blank), "fixture has no blank area");
        POINT child_blank = blank;
        MapWindowPoints(p.playlist_window_, p.playlist_track_control_, &child_blank, 1);
        for (WPARAM modifier : {WPARAM(0), WPARAM(MK_CONTROL), WPARAM(MK_SHIFT)}) {
            p.playlist_selected_rows_ = {0, 4}; p.playlist_selection_ = 0;
            Send(p.playlist_track_control_, WM_LBUTTONDOWN, child_blank, MK_LBUTTON | modifier);
            Check(GetCapture() == p.playlist_window_, "blank child did not capture parent");
            Send(p.playlist_window_, WM_MOUSEMOVE, row2, MK_LBUTTON | modifier);
            auto expected = std::set<size_t>{2, 3, 4, 5, 6, 7};
            if (modifier) expected.insert(0);
            if (modifier == MK_CONTROL) expected.erase(4);
            Check(p.playlist_selected_rows_ == expected, "real child-to-parent marquee selection");
            Check(p.playlist_selection_ == size_t{0} && !p.current_ && !p.playlist_track_drag_pending_, "marquee altered playback/caret or began OLE drag");
            // Exercise owner paint, not XOR directly on a transient window DC.
            HDC screen = GetDC(nullptr); HDC canvas = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, 400, 400);
            const auto previous = SelectObject(canvas, bitmap); p.PaintPlaylist(canvas);
            SelectObject(canvas, previous); DeleteObject(bitmap); DeleteDC(canvas); ReleaseDC(nullptr, screen);
            Send(p.playlist_window_, WM_LBUTTONUP, row2, modifier);
            Check(!p.playlist_marquee_.Pending() && GetCapture() != p.playlist_window_, "release left capture/frame");
        }
        for (UINT end : {WM_CANCELMODE, WM_CAPTURECHANGED, WM_KEYDOWN, WM_SHOWWINDOW}) {
            Send(p.playlist_track_control_, WM_LBUTTONDOWN, child_blank);
            Send(p.playlist_window_, WM_MOUSEMOVE, row2);
            if (end == WM_CAPTURECHANGED) ReleaseCapture();
            else SendMessageW(p.playlist_window_, end, end == WM_KEYDOWN ? VK_ESCAPE : 0, 0);
            Check(!p.playlist_marquee_.Pending(), "cancel/hide/Escape left gesture");
        }
        Send(p.playlist_track_control_, WM_LBUTTONDOWN, child_blank);
        Send(p.playlist_window_, WM_MOUSEMOVE, row2);
        p.RefreshPlaylist();
        Check(!p.playlist_marquee_.Pending() && GetCapture() != p.playlist_window_, "model refresh left stale gesture");
        // Start in the final page's empty space and drag above it: stationary
        // pointer timer ticks must keep scrolling and use the content anchor.
        p.playlist_scroll_ = 4;
        Send(p.playlist_track_control_, WM_LBUTTONDOWN, child_blank);
        Send(p.playlist_window_, WM_MOUSEMOVE, {row2.x, metrics.tracks.top - 30});
        SendMessageW(p.playlist_window_, WM_TIMER, kPlaylistMarqueeTimer, 0);
        Check(p.playlist_scroll_ < 4 && p.playlist_selected_rows_.contains(7), "marquee edge scroll");
        p.CancelPlaylistMarquee(true); p.playlist_scroll_ = 0;
        // A press on an item keeps the existing move path, not marquee.
        Send(p.playlist_window_, WM_LBUTTONDOWN, row2);
        Check(!p.playlist_marquee_.Pending() && p.playlist_track_drag_pending_, "item drag became marquee");
        Send(p.playlist_window_, WM_LBUTTONUP, row2, 0);
        Menus(p, resources);
        p.DestroyPlaylistListControls();
        SetWindowLongPtrW(p.playlist_window_, GWLP_USERDATA, 0);
        DestroyWindow(p.playlist_window_); p.playlist_window_ = nullptr;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2 || argc == 3, "repository path required");
        Check(SUCCEEDED(OleInitialize(nullptr)), "OLE initialization");
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        if (argc == 3) {
            Check(std::wstring_view(argv[2]) == L"--native", "unknown option");
            NativeProbe(); OleUninitialize(); return 0;
        }
        HMODULE resource = LoadLibraryExW((std::filesystem::path(argv[1]) / L"ttpres.dll").c_str(),
            nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Check(resource != nullptr, "original resources missing");
        Policy(); ttplayer::testing::SkinRebindAccess::Run(argv[1], resource);
        FreeLibrary(resource); OleUninitialize();
        std::cout << "PASS: marquee policy, real child/parent messages, painting, cleanup, resource menu states\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
