#include "player_window_internal.h"
#include "../app/resource_ids.h"
#include <algorithm>

namespace ttplayer::ui {
using namespace detail;
namespace {
std::wstring Text(UINT id) {
    LPCWSTR value{};
    const int size = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&value), 0);
    return size > 0 ? std::wstring(value, size) : std::wstring{};
}
std::wstring Field(HWND dialog, int id) {
    const HWND control = GetDlgItem(dialog, id);
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    value.resize(GetWindowTextW(control, value.data(), static_cast<int>(value.size())));
    return value;
}
void Status(HWND dialog, UINT id) { SetDlgItemTextW(dialog, IDC_LYRIC_SERVICES_STATUS, Text(id).c_str()); }
std::optional<lyrics::ServiceCatalog> Completed(std::shared_ptr<lyrics::CatalogJob>& pending) {
    if (!pending) return {};
    auto job = pending;
    std::lock_guard lock(job->mutex);
    if (!job->result) return {};
    auto result = std::move(job->result);
    pending.reset();
    return result;
}
}

void PlayerWindow::InstallLyricServiceEditorButton(HWND dialog) {
    const HWND previous = GetDlgItem(dialog, 2185);
    if (!previous) return;
    wchar_t klass[32]{}; GetClassNameW(previous, klass, 32);
    if (_wcsicmp(klass, L"Button") == 0) return;
    RECT rect{}; GetWindowRect(previous, &rect);
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
    const auto font = SendMessageW(previous, WM_GETFONT, 0, 0);
    const HWND after = GetWindow(previous, GW_HWNDPREV);
    DestroyWindow(previous);
    RECT minimum{0,0,42,14}; MapDialogRect(dialog, &minimum);
    const LONG width = std::max(rect.right - rect.left, minimum.right);
    rect.left = rect.right - width; // retain the resource's right margin
    const HWND combo = GetDlgItem(dialog, 2090);
    RECT combo_rect{}, dropdown{};
    GetWindowRect(combo, &combo_rect);
    SendMessageW(combo, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&dropdown));
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&combo_rect), 2);
    if (combo && combo_rect.right > rect.left - 4)
        SetWindowPos(combo, nullptr, 0, 0, std::max(20L, rect.left - 4 - combo_rect.left),
            dropdown.bottom - dropdown.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const HWND button = CreateWindowExW(0, WC_BUTTONW, Text(IDS_LYRIC_SERVICES_EDIT).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        rect.left, rect.top - 2, width, minimum.bottom,
        dialog, reinterpret_cast<HMENU>(2185), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(button, WM_SETFONT, font, TRUE);
    SetWindowPos(button, after ? after : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PlayerWindow::RefreshLyricServices() {
    // Once per options opening, not per page activation or timer tick.
    if (lyric_catalog_save_job_) { lyric_catalog_refresh_pending_ = true; return; }
    lyric_catalog_job_ = lyrics::ReadServiceCatalogAsync(sound_library_
        ? sound_library_->LyricSearchProviders() : std::vector<plugins::LyricSearchProviderInfo>{});
}
void PlayerWindow::SelectLyricService(int index) {
    if (index < 0 || static_cast<size_t>(index) >= lyric_services_.entries.size()) return;
    settings_.lyric.add_in_index = index;
    settings_.lyric.server_key = lyric_services_.entries[index].key;
}
void PlayerWindow::PopulateLyricServices(HWND dialog) {
    if (!dialog || !IsWindow(dialog)) return;
    const HWND combo = GetDlgItem(dialog, 2090);
    if (!combo) return; // automatic result dialog 208 has no selector
    int selected = settings_.lyric.add_in_index;
    if (!settings_.lyric.server_key.empty()) {
        const auto found = std::find_if(lyric_services_.entries.begin(), lyric_services_.entries.end(),
            [&](const auto& entry) { return entry.key == settings_.lyric.server_key; });
        selected = found == lyric_services_.entries.end() ? 0 : static_cast<int>(found - lyric_services_.entries.begin());
    }
    if (selected < 0 || static_cast<size_t>(selected) >= lyric_services_.entries.size()) selected = 0;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& entry : lyric_services_.entries)
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.name.c_str()));
    SendMessageW(combo, CB_SETCURSEL, lyric_services_.entries.empty() ? -1 : selected, 0);
    SelectLyricService(selected);
    bool busy = false;
    if (lyric_search_ && dialog == lyric_search_dialog_) {
        const auto phase = lyric_search_->Snapshot().phase;
        busy = phase == lyrics::SearchPhase::searching || phase == lyrics::SearchPhase::downloading;
    }
    EnableWindow(combo, lyric_services_ready_ && !lyric_services_.entries.empty() && !busy);
    if (dialog == lyric_search_dialog_)
        EnableWindow(GetDlgItem(dialog, 1046), lyric_services_ready_ && !lyric_services_.entries.empty() && !busy);
}
void PlayerWindow::PollLyricServices() {
    bool changed = false;
    if (auto result = Completed(lyric_catalog_job_)) {
        lyric_services_ = std::move(*result); lyric_services_ready_ = true; changed = true;
        if (lyric_service_editor_ && !lyric_service_dirty_) {
            lyric_editor_baseline_ = lyric_services_;
            lyric_service_draft_ = lyric_services_.entries;
            PopulateLyricServiceEditor(0);
        }
    }
    if (auto result = Completed(lyric_catalog_save_job_)) {
        // Draft keys retain their pre-edit identity until commit. Renaming the
        // selected row must not silently select the first server instead.
        const auto selected = std::find_if(lyric_service_draft_.begin(), lyric_service_draft_.end(),
            [&](const auto& entry) { return entry.key == settings_.lyric.server_key; });
        if (selected != lyric_service_draft_.end()) {
            const auto replacement = lyrics::ServiceKey(*selected);
            if (std::any_of(result->entries.begin(), result->entries.end(),
                [&](const auto& entry) { return entry.key == replacement; }))
                settings_.lyric.server_key = replacement;
        }
        lyric_services_ = std::move(*result); lyric_services_ready_ = true; changed = true;
        lyric_editor_baseline_ = lyric_services_;
        if (lyric_services_.error.empty()) {
            lyric_service_dirty_ = false;
            lyric_service_draft_ = lyric_services_.entries;
        }
        if (lyric_service_editor_) {
            PopulateLyricServiceEditor(lyric_service_selection_);
            if (lyric_services_.error.empty()) Status(lyric_service_editor_, IDS_LYRIC_SERVICES_SAVED);
        }
        if (lyric_catalog_refresh_pending_) { lyric_catalog_refresh_pending_ = false; RefreshLyricServices(); }
    }
    if (changed) {
        auto found = std::find_if(lyric_services_.entries.begin(), lyric_services_.entries.end(),
            [&](const auto& entry) { return entry.key == settings_.lyric.server_key; });
        SelectLyricService(found == lyric_services_.entries.end() ?
            (settings_.lyric.server_key.empty() ? std::clamp(settings_.lyric.add_in_index, 0,
                std::max(0, static_cast<int>(lyric_services_.entries.size()) - 1)) : 0) :
            static_cast<int>(found - lyric_services_.entries.begin()));
        PopulateLyricServices(options_pages_[8]);
        PopulateLyricServices(lyric_search_dialog_);
    }
    if (lyric_services_pending_auto_ && lyric_services_ready_) {
        lyric_services_pending_auto_ = false;
        if (lyric_path_.empty() && !lyric_editor_) StartOnlineLyricSearch(true);
    }
    if (lyric_upload_pending_ && lyric_services_ready_) ContinueLyricUpload();
}

