#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include <iostream>
#include <stdexcept>
#include <windowsx.h>

namespace ttplayer::testing {
using namespace ui::detail;
namespace {
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
}
struct SkinRebindAccess {
    static LRESULT CALLBACK Child(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        auto* player = reinterpret_cast<ui::PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (player && (message == WM_MOUSEWHEEL || (message >= LVM_FIRST && message < LVM_FIRST + 0x100)))
            return player->HandlePlaylistControlMessage(window, message, wp, lp);
        return DefWindowProcW(window, message, wp, lp);
    }
    struct Fixture {
        ui::PlayerWindow player{Quiet()};
        HWND edit{}, cover{};
        Fixture() {
            auto& p = player;
            p.instance_ = GetModuleHandleW(nullptr);
            p.window_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"wheel main", WS_POPUP,
                20, 20, 100, 80, nullptr, nullptr, p.instance_, nullptr);
            edit = CreateWindowExW(0, L"EDIT", L"typing", WS_CHILD | WS_VISIBLE,
                0, 0, 90, 40, p.window_, nullptr, p.instance_, nullptr);
            p.playlist_window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"wheel fixture",
                WS_POPUP, 140, 40, 400, 260, p.window_, nullptr, p.instance_, nullptr);
            Require(p.window_ && edit && p.playlist_window_, "fixture windows");
            p.skin_.emplace();
            // Geometry-only skin: no private resources, filesystem or profile.
            auto& skin = const_cast<skin::PlaylistSkin&>(p.skin_->Playlist());
            skin.valid = true; skin.background.size = {400, 260}; skin.list_bounds = {10, 20, 390, 220};
            p.settings_.playlist.split_on_lists = 100;
            p.playlist_list_control_ = CreateWindowExW(0, L"TTPlayerWheelFixture", L"PlayLists",
                WS_CHILD | WS_VISIBLE, 10, 20, 100, 200, p.playlist_window_,
                reinterpret_cast<HMENU>(kPlaylistListId), p.instance_, &p);
            p.playlist_track_control_ = CreateWindowExW(0, L"TTPlayerWheelFixture", L"Files",
                WS_CHILD | WS_VISIBLE, 115, 20, 275, 200, p.playlist_window_,
                reinterpret_cast<HMENU>(kPlaylistTrackId), p.instance_, &p);
            Require(p.playlist_list_control_ && p.playlist_track_control_, "fixture list controls");
            for (int i = 0; i < 35; ++i) p.playlists_.NewList(L"list");
            for (int i = 0; i < 80; ++i) {
                playlist::Track track; track.path = L"wheel-" + std::to_wstring(i) + L".wav";
                p.ActivePlaylist().Add(std::move(track));
            }
            ShowWindow(p.window_, SW_SHOWNOACTIVATE);
            ShowWindow(p.playlist_window_, SW_SHOWNOACTIVATE);
        }
        ~Fixture() {
            auto& p = player;
            if (GetCapture()) ReleaseCapture();
            if (cover) DestroyWindow(cover);
            DestroyWindow(p.playlist_window_);
            DestroyWindow(p.window_);
            p.playlist_window_ = p.window_ = p.playlist_list_control_ = p.playlist_track_control_ = nullptr;
            p.playlist_tree_control_ = p.playlist_view_ = nullptr;
        }
        static settings::Settings Quiet() {
            settings::Settings settings;
            settings.general.tray_icon = settings.general.fade_windows = settings.general.send_title_to_msn = false;
            settings.lyric.auto_download = settings.lyric.auto_load_lyric = false;
            settings.playlist.item_tips = false; settings.playlist.read_info_mode = 2;
            return settings;
        }
        static POINT Center(HWND window) {
            RECT bounds{}; GetWindowRect(window, &bounds);
            return {(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        }
        MSG Wheel(HWND recipient, HWND hover, int delta = -WHEEL_DELTA) {
            const auto point = Center(hover);
            Require(WindowFromPoint(point) == hover, "fixture target is obscured");
            MSG message{}; message.hwnd = recipient; message.message = WM_MOUSEWHEEL;
            message.wParam = MAKEWPARAM(0, static_cast<WORD>(delta));
            message.lParam = MAKELPARAM(point.x, point.y);
            // A delayed queue message must use its wheel coordinates, not the
            // current cursor position or an unrelated MSG.pt.
            message.pt = {0, 0};
            return message;
        }
        bool Dispatch(const MSG& message) {
            const bool consumed = player.PreTranslateMessage(message);
            if (!consumed) DispatchMessageW(&message);
            return consumed;
        }
    };
    static void Run() {
        WNDCLASSW type{}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = Child;
        type.lpszClassName = L"TTPlayerWheelFixture"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        Require(RegisterClassW(&type) != 0, "register fixture control");
        Fixture f; auto& p = f.player;
        SetFocus(f.edit);
        Require(GetFocus() == f.edit, "fixture keyboard focus");
        const HWND active = GetActiveWindow();
        p.playlist_scroll_ = 10; p.playlist_list_scroll_ = 5;
        p.playlist_selected_rows_ = {3, 8}; p.playlist_selection_ = 8;
        Require(f.Dispatch(f.Wheel(f.edit, p.playlist_track_control_)), "unfocused wheel not routed");
        Require(p.playlist_scroll_ == 13 && p.playlist_list_scroll_ == 5, "unfocused tracks did not scroll");
        Require(GetFocus() == f.edit && GetActiveWindow() == active && p.playlist_selected_rows_ == std::set<size_t>{3, 8} &&
            p.playlist_selection_ == size_t{8} && !p.current_, "wheel changed focus/activation/selection/playback");

        SetFocus(p.playlist_list_control_);
        Require(f.Dispatch(f.Wheel(p.playlist_list_control_, p.playlist_track_control_)), "catalogue-to-tracks route");
        Require(p.playlist_scroll_ == 16 && p.playlist_list_scroll_ == 5 && GetFocus() == p.playlist_list_control_,
            "catalogue focus overrode hovered tracks");
        SetFocus(p.playlist_track_control_);
        Require(f.Dispatch(f.Wheel(p.playlist_track_control_, p.playlist_list_control_)), "tracks-to-catalogue route");
        Require(p.playlist_scroll_ == 16 && p.playlist_list_scroll_ == 8, "tracks focus overrode hovered catalogue");
        Require(!f.Dispatch(f.Wheel(p.playlist_track_control_, p.playlist_track_control_)), "direct wheel was redirected");
        Require(p.playlist_scroll_ == 19, "direct wheel lost or double-scrolled");
        // Also exercise direct child dispatch, bypassing the queue filter.
        SetFocus(p.playlist_list_control_);
        const auto direct = f.Wheel(p.playlist_track_control_, p.playlist_track_control_, WHEEL_DELTA);
        SendMessageW(direct.hwnd, direct.message, direct.wParam, direct.lParam);
        Require(p.playlist_scroll_ == 16 && p.playlist_list_scroll_ == 8, "direct child still used other pane focus");

        SetFocus(f.edit);
        auto wheel = f.Wheel(f.edit, p.playlist_track_control_);
        EnableWindow(p.window_, FALSE);
        Require(!p.RoutePlaylistMouseWheel(wheel), "routed through disabled modal owner");
        EnableWindow(p.window_, TRUE);
        EnableWindow(p.playlist_track_control_, FALSE);
        Require(!p.RoutePlaylistMouseWheel(wheel), "routed into disabled target");
        EnableWindow(p.playlist_track_control_, TRUE);
        ShowWindow(p.playlist_window_, SW_HIDE);
        Require(!p.RoutePlaylistMouseWheel(wheel), "routed into hidden playlist");
        ShowWindow(p.playlist_window_, SW_SHOWNOACTIVATE);
        SetCapture(f.edit);
        Require(!p.RoutePlaylistMouseWheel(wheel), "wheel stolen during capture");
        ReleaseCapture();
        const auto point = Fixture::Center(p.playlist_track_control_);
        f.cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"TTPlayerWheelFixture", L"cover", WS_POPUP,
            point.x - 20, point.y - 20, 40, 40, nullptr, nullptr, p.instance_, nullptr);
        ShowWindow(f.cover, SW_SHOWNOACTIVATE);
        Require(WindowFromPoint(point) == f.cover && !p.RoutePlaylistMouseWheel(wheel), "scrolled an obscured playlist");
        DestroyWindow(f.cover); f.cover = nullptr;
        wheel.message = WM_KEYDOWN;
        Require(!p.RoutePlaylistMouseWheel(wheel), "keyboard message treated as wheel");
        wheel.message = WM_MOUSEWHEEL; wheel.lParam = MAKELPARAM(20, 20);
        Require(!p.RoutePlaylistMouseWheel(wheel), "outside point routed to playlist");

        // The original LibraryTree remains a native tree; redirect to it, not
        // the custom track scroller. Its own wheel procedure determines rows.
        p.settings_.playlist.library_mode = true;
        ShowWindow(p.playlist_list_control_, SW_HIDE);
        p.playlist_tree_control_ = CreateWindowExW(0, WC_TREEVIEWW, L"LibraryTree", WS_CHILD | WS_VISIBLE,
            10, 20, 100, 200, p.playlist_window_, nullptr, p.instance_, nullptr);
        Require(p.playlist_tree_control_ != nullptr, "native tree fixture");
        for (int i = 0; i < 60; ++i) {
            TVINSERTSTRUCTW item{}; item.hParent = TVI_ROOT; item.hInsertAfter = TVI_LAST;
            item.item.mask = TVIF_TEXT; item.item.pszText = const_cast<wchar_t*>(L"library item");
            TreeView_InsertItem(p.playlist_tree_control_, &item);
        }
        SetFocus(f.edit);
        const auto before = TreeView_GetFirstVisible(p.playlist_tree_control_);
        Require(f.Dispatch(f.Wheel(f.edit, p.playlist_tree_control_)), "native tree wheel route");
        Require(TreeView_GetFirstVisible(p.playlist_tree_control_) != before &&
            p.playlist_scroll_ == 16 && GetFocus() == f.edit, "native tree routing moved tracks/focus");

        // Classic fallback uses an ordinary LISTBOX and must receive its own
        // wheel message while another control owns keyboard focus.
        ShowWindow(p.playlist_window_, SW_HIDE);
        p.playlist_view_ = CreateWindowExW(0, L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL,
            0, 42, 95, 36, p.window_, nullptr, p.instance_, nullptr);
        for (int i = 0; i < 40; ++i) SendMessageW(p.playlist_view_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"song"));
        SetFocus(f.edit);
        Require(f.Dispatch(f.Wheel(f.edit, p.playlist_view_)), "fallback list wheel route");
        Require(SendMessageW(p.playlist_view_, LB_GETTOPINDEX, 0, 0) > 0 && GetFocus() == f.edit,
            "fallback list did not scroll without focus");
    }
    static void Bottom() {
        // 00425DEC proves the original base class is SysListView32. Compare
        // against the real control's page/range/row rectangles, not a second
        // copy of the rebuilt scrollbar formula.
        struct Font {
            HFONT value = CreateFontW(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH, L"Tahoma");
            ~Font() { if (value) DeleteObject(value); }
        } font;
        Fixture f; auto& p = f.player;
        auto& skin = const_cast<skin::PlaylistSkin&>(p.skin_->Playlist());
        skin.scrollbar_buttons.size = {30, 20};
        skin.scrollbar_thumb.size = {30, 18};
        HWND native = CreateWindowExW(0, WC_LISTVIEWW, L"Files", 0x50015001U,
            0, 1000, 300, 200, p.window_, nullptr, p.instance_, nullptr);
        Require(native && font.value, "native page oracle");
        SendMessageW(native, WM_SETFONT, reinterpret_cast<WPARAM>(font.value), FALSE);
        LVCOLUMNW column{}; column.mask = LVCF_WIDTH; column.cx = 200;
        ListView_InsertColumn(native, 0, &column);
        ListView_SetItemCount(native, 80);
        RECT row{}; ListView_GetItemRect(native, 0, &row, LVIR_BOUNDS);
        const int native_row_height = row.bottom - row.top;
        Require(native_row_height > 1, "native oracle row height");
        // The host DPI/theme can use a different native row height from a
        // skin's authored 16 pixels. Compare the same full/partial row
        // capacity and normalize row rectangles to row units in that case.
        for (const int height : {128, 129, 136, 143, 144, 160}) {
            skin.list_bounds.bottom = skin.list_bounds.top + height;
            const int remainder = height % 16;
            const int native_height = (height / 16) * native_row_height +
                (remainder ? std::clamp(remainder * native_row_height / 16, 1, native_row_height - 1) : 0);
            SetWindowPos(native, nullptr, 0, 1000, 300, native_height, SWP_NOZORDER | SWP_NOACTIVATE);
            for (const int count : {0, 1, 8, 9, 80}) {
                p.ActivePlaylist().Clear();
                for (int i = 0; i < count; ++i) {
                    playlist::Track track; track.path = L"bottom-" + std::to_wstring(i) + L".wav";
                    p.ActivePlaylist().Add(std::move(track));
                }
                ListView_SetItemCount(native, count);
                SendMessageW(native, WM_VSCROLL, SB_BOTTOM, 0);
                const auto top = static_cast<size_t>(ListView_GetTopIndex(native));
                p.playlist_scroll_ = 999;
                p.LayoutPlaylistListControls();
                if (p.playlist_scroll_ != top) {
                    RECT client{}; GetClientRect(native, &client);
                    std::cerr << "height=" << height << " count=" << count
                        << " native_height=" << native_height << " native_row=" << native_row_height
                        << " native_client=" << client.bottom << " native_page=" << ListView_GetCountPerPage(native)
                        << " native_top=" << top << " rebuilt_top=" << p.playlist_scroll_ << '\n';
                }
                Require(p.playlist_scroll_ == top, "resize/layout bottom differs from native ListView");
                Require(SendMessageW(p.playlist_track_control_, LVM_GETCOUNTPERPAGE, 0, 0) ==
                    ListView_GetCountPerPage(native), "page count included partial row");
                p.playlist_scroll_ = 0; p.ScrollPlaylist(10000);
                Require(p.playlist_scroll_ == top, "scroll bottom differs from native ListView");
                const auto metrics = MakePlaylistGeometry(skin, p.settings_.playlist.split_on_lists, 400, 260, count);
                Require((metrics.scrollbar_width > 0) == (count > std::max(1, ListView_GetCountPerPage(native))),
                    "partial bottom row suppressed scrollbar");
                Require(metrics.visible_rows == std::max(1, (height + 15) / 16), "partial row lost from painting");
                p.playlist_scroll_ = 999;
                SendMessageW(p.playlist_track_control_, LVM_SETITEMCOUNT, count, 0);
                Require(p.playlist_scroll_ == top, "item count update shrank final page");
                if (count) {
                    RECT expected{}, actual{};
                    ListView_GetItemRect(native, count - 1, &expected, LVIR_BOUNDS);
                    ListView_GetItemRect(p.playlist_track_control_, count - 1, &actual, LVIR_BOUNDS);
                    Require(actual.top * native_row_height == expected.top * 16 &&
                        actual.bottom * native_row_height == expected.bottom * 16,
                        "last row bottom/blank space differs from native ListView");
                    p.playlist_scroll_ = 0;
                    SendMessageW(p.playlist_track_control_, LVM_ENSUREVISIBLE, count - 1, FALSE);
                    Require(p.playlist_scroll_ == top, "ensure-visible left last row clipped");
                    p.playlist_scroll_ = 0; p.playlist_selection_ = static_cast<size_t>(count - 1);
                    p.EnsurePlaylistSelectionVisible();
                    Require(p.playlist_scroll_ == top, "selection/end navigation left last row clipped");
                    p.playlist_scroll_ = 0;
                    const auto wheel = f.Wheel(p.playlist_track_control_, p.playlist_track_control_, -12000);
                    SendMessageW(wheel.hwnd, wheel.message, wheel.wParam, wheel.lParam);
                    Require(p.playlist_scroll_ == top, "wheel bottom differs from native ListView");
                    if (top > 0) {
                        p.playlist_scroll_ = 0;
                        // At the top, the fixed 18px thumb starts below the
                        // 10px up button. Drag it beyond the lower endpoint.
                        const int x = (metrics.scrollbar.left + metrics.scrollbar.right) / 2;
                        const int y = metrics.scrollbar.top + 10 + 9;
                        p.HandlePlaylistMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y), nullptr);
                        Require(p.playlist_scrollbar_dragging_, "scrollbar thumb did not capture");
                        p.HandlePlaylistMessage(WM_MOUSEMOVE, MK_LBUTTON,
                            MAKELPARAM(x, metrics.scrollbar.bottom + 100), nullptr);
                        p.HandlePlaylistMessage(WM_LBUTTONUP, 0,
                            MAKELPARAM(x, metrics.scrollbar.bottom + 100), nullptr);
                        Require(p.playlist_scroll_ == top && !GetCapture(), "thumb bottom differs from native ListView");
                    }
                }
            }
        }
        // TRUE permits a partially visible row; FALSE exposes the entire row.
        skin.list_bounds.bottom = skin.list_bounds.top + 136;
        p.LayoutPlaylistListControls();
        p.playlist_scroll_ = 0;
        SendMessageW(p.playlist_track_control_, LVM_ENSUREVISIBLE, 8, TRUE);
        Require(p.playlist_scroll_ == 0, "partial-okay ensure-visible scrolled unnecessarily");
        SendMessageW(p.playlist_track_control_, LVM_ENSUREVISIBLE, 8, FALSE);
        Require(p.playlist_scroll_ == 1, "full ensure-visible accepted half row");
        p.playlist_list_scroll_ = 0;
        const auto catalogue = f.Wheel(p.playlist_list_control_, p.playlist_list_control_, -12000);
        SendMessageW(catalogue.hwnd, catalogue.message, catalogue.wParam, catalogue.lParam);
        Require(p.playlist_list_scroll_ == p.playlists_.Size() - 8, "catalogue bottom used painted row count");
        std::cout << "PASS: native ListView bottom/page/row bounds at 6 heights and 5 item counts, "
                     "resize, item count, wheel, thumb, ensure-visible and selection\n";
    }
};
}

int wmain() {
    try {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&common);
        ttplayer::testing::SkinRebindAccess::Run();
        ttplayer::testing::SkinRebindAccess::Bottom();
        std::cout << "PASS: unfocused wheel routing, pane targeting, single dispatch, focus/selection preservation, "
                     "modal/hidden/occlusion/capture boundaries, native tree and fallback list\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
