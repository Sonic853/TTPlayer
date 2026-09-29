#include "ttplayer/ui/wtl_runtime.h"
#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"

#include <limits>
#include <new>

namespace ttplayer::ui {
namespace {
using namespace detail;
constexpr wchar_t kNativeListState[] = L"TTPlayer.NativeListState";
WNDPROC native_list_proc{};

// A native list HWND owns one ATL/WTL binding. NativeListState remains the
// skin/model projection, and no additional song data is stored by this class.
class NativeListWindow final : public ATL::CWindowImpl<NativeListWindow, WTL::CListViewCtrl> {
public:
    WNDPROC handler{};
    BOOL ProcessWindowMessage(HWND window, UINT message, WPARAM wp, LPARAM lp,
                              LRESULT& result, DWORD = 0) override {
        result = handler(window, message, wp, lp);
        if (message == WM_NCDESTROY) m_dwState |= WINSTATE_DESTROYED;
        return TRUE;
    }
    void OnFinalMessage(HWND) override { delete this; }
    static bool Bind(HWND window, WNDPROC handler) {
        auto binding = std::unique_ptr<NativeListWindow>(new (std::nothrow) NativeListWindow);
        if (!binding) return false;
        binding->handler = handler;
        if (!binding->SubclassWindow(window)) return false;
        binding.release();
        return true;
    }
};

// The skin/model remains the owner of rows and playback selection. This is
// only the native control's cached projection, updated at the default-proc
// boundary. Skin drawing and left-button gestures use the model directly;
// native right clicks publish their state before the context menu opens.
struct NativeListState {
    PlayerWindow* owner{};
    bool ready{}, synchronizing{}, column_created{};
    HFONT font{};
    LOGFONTW font_descriptor{};
    int row_height{16};
    int count{-1}, width{-1};
    DWORD extended_style{MAXDWORD};
    std::set<size_t> selected;
    std::optional<size_t> focus, mark;
    ~NativeListState() { if (font) DeleteObject(font); }
};

NativeListState* State(HWND window) {
    return reinterpret_cast<NativeListState*>(GetPropW(window, kNativeListState));
}
LRESULT Native(HWND window, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
    return CallWindowProcW(native_list_proc, window, message, wparam, lparam);
}
std::optional<size_t> Row(LRESULT value) {
    return value < 0 ? std::nullopt : std::optional<size_t>{static_cast<size_t>(value)};
}

void ApplyListFont(HWND window, NativeListState& state,
                   const settings::PlaylistSettings& settings) {
    const auto descriptor = PlaylistFontDescriptor(settings);
    if (state.font && memcmp(&descriptor, &state.font_descriptor, sizeof(descriptor)) == 0)
        return;
    const HFONT font = CreateFontIndirectW(&descriptor);
    if (!font) return;
    const bool synchronizing = state.synchronizing;
    state.synchronizing = true;
    // WM_SETFONT belongs to SysListView32 (0048301E -> 004052D6), including
    // its font mapping and item padding on the running Windows version.
    Native(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    if (state.font) DeleteObject(state.font);
    state.font = font;
    state.font_descriptor = descriptor;
    if (!state.column_created) {
        LVCOLUMNW column{}; column.mask = LVCF_WIDTH; column.cx = 1;
        Native(window, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
        state.column_created = true;
    }
    // Empty owner-data lists have no item rectangle. Measure one temporary
    // native row without changing the model, selection or cached item count.
    // This keeps empty->populated lists and font changes equally correct.
    const auto count = Native(window, LVM_GETITEMCOUNT);
    if (count == 0)
        Native(window, LVM_SETITEMCOUNT, 1, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    RECT bounds{LVIR_BOUNDS};
    if (Native(window, LVM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&bounds)))
        state.row_height = std::max<LONG>(1, bounds.bottom - bounds.top);
    if (count == 0)
        Native(window, LVM_SETITEMCOUNT, 0, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    state.synchronizing = synchronizing;
}
} // namespace

int PlayerWindow::PlaylistRowHeight() const {
    for (const HWND window : {playlist_track_control_, playlist_list_control_}) {
        if (auto* state = State(window); state && state->ready) {
            if (!state->synchronizing) ApplyListFont(window, *state, settings_.playlist);
            return state->row_height;
        }
    }
    // Only used before the list HWNDs exist; their first layout measures the
    // real font. No visible list is rendered with this creation-time default.
    return 16;
}

bool PlayerWindow::RegisterPlaylistListClass(HINSTANCE instance) {
    WNDCLASSEXW type{sizeof(type)};
    if (GetClassInfoExW(instance, kPlaylistListClass, &type))
        return type.lpfnWndProc == PlaylistListWindowProc && native_list_proc;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&common) ||
        !GetClassInfoExW(nullptr, WC_LISTVIEWW, &type)) return false;
    native_list_proc = type.lpfnWndProc;
    type.style &= ~CS_GLOBALCLASS;
    type.hInstance = instance;
    type.lpszClassName = kPlaylistListClass;
    type.lpfnWndProc = PlaylistListWindowProc;
    return RegisterClassExW(&type) != 0;
}

LRESULT CALLBACK PlayerWindow::PlaylistListWindowProc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = State(window);
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = new (std::nothrow) NativeListState;
        if (!state) return FALSE;
        state->owner = static_cast<PlayerWindow*>(create->lpCreateParams);
        if (!SetPropW(window, kNativeListState, state)) { delete state; return FALSE; }
        const auto result = Native(window, message, wparam, lparam);
        if (!result) { RemovePropW(window, kNativeListState); delete state; }
        else if (!NativeListWindow::Bind(window, PlaylistListWindowProc)) return FALSE;
        return result;
    }
    if (message == WM_NCDESTROY) {
        const auto result = Native(window, message, wparam, lparam);
        RemovePropW(window, kNativeListState);
        delete state;
        return result;
    }
    if (message == WM_CREATE) {
        const auto result = Native(window, message, wparam, lparam);
        if (state) state->ready = result != -1;
        return result;
    }
    // The skin paints the full client surface and its scrollbar. Native
    // scrolling/state still run, but must not reserve/draw a second scrollbar.
    if (message == WM_NCCALCSIZE || message == WM_NCPAINT) return 0;
    if (message == WM_NCHITTEST) return DefWindowProcW(window, message, wparam, lparam);
    // Native right-button handling can enter the popup menu's nested message
    // loop before DefaultPlaylistListMessage returns. The synchronization
    // guard must not give client painting back to SysListView32 during that
    // loop: it would erase the skin and draw the system's white/black list.
    if (state && state->ready && state->owner &&
        (message == WM_PAINT || message == WM_ERASEBKGND))
        return state->owner->HandlePlaylistControlMessage(window, message, wparam, lparam);
    if (!state || !state->ready || state->synchronizing || !state->owner ||
        message == WM_DESTROY)
        return Native(window, message, wparam, lparam);
    return state->owner->HandlePlaylistControlMessage(window, message, wparam, lparam);
}