void PlayerWindow::ShowLyricServiceEditor(HWND owner_window) {
    if (lyric_service_editor_ && IsWindow(lyric_service_editor_)) {
        HWND target = lyric_service_editor_;
        if (!IsWindowEnabled(target)) {
            // The unsaved-changes MessageBox owns input until it is dismissed.
            // Do not activate its disabled owner through a repeated Options entry.
            target = GetWindow(target, GW_ENABLEDPOPUP);
            if (!IsWindowVisible(target) || !IsWindowEnabled(target) ||
                (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_NOACTIVATE)) return;
        }
        ShowWindow(target, SW_SHOWNORMAL); SetForegroundWindow(target); return;
    }
    if (!lyric_services_ready_ && !lyric_catalog_job_) RefreshLyricServices();
    lyric_editor_baseline_ = lyric_services_;
    lyric_service_draft_ = lyric_services_.entries; lyric_service_dirty_ = false;
    // Disable the same window that owns the dialog. User32 then redirects a
    // click on the entry window to this popup, including its native attention flash.
    // A sibling owned by the player cannot participate in that relationship.
    const HWND modal_owner = IsWindow(owner_window) ? owner_window :
        IsWindow(options_window_) ? options_window_ :
        IsWindow(lyric_search_dialog_) ? lyric_search_dialog_ : window_;
    lyric_service_editor_ = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_LYRIC_SERVICES),
        modal_owner, LyricServiceEditorProc, reinterpret_cast<LPARAM>(this));
    if (lyric_service_editor_) {
        if (modal_owner == lyric_search_dialog_) CancelOnlineLyricCountdown();
        if (IsWindow(modal_owner) && modal_owner != window_ && IsWindowEnabled(modal_owner)) {
            lyric_service_disabled_owner_ = modal_owner;
            EnableWindow(modal_owner, FALSE);
        }
        const HWND anchor = modal_owner;
        RECT bounds{}, owner{};
        GetWindowRect(lyric_service_editor_, &bounds); GetWindowRect(anchor, &owner);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) {
            if (IsRectEmpty(&owner)) owner = monitor.rcWork;
            const LONG width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
            const LONG x = std::clamp((owner.left + owner.right - width) / 2, monitor.rcWork.left,
                std::max(monitor.rcWork.left, monitor.rcWork.right - width));
            const LONG y = std::clamp((owner.top + owner.bottom - height) / 2, monitor.rcWork.top,
                std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
            SetWindowPos(lyric_service_editor_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(lyric_service_editor_, SW_SHOWNORMAL);
        ApplySkinWindowTopMost(); SetForegroundWindow(lyric_service_editor_);
    }
}
void PlayerWindow::CloseLyricServiceEditor() {
    // Restore the owner BEFORE destroying the active popup so User32 can
    // return activation/focus to it. WM_DESTROY also covers external teardown.
    const HWND owner = std::exchange(lyric_service_disabled_owner_, nullptr);
    if (owner && (owner == options_window_ || owner == lyric_search_dialog_) && IsWindow(owner))
        EnableWindow(owner, TRUE);
    if (lyric_service_editor_ && IsWindow(lyric_service_editor_)) DestroyWindow(lyric_service_editor_);
    lyric_service_editor_ = nullptr;
}
void PlayerWindow::PopulateLyricServiceEditor(int selected) {
    if (!lyric_service_editor_) return;
    lyric_service_populating_ = true;
    const HWND list = GetDlgItem(lyric_service_editor_, IDC_LYRIC_SERVICES_LIST);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < lyric_service_draft_.size(); ++i) {
        auto& entry = lyric_service_draft_[i];
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = entry.name.data();
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, item.iItem, 1, entry.url.data());
    }
    if (!lyric_service_draft_.empty()) {
        selected = std::clamp(selected, 0, static_cast<int>(lyric_service_draft_.size()) - 1);
        ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selected, FALSE);
    } else selected = -1;
    lyric_service_populating_ = false;
    SelectLyricServiceEditorRow(selected);
}
void PlayerWindow::SelectLyricServiceEditorRow(int index) {
    lyric_service_selection_ = index;
    const bool exists = index >= 0 && static_cast<size_t>(index) < lyric_service_draft_.size();
    const auto* row = exists ? &lyric_service_draft_[index] : nullptr;
    const HWND dialog = lyric_service_editor_;
    if (!dialog) return;
    const bool busy = !lyric_services_ready_ || lyric_catalog_save_job_ != nullptr;
    lyric_service_populating_ = true;
    SetDlgItemTextW(dialog, IDC_LYRIC_SERVICES_NAME, row ? row->name.c_str() : L"");
    SetDlgItemTextW(dialog, IDC_LYRIC_SERVICES_URL, row ? row->url.c_str() : L"");
    SetDlgItemTextW(dialog, IDC_LYRIC_SERVICES_STORAGE, row ? row->storage.c_str() : L"");
    for (int id : {IDC_LYRIC_SERVICES_NAME, IDC_LYRIC_SERVICES_URL})
        SendDlgItemMessageW(dialog, id, EM_SETREADONLY, busy || !row || row->read_only, 0);
    EnableWindow(GetDlgItem(dialog, IDC_LYRIC_SERVICES_LIST), !busy);
    EnableWindow(GetDlgItem(dialog, IDC_LYRIC_SERVICES_DELETE), !busy && row && !row->read_only);
    EnableWindow(GetDlgItem(dialog, IDC_LYRIC_SERVICES_ADD), !busy && !lyric_editor_baseline_.files.empty());
    EnableWindow(GetDlgItem(dialog, IDC_LYRIC_SERVICES_UP),
        !busy && lyrics::CanMoveService(lyric_service_draft_, index, -1));
    EnableWindow(GetDlgItem(dialog, IDC_LYRIC_SERVICES_DOWN),
        !busy && lyrics::CanMoveService(lyric_service_draft_, index, 1));
    EnableWindow(GetDlgItem(dialog, IDOK), !busy && lyric_service_dirty_);
    EnableWindow(GetDlgItem(dialog, IDCANCEL), !lyric_catalog_save_job_);
    if (lyric_catalog_save_job_) Status(dialog, IDS_LYRIC_SERVICES_SAVING);
    else if (!lyric_services_ready_) Status(dialog, IDS_LYRIC_SERVICES_LOADING);
    else if (!lyric_editor_baseline_.error.empty())
        SetDlgItemTextW(dialog, IDC_LYRIC_SERVICES_STATUS, lyric_editor_baseline_.error.c_str());
    else Status(dialog, lyric_editor_baseline_.files.empty() ? IDS_LYRIC_SERVICES_EMPTY :
        row && row->read_only ? IDS_LYRIC_SERVICES_READONLY : IDS_LYRIC_SERVICES_CUSTOM);
    lyric_service_populating_ = false;
}
void PlayerWindow::SaveLyricServiceEditor() {
    if (!lyric_service_dirty_ || lyric_catalog_save_job_ || !lyric_services_ready_) return;
    lyric_catalog_job_.reset(); // stale read must never publish over this save
    lyric_catalog_save_job_ = lyrics::SaveServiceCatalogAsync(lyric_editor_baseline_, lyric_service_draft_);
    SelectLyricServiceEditorRow(lyric_service_selection_);
}
INT_PTR CALLBACK PlayerWindow::LyricServiceEditorProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<PlayerWindow*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp); self->lyric_service_editor_ = dialog;
        self->InstallLyricServiceEditorButtons(dialog);
        const HWND tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, dialog, nullptr, GetModuleHandleW(nullptr), nullptr);
        for (const auto [button, text] : {std::pair{IDC_LYRIC_SERVICES_ADD, IDS_LYRIC_SERVICES_ADD},
            {IDC_LYRIC_SERVICES_DELETE, IDS_LYRIC_SERVICES_DELETE},
            {IDC_LYRIC_SERVICES_UP, IDS_LYRIC_SERVICES_UP}, {IDC_LYRIC_SERVICES_DOWN, IDS_LYRIC_SERVICES_DOWN}}) {
            TTTOOLINFOW tool{sizeof(tool)};
            tool.hwnd = dialog; tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dialog, button));
            tool.hinst = GetModuleHandleW(nullptr); tool.lpszText = MAKEINTRESOURCEW(text);
            SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        const HWND list = GetDlgItem(dialog, IDC_LYRIC_SERVICES_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        const UINT names[]{IDS_LYRIC_SERVICES_NAME, IDS_LYRIC_SERVICES_URL};
        RECT bounds{}; GetClientRect(list, &bounds);
        // Storage remains in the read-only field below, not a third column.
        // Reserve scrollbar space and give the URL the remaining width.
        const int width = std::max(0L, bounds.right - bounds.left - GetSystemMetrics(SM_CXVSCROLL));
        const int name_width = width * 30 / 100;
        for (int i = 0; i < 2; ++i) {
            auto text = Text(names[i]); LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = text.data(); col.cx = i == 0 ? name_width : width - name_width;
            ListView_InsertColumn(list, i, &col);
        }
        SendDlgItemMessageW(dialog, IDC_LYRIC_SERVICES_NAME, EM_SETLIMITTEXT, 256, 0);
        SendDlgItemMessageW(dialog, IDC_LYRIC_SERVICES_URL, EM_SETLIMITTEXT, 8192, 0);
        self->PopulateLyricServiceEditor(0);
        SetTimer(dialog, 1, 100, nullptr); return TRUE;
    }
    if (!self) return FALSE;
    switch (message) {
    case WM_TIMER: self->PollLyricServices(); return TRUE;
    case WM_CLOSE: {
        if (self->lyric_catalog_save_job_) return TRUE;
        if (self->lyric_service_dirty_ && MessageBoxW(dialog, Text(IDS_LYRIC_SERVICES_DISCARD).c_str(),
            Text(IDS_LYRIC_SERVICES_EDIT).c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
        const HWND owner = self->lyric_service_disabled_owner_;
        self->CloseLyricServiceEditor();
        if (owner && (owner == self->options_window_ || owner == self->lyric_search_dialog_) &&
            IsWindowEnabled(owner)) SetForegroundWindow(owner);
        return TRUE;
    }
    case WM_DESTROY: {
        KillTimer(dialog, 1); self->lyric_service_editor_ = nullptr;
        const HWND owner = std::exchange(self->lyric_service_disabled_owner_, nullptr);
        if (owner && (owner == self->options_window_ || owner == self->lyric_search_dialog_) &&
            IsWindow(owner)) EnableWindow(owner, TRUE);
        return TRUE;
    }
    case WM_NOTIFY: {
        const auto* notify = reinterpret_cast<NMLISTVIEW*>(lp);
        if (notify->hdr.idFrom == IDC_LYRIC_SERVICES_LIST && notify->hdr.code == LVN_ITEMCHANGED &&
            (notify->uChanged & LVIF_STATE) && ((notify->uNewState ^ notify->uOldState) & LVIS_SELECTED) &&
            !self->lyric_service_populating_)
            self->SelectLyricServiceEditorRow(ListView_GetNextItem(notify->hdr.hwndFrom, -1, LVNI_SELECTED));
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp), index = self->lyric_service_selection_;
        if (id == IDCANCEL) { SendMessageW(dialog, WM_CLOSE, 0, 0); return TRUE; }
        if (id == IDOK) { self->SaveLyricServiceEditor(); return TRUE; }
        if (self->lyric_catalog_save_job_ || !self->lyric_services_ready_) return TRUE;
        const bool selected = index >= 0 && static_cast<size_t>(index) < self->lyric_service_draft_.size();
        if (id == IDC_LYRIC_SERVICES_ADD && !self->lyric_editor_baseline_.files.empty()) {
            const auto module = selected ? self->lyric_service_draft_[index].module : self->lyric_editor_baseline_.files.front().module;
            lyrics::LyricService entry{Text(IDS_LYRIC_SERVICES_NEW), L"https://", {}, module,
                lyrics::ServiceIniPath(module), false, 0};
            self->lyric_service_draft_.push_back(std::move(entry)); self->lyric_service_dirty_ = true;
            self->PopulateLyricServiceEditor(static_cast<int>(self->lyric_service_draft_.size()) - 1);
            SetFocus(GetDlgItem(dialog, IDC_LYRIC_SERVICES_NAME));
            SendDlgItemMessageW(dialog, IDC_LYRIC_SERVICES_NAME, EM_SETSEL, 0, -1); return TRUE;
        }
        if (!selected || self->lyric_service_draft_[index].read_only) break;
        if (id == IDC_LYRIC_SERVICES_UP || id == IDC_LYRIC_SERVICES_DOWN) {
            const int direction = id == IDC_LYRIC_SERVICES_UP ? -1 : 1;
            if (lyrics::MoveService(self->lyric_service_draft_, index, direction)) {
                self->lyric_service_dirty_ = true;
                self->PopulateLyricServiceEditor(index + direction);
            }
            return TRUE;
        }
        if (id == IDC_LYRIC_SERVICES_DELETE) {
            self->lyric_service_draft_.erase(self->lyric_service_draft_.begin() + index);
            self->lyric_service_dirty_ = true; self->PopulateLyricServiceEditor(index); return TRUE;
        }
        if ((id == IDC_LYRIC_SERVICES_NAME || id == IDC_LYRIC_SERVICES_URL) &&
            HIWORD(wp) == EN_CHANGE && !self->lyric_service_populating_) {
            auto& entry = self->lyric_service_draft_[index];
            auto& value = id == IDC_LYRIC_SERVICES_NAME ? entry.name : entry.url;
            value = Field(dialog, id); self->lyric_service_dirty_ = true;
            ListView_SetItemText(GetDlgItem(dialog, IDC_LYRIC_SERVICES_LIST), index,
                id == IDC_LYRIC_SERVICES_NAME ? 0 : 1, value.data());
            EnableWindow(GetDlgItem(dialog, IDOK), TRUE); return TRUE;
        }
        break;
    }
    }
    return FALSE;
}
} // namespace ttplayer::ui
