#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"

#include <limits>
#include <new>

namespace ttplayer::ui {
namespace {
using namespace detail;
constexpr wchar_t kNativeListState[] = L"TTPlayer.NativeListState";
WNDPROC native_list_proc{};

// The skin/model remains the owner of rows and playback selection. This is
// only the native control's cached projection, updated at the default-proc
// boundary. Mouse/paint traffic handled by the skin does not copy selections.
struct NativeListState {
    PlayerWindow* owner{};
    bool ready{}, synchronizing{};
    HFONT font{};
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
} // namespace

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
    if (!state || !state->ready || state->synchronizing || !state->owner ||
        message == WM_DESTROY)
        return Native(window, message, wparam, lparam);
    return state->owner->HandlePlaylistControlMessage(window, message, wparam, lparam);
}

LRESULT PlayerWindow::DefaultPlaylistListMessage(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = State(window);
    if (!state || !state->ready) return DefWindowProcW(window, message, wparam, lparam);
    const bool catalogue = GetDlgCtrlID(window) == kPlaylistListId;
    const size_t count = catalogue ? playlists_.Size() : VisiblePlaylistTrackCount();
    const auto focus = catalogue ? playlist_list_focus_ : playlist_selection_;
    const std::set<size_t> catalogue_selection = catalogue && playlist_list_selection_
        ? std::set<size_t>{*playlist_list_selection_} : std::set<size_t>{};
    const auto& selected = catalogue ? catalogue_selection : playlist_selected_rows_;
    const auto mark = catalogue ? (focus != state->focus ? focus : state->mark)
                                : playlist_selection_anchor_;
    state->synchronizing = true;
    if (!state->font) {
        // 16-pixel rows, independent of the font used by the skin renderer.
        state->font = CreateFontW(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH, L"Tahoma");
        Native(window, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
        LVCOLUMNW column{}; column.mask = LVCF_WIDTH; column.cx = 1;
        Native(window, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    }
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
    const int row_height = have_row ? std::max<LONG>(1, row.bottom - row.top) : 16;
    if (top != static_cast<size_t>(native_top)) {
        const auto delta = (static_cast<std::int64_t>(top) - native_top) * row_height;
        Native(window, LVM_SCROLL, 0, static_cast<LPARAM>(std::clamp<std::int64_t>(delta,
            std::numeric_limits<int>::min(), std::numeric_limits<int>::max())));
    }
    const auto result = Native(window, message, wparam, lparam);
    if (message == LVM_SETITEMSTATE || message == LVM_SETITEMW || message == LVM_SETITEMA ||
        message == LVM_SETSELECTIONMARK || message == LVM_SCROLL ||
        message == WM_VSCROLL || message == WM_KEYDOWN) {
        state->selected.clear();
        for (LRESULT item = Native(window, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED);
             item >= 0; item = Native(window, LVM_GETNEXTITEM, item, LVNI_SELECTED))
            state->selected.insert(static_cast<size_t>(item));
        state->focus = Row(Native(window, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED));
        state->mark = Row(Native(window, LVM_GETSELECTIONMARK));
        const auto scroll = static_cast<size_t>(Native(window, LVM_GETTOPINDEX));
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
    state->synchronizing = false;
    return result;
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
    // Projecting model state must never activate a playlist or start playback.
    if (State(header->hwndFrom)->synchronizing && header->code != LVN_ODFINDITEMW)
        return 0;
    return std::nullopt;
}
} // namespace ttplayer::ui
