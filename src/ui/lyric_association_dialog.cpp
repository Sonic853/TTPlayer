#include "lyric_association_dialog.h"
#include "modern_file_dialog.h"
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>

namespace ttplayer::ui::detail {
namespace {
constexpr int kFiles = 1064, kQuery = 1009, kSearch = 1046, kBrowse = 1023;
constexpr int kAssociate = 2115, kRemove = 2116, kRename = 2117, kDelete = 2118;
constexpr int kPartial = 2119, kBlock = 2120, kAll = 2087;
constexpr UINT kSearchReady = WM_APP + 206;
std::wstring Text(HMODULE module, UINT id) {
    const wchar_t* data{};
    const int length = LoadStringW(module, id, reinterpret_cast<LPWSTR>(&data), 0);
    return length > 0 ? std::wstring(data, length) : std::wstring{};
}
std::wstring Field(HWND dialog, int id) {
    const HWND window = GetDlgItem(dialog, id);
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), static_cast<int>(text.size())));
    return text;
}
void Columns(HWND list, HMODULE module, UINT resource) {
    auto text = Text(module, resource);
    const auto split = text.find(L'|');
    for (int i = 0; i < 2; ++i) {
        auto label = i == 0 ? text.substr(0, split) : split == text.npos ? L"" : text.substr(split + 1);
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = label.data();
        column.cx = resource == 0x8158 ? (i ? 200 : 80) : 235; // 00447BAA/00447BD9, 00445B30.
        ListView_InsertColumn(list, i, &column);
    }
}
void Row(HWND list, int index, const std::wstring& first, const std::wstring& second) {
    LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = index;
    item.pszText = const_cast<wchar_t*>(first.c_str());
    ListView_InsertItem(list, &item);
    ListView_SetItemText(list, index, 1, const_cast<wchar_t*>(second.c_str()));
}
void Center(HWND dialog) {
    RECT bounds{}, parent{};
    GetWindowRect(dialog, &bounds);
    const HWND owner = GetWindow(dialog, GW_OWNER);
    GetWindowRect(owner, &parent);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const LONG width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const LONG x = std::clamp((parent.left + parent.right - width) / 2, monitor.rcWork.left,
        std::max(monitor.rcWork.left, monitor.rcWork.right - width));
    const LONG y = std::clamp((parent.top + parent.bottom - height) / 2, monitor.rcWork.top,
        std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // DialogBoxParam handles disabled-owner attention and owned topmost
    // ordering natively; do not create another top-level owner or UI loop.
}
std::optional<std::filesystem::path> Browse(HMODULE module, HWND owner, const std::filesystem::path& current) {
    ModernOpenFileOptions options;
    options.owner = owner;
    const auto filter = Text(module, 0x8128);
    auto name = filter.substr(0, filter.find(L'|'));
    options.filters = {{name, L"*.lrc;*.txt"}};
    if (current != lyrics::kNoLyric) options.initial_path = current;
    return ModernOpenFile(options);
}
struct Dialog {
    HMODULE resources{};
    lyrics::AssociationStore* store{};
    lyrics::SongKey song;
    lyrics::LocalSearchRequest request;
    std::function<void()> changed;
    LyricAssociationChoice choice;
    std::filesystem::path associated;
    std::vector<std::filesystem::path> paths;
    std::vector<lyrics::SongKey> all_keys;
    std::shared_ptr<lyrics::LocalSearchJob> job;
    bool blocked{};
    HFONT link_font{};
    ~Dialog() { CancelSearch(); if (link_font) DeleteObject(link_font); }
    static LRESULT CALLBACK LinkProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR reference) {
        const auto* self = reinterpret_cast<Dialog*>(reference);
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, LinkProc, id);
        if (!self->blocked && message == WM_GETDLGCODE)
            return DLGC_WANTCHARS |
                ((wparam == VK_RETURN || wparam == VK_SPACE) ? DLGC_WANTMESSAGE : 0);
        // Finish the key gesture before entering a nested modal loop. Leaving
        // an Enter key-up/translated char queued can activate its default button.
        if (!self->blocked && message == WM_KEYUP && (wparam == VK_RETURN || wparam == VK_SPACE)) {
            SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(kAll, STN_CLICKED), reinterpret_cast<LPARAM>(window));
            return 0;
        }
        if (!self->blocked && ((message == WM_CHAR && (wparam == L' ' || wparam == L'\r')) ||
            (message == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_SPACE)))) return 0;
        return DefSubclassProc(window, message, wparam, lparam);
    }
    void CancelSearch() { if (job) job->canceled = true; job.reset(); }
    void Selection(HWND window) {
        const bool selected = ListView_GetNextItem(GetDlgItem(window, kFiles), -1, LVNI_SELECTED) >= 0;
        for (int id : {kAssociate, kRename, kDelete}) EnableWindow(GetDlgItem(window, id), selected && !blocked);
    }
    void Populate(HWND window) {
        const HWND list = GetDlgItem(window, kFiles);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        for (size_t i = 0; i < paths.size(); ++i)
            Row(list, static_cast<int>(i), paths[i].filename().wstring(), paths[i].parent_path().wstring());
        if (!paths.empty()) ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
        Selection(window);
    }
    void Initialize(HWND window) {
        associated = store->Find(song).value_or(std::filesystem::path{});
        blocked = associated == lyrics::kNoLyric;
        // 00447C18 -> 0042BC90 subclasses resource STATIC 2087. Its template
        // has WS_TABSTOP, but NOT SS_NOTIFY: without this actual clicks vanish.
        const HWND link = GetDlgItem(window, kAll);
        auto style = GetWindowLongPtrW(link, GWL_STYLE);
        SetWindowLongPtrW(link, GWL_STYLE, blocked ? (style & ~SS_NOTIFY) : (style | SS_NOTIFY));
        SetWindowSubclass(link, LinkProc, 206, reinterpret_cast<DWORD_PTR>(this));
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        if (!blocked && !link_font) {
            LOGFONTW info{};
            if (GetObjectW(font, sizeof(info), &info)) { info.lfUnderline = TRUE; link_font = CreateFontIndirectW(&info); }
        }
        SendMessageW(link, WM_SETFONT, reinterpret_cast<WPARAM>(blocked ? font : link_font), TRUE);
        for (int id : {kQuery, kPartial, kSearch, kBrowse, kRemove, kRename, kDelete, kFiles, kAssociate})
            EnableWindow(GetDlgItem(window, id), !blocked);
        EnableWindow(GetDlgItem(window, kRemove), !blocked && !associated.empty());
        if (blocked) SetDlgItemTextW(window, kBlock, Text(resources, 0x803a).c_str());
        else {
            auto query = request.artist.empty() ? request.media.stem().wstring() : request.artist + L" - " + request.title;
            if (query.empty()) query = request.media.stem().wstring();
            SetDlgItemTextW(window, kQuery, query.c_str());
            PostMessageW(window, kSearchReady, 0, 0);
        }
        Selection(window);
    }
    void Search(HWND window) {
        CancelSearch(); paths.clear();
        if (!associated.empty() && !blocked) paths.push_back(associated);
        Populate(window);
        const auto query = Field(window, kQuery);
        if (query.empty()) { SetFocus(GetDlgItem(window, kQuery)); return; }
        auto search = request;
        const auto dash = query.find(L'-');
        search.artist = dash == query.npos ? L"" : query.substr(0, dash);
        search.title = dash == query.npos ? query : query.substr(dash + 1);
        search.media = std::filesystem::path(query + L".lrc");
        search.partial = IsDlgButtonChecked(window, kPartial) == BST_CHECKED;
        search.all_matches = true;
        job = lyrics::SearchLocalLyricsAsync(std::move(search));
        EnableWindow(GetDlgItem(window, kSearch), FALSE);
        SetTimer(window, 1, 50, nullptr);
    }
    void Poll(HWND window) {
        if (!job) return;
        std::optional<lyrics::LocalSearchResult> result;
        { std::lock_guard lock(job->mutex); if (job->result) result = std::move(job->result); }
        if (!result) return;
        CancelSearch(); KillTimer(window, 1);
        for (auto& path : result->matches)
            if (_wcsicmp(path.c_str(), associated.c_str())) paths.push_back(std::move(path));
        Populate(window);
        if (!paths.empty()) {
            ListView_SetColumnWidth(GetDlgItem(window, kFiles), 0, LVSCW_AUTOSIZE_USEHEADER);
            ListView_SetColumnWidth(GetDlgItem(window, kFiles), 1, LVSCW_AUTOSIZE_USEHEADER);
        }
        EnableWindow(GetDlgItem(window, kSearch), !blocked);
        SendMessageW(window, DM_SETDEFID, paths.empty() ? kBrowse : kAssociate, 0);
    }
    void PopulateAll(HWND window) {
        const HWND list = GetDlgItem(window, kFiles);
        ListView_DeleteAllItems(list); all_keys.clear();
        for (const auto& [key, path] : store->All()) {
            Row(list, static_cast<int>(all_keys.size()), key.path.wstring(), path.wstring());
            all_keys.push_back(key);
        }
        EnableWindow(GetDlgItem(window, 0x802d), FALSE);
        EnableWindow(GetDlgItem(window, 0x802e), FALSE);
    }
};
INT_PTR CALLBACK AllProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<Dialog*>(lparam);
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        ListView_SetExtendedListViewStyle(GetDlgItem(window, kFiles), LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
        Columns(GetDlgItem(window, kFiles), self->resources, 0x8159);
        InstallLyricAssociationImages(window, self->resources, true);
        self->PopulateAll(window); Center(window); return TRUE;
    }
    if (!self) return FALSE;
    const HWND list = GetDlgItem(window, kFiles);
    if (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL))) {
        EndDialog(window, IDCANCEL); return TRUE;
    }
    if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->idFrom == kFiles) {
        const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        EnableWindow(GetDlgItem(window, 0x802d), ListView_GetSelectedCount(list) == 1);
        EnableWindow(GetDlgItem(window, 0x802e), selected >= 0);
        if (reinterpret_cast<NMHDR*>(lparam)->code == LVN_KEYDOWN &&
            reinterpret_cast<NMLVKEYDOWN*>(lparam)->wVKey == VK_DELETE) SendMessageW(window, WM_COMMAND, 0x802e, 0);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    const int command = LOWORD(wparam);
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    bool changed{};
    if (command == 0x802d && selected >= 0 && ListView_GetSelectedCount(list) == 1) {
        const auto key = self->all_keys[static_cast<size_t>(selected)];
        if (auto path = Browse(self->resources, window, self->store->Find(key).value_or(std::filesystem::path{})))
            changed = self->store->Set(key, *path);
    } else if (command == 0x802e) {
        for (int index = selected; index >= 0; index = ListView_GetNextItem(list, index, LVNI_SELECTED))
            changed |= self->store->Erase(self->all_keys[static_cast<size_t>(index)]);
    } else if (command == 0x854) {
        for (const auto& key : self->all_keys) {
            const auto path = self->store->Find(key);
            std::error_code error;
            const bool media_missing = !std::filesystem::is_regular_file(key.path, error);
            // Do not mistake the intentional no-lyric sentinel for a broken path.
            if (path && (media_missing || (*path != lyrics::kNoLyric && !std::filesystem::is_regular_file(*path, error))))
                changed |= self->store->Erase(key);
        }
    } else return FALSE;
    if (changed) { self->PopulateAll(window); if (self->changed) self->changed(); }
    return TRUE;
}
INT_PTR CALLBACK AssociateProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<Dialog*>(lparam);
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        ListView_SetExtendedListViewStyle(GetDlgItem(window, kFiles), LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
        Columns(GetDlgItem(window, kFiles), self->resources, 0x8158);
        InstallLyricAssociationImages(window, self->resources, false);
        self->Initialize(window); Center(window); return TRUE;
    }
    if (!self) return FALSE;
    const HWND list = GetDlgItem(window, kFiles);
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    const bool valid = selected >= 0 && static_cast<size_t>(selected) < self->paths.size();
    if (message == WM_DESTROY) { KillTimer(window, 1); self->CancelSearch(); return TRUE; }
    if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
    if (!self->blocked && message == WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(lparam) == GetDlgItem(window, kAll)) {
        SetTextColor(reinterpret_cast<HDC>(wparam), RGB(0, 0, 255));
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
    }
    if (!self->blocked && message == WM_SETCURSOR && reinterpret_cast<HWND>(wparam) == GetDlgItem(window, kAll)) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
    }
    if (message == WM_TIMER && wparam == 1) { self->Poll(window); return TRUE; }
    if (message == kSearchReady) { if (!self->blocked) self->Search(window); return TRUE; }
    if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->idFrom == kFiles) {
        const auto code = reinterpret_cast<NMHDR*>(lparam)->code;
        if (code == LVN_ITEMCHANGED) self->Selection(window);
        if (code == NM_DBLCLK || code == LVN_ITEMACTIVATE) SendMessageW(window, WM_COMMAND, kAssociate, 0);
        if (code == LVN_ENDLABELEDITW) {
            const auto& item = reinterpret_cast<NMLVDISPINFOW*>(lparam)->item;
            bool okay{};
            if (item.pszText && item.iItem >= 0 && static_cast<size_t>(item.iItem) < self->paths.size()) {
                const std::wstring name = item.pszText;
                const auto old = self->paths[item.iItem];
                const auto target = old.parent_path() / name;
                if (!name.empty() && name != L"." && name != L".." && name.find_first_of(L"<>:\"/\\|?*") == name.npos)
                    okay = MoveFileW(old.c_str(), target.c_str()) != FALSE;
                if (okay) self->paths[item.iItem] = target;
                else MessageBoxW(window, Text(self->resources, 0x8187).c_str(), Text(self->resources, 0x80).c_str(), MB_ICONERROR);
            }
            SetWindowLongPtrW(window, DWLP_MSGRESULT, okay); return TRUE;
        }
        return FALSE;
    }
    if (message != WM_COMMAND) return FALSE;
    const int command = LOWORD(wparam);
    if (command == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
    if (command == kAll && !self->blocked) {
        self->CancelSearch(); KillTimer(window, 1);
        DialogBoxParamW(self->resources, MAKEINTRESOURCEW(205), window, AllProc, reinterpret_cast<LPARAM>(self));
        self->Initialize(window); return TRUE;
    }
    if (command == kBlock) { EndDialog(window, self->blocked ? 6 : 7); return TRUE; }
    if (self->blocked) return FALSE;
    if (command == kQuery && HIWORD(wparam) == EN_SETFOCUS) { SendMessageW(window, DM_SETDEFID, kSearch, 0); return TRUE; }
    if (command == kSearch) { self->Search(window); return TRUE; }
    if (command == kRemove && !self->associated.empty()) { EndDialog(window, kRemove); return TRUE; }
    if (command == kBrowse) {
        if (const auto path = Browse(self->resources, window, valid ? self->paths[selected] : self->associated)) {
            self->choice.path = *path; EndDialog(window, IDOK);
        }
        return TRUE;
    }
    if (command == kAssociate && valid) { self->choice.path = self->paths[selected]; EndDialog(window, IDOK); return TRUE; }
    if (command == kRename && valid) { SetFocus(list); ListView_EditLabel(list, selected); return TRUE; }
    if (command == kDelete && valid) {
        // 004480E8: delete precisely the selected file, with recycle/confirmation.
        auto path = self->paths[selected].wstring(); path.push_back(L'\0'); path.push_back(L'\0');
        SHFILEOPSTRUCTW operation{}; operation.hwnd = window; operation.wFunc = FO_DELETE;
        operation.pFrom = path.c_str(); operation.fFlags = FOF_ALLOWUNDO;
        if (SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted) {
            self->paths.erase(self->paths.begin() + selected); self->Populate(window);
        }
        return TRUE;
    }
    return FALSE;
}
}
LyricAssociationChoice ChooseLyricAssociation(HMODULE resources, HWND owner,
    lyrics::AssociationStore& store, const lyrics::SongKey& song,
    lyrics::LocalSearchRequest request, std::function<void()> changed) {
    Dialog dialog;
    dialog.resources = resources; dialog.store = &store; dialog.song = song;
    dialog.request = std::move(request); dialog.changed = std::move(changed);
    dialog.choice.action = DialogBoxParamW(resources, MAKEINTRESOURCEW(206), owner,
        AssociateProc, reinterpret_cast<LPARAM>(&dialog));
    return dialog.choice;
}
} // namespace ttplayer::ui::detail