LRESULT PlayerWindow::DefaultPlaylistListMessage(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = State(window);
    if (!state || !state->ready) return DefWindowProcW(window, message, wparam, lparam);
    // SysListView32 may bubble an unconsumed partial/modifier wheel message
    // to its parent. The skin parent must not route it back into that same
    // native call: this would accumulate the delta twice or recurse forever.
    if (state->synchronizing && message == WM_MOUSEWHEEL) return 0;
    // The original report ListViews leave Ctrl/Shift wheel input unhandled.
    // Consume it here so the skin/owner cannot turn the bubbled message into
    // a second gesture or move keyboard focus to the hovered pane.
    if (message == WM_MOUSEWHEEL && (LOWORD(wparam) & (MK_CONTROL | MK_SHIFT))) return 0;
    const bool catalogue = GetDlgCtrlID(window) == kPlaylistListId;
    const size_t count = catalogue ? playlists_.Size() : VisiblePlaylistTrackCount();
    const auto focus = catalogue ? playlist_list_focus_ : playlist_selection_;
    const std::set<size_t> catalogue_selection = catalogue && playlist_list_selection_
        ? std::set<size_t>{*playlist_list_selection_} : std::set<size_t>{};
    const auto& selected = catalogue ? catalogue_selection : playlist_selected_rows_;
    const auto mark = catalogue ? (focus != state->focus ? focus : state->mark)
                                : playlist_selection_anchor_;
    state->synchronizing = true;
    ApplyListFont(window, *state, settings_.playlist);
    RECT client{}; GetClientRect(window, &client);
    if (state->width != client.right) {
        state->width = client.right;
        // The report control reserves its vertical scrollbar internally even
        // when the skin handles non-client painting. Leave that gutter out of
        // the hidden column so it cannot create an extra horizontal scroll row.
        Native(window, LVM_SETCOLUMNWIDTH, 0,
            std::max<LONG>(1, client.right - GetSystemMetrics(SM_CXVSCROLL) - 1));
    }
    const int native_count = static_cast<int>(std::min<size_t>(count,
        std::numeric_limits<int>::max()));
    const bool count_changed = state->count != native_count;
    if (count_changed) {
        state->count = native_count;
        Native(window, LVM_SETITEMCOUNT, native_count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    }
    const DWORD extended = catalogue ? playlist_list_extended_style_ : playlist_track_extended_style_;
    if (state->extended_style != extended) {
        Native(window, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, extended);
        state->extended_style = extended;
    }
    if (count_changed || state->selected != selected || state->focus != focus) {
        LVITEMW item{}; item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        Native(window, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&item));
        item.stateMask = item.state = LVIS_SELECTED;
        for (const auto row : selected)
            if (row < count) Native(window, LVM_SETITEMSTATE, row, reinterpret_cast<LPARAM>(&item));
        item.stateMask = item.state = LVIS_FOCUSED;
        if (focus && *focus < count)
            Native(window, LVM_SETITEMSTATE, *focus, reinterpret_cast<LPARAM>(&item));
        state->selected = selected;
        state->focus = focus;
    }
    Native(window, LVM_SETSELECTIONMARK, 0, mark ? static_cast<LPARAM>(*mark) : -1);
    state->mark = mark;
    const size_t top = catalogue ? playlist_list_scroll_ : playlist_scroll_;
    const auto native_top = Native(window, LVM_GETTOPINDEX);
    RECT row{LVIR_BOUNDS};
    const bool have_row = Native(window, LVM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&row)) != 0;
    const int row_height = have_row ? std::max<LONG>(1, row.bottom - row.top) : state->row_height;
    if (top != static_cast<size_t>(native_top)) {
        const auto delta = (static_cast<std::int64_t>(top) - native_top) * row_height;
        Native(window, LVM_SCROLL, 0, static_cast<LPARAM>(std::clamp<std::int64_t>(delta,
            std::numeric_limits<int>::min(), std::numeric_limits<int>::max())));
    }
    const auto result = Native(window, message, wparam, lparam);
    if (message == LVM_SETITEMSTATE || message == LVM_SETITEMW || message == LVM_SETITEMA ||
        message == LVM_SETSELECTIONMARK || message == LVM_SCROLL ||
        message == WM_VSCROLL || message == WM_KEYDOWN || message == WM_MOUSEWHEEL) {
        ReadPlaylistNativeState(window);
    }
    state->synchronizing = false;
    return result;
}

void PlayerWindow::ReadPlaylistNativeState(HWND window) {
    auto* state = State(window);
    if (!state || !state->ready) return;
    const bool catalogue = GetDlgCtrlID(window) == kPlaylistListId;
    state->selected.clear();
    for (LRESULT item = Native(window, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED);
         item >= 0; item = Native(window, LVM_GETNEXTITEM, item, LVNI_SELECTED))
        state->selected.insert(static_cast<size_t>(item));
    state->focus = Row(Native(window, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED));
    state->mark = Row(Native(window, LVM_GETSELECTIONMARK));
    RECT client{};
    GetClientRect(window, &client);
    const size_t count = catalogue ? playlists_.Size() : VisiblePlaylistTrackCount();
    const size_t page = static_cast<size_t>(std::max<LONG>(1, client.bottom / state->row_height));
    // The skin consumes the whole client area and owns its scrollbar. Native
    // bookkeeping can still reserve a hidden scrollbar gutter after a count
    // change, so never expose a top row past the skin's last complete page.
    const auto scroll = std::min(static_cast<size_t>(Native(window, LVM_GETTOPINDEX)),
        count > page ? count - page : 0);
    if (catalogue) {
        playlist_list_selection_ = state->selected.empty() ? std::nullopt
            : std::optional<size_t>{*state->selected.begin()};
        playlist_list_focus_ = state->focus;
        playlist_list_scroll_ = scroll;
        LayoutPlaylistListEdit();
    } else {
        playlist_selected_rows_ = state->selected;
        playlist_selection_ = state->focus;
        playlist_selection_anchor_ = state->mark;
        playlist_scroll_ = scroll;
        if (!settings_.playlist.library_mode)
            RememberPlaylistRow(playlists_.ActiveIndex(), playlist_selection_);
        UpdatePlaylistItemTipRects();
    }
    InvalidateRect(GetParent(window), nullptr, FALSE);
}

std::optional<LRESULT> PlayerWindow::PlaylistNativeNotification(LPARAM value) {
    auto* header = reinterpret_cast<NMHDR*>(value);
    if (!header || !State(header->hwndFrom)) return std::nullopt;
    if (header->code == LVN_GETDISPINFOW || header->code == LVN_GETDISPINFOA) {
        const bool unicode = header->code == LVN_GETDISPINFOW;
        auto* info = reinterpret_cast<NMLVDISPINFOW*>(value);
        if ((info->item.mask & LVIF_TEXT) == 0 || !info->item.pszText ||
            info->item.cchTextMax <= 0) return 0;
        const int row = info->item.iItem;
        std::wstring text;
        if (row >= 0 && header->idFrom == kPlaylistListId &&
            static_cast<size_t>(row) < playlists_.Size()) text = playlists_.At(row).Title();
        else if (row >= 0 && header->idFrom == kPlaylistTrackId) {
            if (const auto* track = VisiblePlaylistTrack(row))
                text = PlaylistDisplayText(*track);
        }
        if (unicode) lstrcpynW(info->item.pszText, text.c_str(), info->item.cchTextMax);
        else {
            auto* ansi = reinterpret_cast<NMLVDISPINFOA*>(value);
            const int length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string converted(static_cast<size_t>(std::max(1, length)), '\0');
            WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, converted.data(), length, nullptr, nullptr);
            lstrcpynA(ansi->item.pszText, converted.c_str(), ansi->item.cchTextMax);
        }
        return 0;
    }
    // 00489D2E/00488FEF build menus from the selection already established by
    // SysListView32. Publish it before the nested popup loop, not after the
    // right-button procedure returns (when a menu command may have run).
    if (header->code == NM_RCLICK) {
        ReadPlaylistNativeState(header->hwndFrom);
        if (header->idFrom == kPlaylistListId && playlist_list_selection_)
            SwitchPlaylist(*playlist_list_selection_);
        return 0;
    }
    // Projecting model state must never activate a playlist or start playback.
    if (State(header->hwndFrom)->synchronizing && header->code != LVN_ODFINDITEMW)
        return 0;
    return std::nullopt;
}
} // namespace ttplayer::ui
