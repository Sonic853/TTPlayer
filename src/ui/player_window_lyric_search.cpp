#include "player_window_internal.h"
#include "ttplayer/core/text.h"

#include <algorithm>
#include <shellapi.h>

namespace ttplayer::ui {
using namespace detail;
namespace {
std::wstring DialogText(HWND dialog, int id) {
    const HWND item = GetDlgItem(dialog, id);
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(item)) + 1, L'\0');
    text.resize(GetWindowTextW(item, text.data(), static_cast<int>(text.size())));
    return text;
}
std::wstring Wide(const std::string& text) {
    try { return core::Utf8ToWide(text); } catch (...) { return {}; }
}
bool SameTrack(const playlist::Track* current, const playlist::Track& request) {
    return current && current->path == request.path && current->subtrack == request.subtrack &&
        current->title == request.title && current->artist == request.artist;
}
bool WebLink(const std::wstring& text) {
    return text.starts_with(L"http://") || text.starts_with(L"https://");
}
}

void PlayerWindow::CloseOnlineLyricSearch() {
    if (lyric_search_dialog_ && IsWindow(lyric_service_editor_) &&
        GetWindow(lyric_service_editor_, GW_OWNER) == lyric_search_dialog_)
        CloseLyricServiceEditor(); // Forced teardown: restore owner before destroying either HWND.
    lyric_services_pending_auto_ = false;
    lyric_search_.reset(); // cancellation only; DLL Release is on its worker
    lyric_download_deadline_ = 0;
    if (lyric_search_dialog_ && IsWindow(lyric_search_dialog_)) DestroyWindow(lyric_search_dialog_);
    lyric_search_dialog_ = nullptr;
}

void PlayerWindow::CancelOnlineLyricCountdown() {
    if (!lyric_download_deadline_) return;
    lyric_download_deadline_ = 0;
    if (lyric_search_dialog_)
        SetDlgItemTextW(lyric_search_dialog_, 1052, ResourceText(0x8184).c_str());
}

void PlayerWindow::ShowOnlineLyricSearch(bool automatic_results) {
    if (!lyric_services_ready_ && !lyric_catalog_job_) RefreshLyricServices();
    if (lyric_search_dialog_ && IsWindow(lyric_search_dialog_)) {
        if (IsWindow(lyric_service_editor_) &&
            GetWindow(lyric_service_editor_, GW_OWNER) == lyric_search_dialog_) {
            ShowLyricServiceEditor();
            return;
        }
        ShowWindow(lyric_search_dialog_, SW_SHOWNORMAL);
        SetForegroundWindow(lyric_search_dialog_);
        return;
    }
    if (!automatic_results) {
        lyric_search_.reset();
        lyric_search_results_shown_ = false;
        lyric_search_saved_ = false;
        lyric_search_automatic_ = false;
        if (const auto* track = PlaybackTrackForUi()) lyric_search_track_ = *track;
        else lyric_search_track_ = {};
        lyric_search_artist_ = Wide(lyric_search_track_.artist);
        lyric_search_title_ = Wide(lyric_search_track_.title);
        if (lyric_search_title_.empty()) lyric_search_title_ = lyric_search_track_.path.stem().wstring();
    }
    // CLrcSearchDlg::0043AB9B selects RT_DIALOG 208 (automatic result
    // choice) or 209 (manual search). Preserve the original template/layout.
    lyric_search_dialog_ = CreateDialogParamW(ResourceModule(),
        MAKEINTRESOURCEW(automatic_results ? 208 : 209), window_,
        OnlineLyricDialogProc, reinterpret_cast<LPARAM>(this));
    if (lyric_search_dialog_) {
        RECT bounds{}, owner{};
        GetWindowRect(lyric_search_dialog_, &bounds);
        GetWindowRect(window_, &owner);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            if (IsRectEmpty(&owner)) owner = monitor.rcWork;
            const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
            const int x = std::clamp((owner.left + owner.right - width) / 2,
                monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
            const int y = std::clamp((owner.top + owner.bottom - height) / 2,
                monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
            SetWindowPos(lyric_search_dialog_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(lyric_search_dialog_, SW_SHOWNORMAL);
        ApplySkinWindowTopMost();
        if (!automatic_results) SetForegroundWindow(lyric_search_dialog_);
    }
}

void PlayerWindow::StartOnlineLyricSearch(bool automatic) {
    if (!sound_library_ || sound_library_->LyricSearchProviders().empty()) return;
    if (!lyric_services_ready_) {
        if (!lyric_catalog_job_) RefreshLyricServices();
        lyric_services_pending_auto_ = automatic;
        return;
    }
    if (lyric_services_.entries.empty()) return;
    const auto* track = PlaybackTrackForUi();
    if (automatic) {
        if (!track || !settings_.lyric.auto_download || lyric_search_dialog_) return;
        const auto key = track->path.wstring() + L"\n" + std::to_wstring(track->subtrack) +
            L"\n" + Wide(track->artist) + L"\n" + Wide(track->title);
        if (lyric_auto_search_key_ == key) return;
        lyric_auto_search_key_ = key;
        lyric_search_track_ = *track;
        lyric_search_artist_ = Wide(track->artist);
        lyric_search_title_ = Wide(track->title);
        if (settings_.lyric.download_when_full_info &&
            (lyric_search_artist_.empty() || lyric_search_title_.empty())) return;
        if (lyric_search_title_.empty()) lyric_search_title_ = track->path.stem().wstring();
    } else {
        if (track) lyric_search_track_ = *track;
        lyric_search_artist_ = DialogText(lyric_search_dialog_, 1021);
        lyric_search_title_ = DialogText(lyric_search_dialog_, 1009);
        const auto index = SendDlgItemMessageW(lyric_search_dialog_, 2090, CB_GETCURSEL, 0, 0);
        if (index >= 0) SelectLyricService(static_cast<int>(index));
    }
    if (lyric_search_title_.empty()) return;
    lyric_search_.reset();
    lyric_search_automatic_ = automatic;
    lyric_search_results_shown_ = false;
    lyric_search_saved_ = false;
    lyric_download_deadline_ = 0;
    lyric_search_revision_ = static_cast<size_t>(-1);
    lyric_download_path_.clear();
    if (lyric_search_dialog_) ListView_DeleteAllItems(GetDlgItem(lyric_search_dialog_, 1064));
    try {
        const auto index = static_cast<size_t>(std::clamp(settings_.lyric.add_in_index, 0,
            static_cast<int>(lyric_services_.entries.size()) - 1));
        const auto& service = lyric_services_.entries[index];
        if (!service.url.empty())
            lyric_search_ = std::make_unique<lyrics::OnlineSearch>(service, index, settings_.network,
                lyric_search_artist_, lyric_search_title_);
        else // Other AddIns keep their own private, non-ttp_lrcsh protocol.
            lyric_search_ = std::make_unique<lyrics::OnlineSearch>(*sound_library_, service.legacy_provider,
                settings_.network, lyric_search_artist_, lyric_search_title_);
    } catch (...) {
        if (lyric_search_dialog_) SetDlgItemTextW(lyric_search_dialog_, 1052, ResourceText(0x817b).c_str());
    }
    PollOnlineLyricSearch();
}

void PlayerWindow::UpdateOnlineLyricSelection() {
    if (!lyric_search_ || !lyric_search_dialog_) return;
    const auto snapshot = lyric_search_->Snapshot();
    const int index = ListView_GetNextItem(GetDlgItem(lyric_search_dialog_, 1064), -1, LVNI_SELECTED);
    if (index < 0 || static_cast<size_t>(index) >= snapshot.results.size()) {
        EnableWindow(GetDlgItem(lyric_search_dialog_, IDOK), FALSE);
        return;
    }
    const auto& result = snapshot.results[index];
    auto filename = settings_.lyric.same_file_title && !lyric_search_track_.path.empty()
        ? lyric_search_track_.path.stem().wstring() : result.artist + L" - " + result.title;
    filename = lyrics::LyricFileName(std::move(filename));
    SetDlgItemTextW(lyric_search_dialog_, 2001, filename.c_str());
    EnableWindow(GetDlgItem(lyric_search_dialog_, IDOK), snapshot.phase == lyrics::SearchPhase::results ||
        snapshot.phase == lyrics::SearchPhase::downloaded);
}

void PlayerWindow::DownloadOnlineLyric(int index) {
    if (!lyric_search_) return;
    const auto snapshot = lyric_search_->Snapshot();
    if ((snapshot.phase != lyrics::SearchPhase::results && snapshot.phase != lyrics::SearchPhase::downloaded) || index < 0 ||
        static_cast<size_t>(index) >= snapshot.results.size()) return;
    lyric_download_deadline_ = 0;
    const auto& item = snapshot.results[index];
    auto filename = lyric_search_dialog_ ? DialogText(lyric_search_dialog_, 2001) :
        (settings_.lyric.same_file_title && !lyric_search_track_.path.empty()
            ? lyric_search_track_.path.stem().wstring() : item.artist + L" - " + item.title);
    if (filename.empty()) return;
    lyric_download_path_ = lyrics::DownloadDirectory(settings_.lyric, lyric_search_track_.path,
        PlayerRuntimeDirectory()) / lyrics::LyricFileName(std::move(filename));
    lyric_download_overwrite_ = settings_.lyric.overwrite;
    std::error_code error;
    if (!lyric_download_overwrite_ && std::filesystem::exists(lyric_download_path_, error)) {
        if (!lyric_search_dialog_) {
            // Original automatic path accepts the already-present lyric;
            // do not leave an idle DLL session (or overwrite it silently).
            LoadLyricsFrom(lyric_download_path_, settings_.lyric.auto_associate);
            CloseOnlineLyricSearch();
            return;
        }
        std::wstring prompt = ResourceText(0x814d);
        if (const auto at = prompt.find(L"%s"); at != prompt.npos)
            prompt.replace(at, 2, lyric_download_path_.wstring());
        if (MessageBoxW(lyric_search_dialog_, prompt.c_str(), ResourceText(0x80).c_str(),
            MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        lyric_download_overwrite_ = true;
    }
    lyric_download_associate_ = lyric_search_dialog_
        ? IsDlgButtonChecked(lyric_search_dialog_, 2064) == BST_CHECKED : settings_.lyric.auto_associate;
    settings_.lyric.auto_associate = lyric_download_associate_;
    lyric_search_saved_ = false;
    lyric_search_->Download(index);
    PollOnlineLyricSearch();
}

void PlayerWindow::PollOnlineLyricSearch() {
    if (!lyric_search_) return;
    // Network work remains asynchronous, but do not auto-download/close the
    // modal owner (and discard its editor) while the user edits the service list.
    if (lyric_search_dialog_ && IsWindow(lyric_service_editor_) &&
        GetWindow(lyric_service_editor_, GW_OWNER) == lyric_search_dialog_) return;
    if (!lyric_search_track_.path.empty() && !SameTrack(PlaybackTrackForUi(), lyric_search_track_)) {
        CloseOnlineLyricSearch(); return;
    }
    // A local association/editor opened while a background search was in
    // flight takes precedence over its late network response.
    if (lyric_search_automatic_ && (!lyric_path_.empty() || lyric_editor_ ||
        !settings_.lyric.auto_download)) { CloseOnlineLyricSearch(); return; }
    auto snapshot = lyric_search_->Snapshot();
    if (lyric_download_deadline_ && GetTickCount64() >= lyric_download_deadline_) {
        DownloadOnlineLyric(ListView_GetNextItem(GetDlgItem(lyric_search_dialog_, 1064), -1, LVNI_SELECTED));
        return;
    }
    const auto update_countdown = [&] {
        if (!lyric_download_deadline_ || !lyric_search_dialog_) return;
        auto text = lyric_download_countdown_format_;
        const auto now = GetTickCount64();
        const auto seconds = now < lyric_download_deadline_ ? (lyric_download_deadline_ - now + 999) / 1000 : 0;
        if (const auto at = text.find(L"%d"); at != text.npos) text.replace(at, 2, std::to_wstring(seconds));
        if (DialogText(lyric_search_dialog_, 1052) != text)
            SetDlgItemTextW(lyric_search_dialog_, 1052, text.c_str());
    };
    if (snapshot.revision == lyric_search_revision_) { update_countdown(); return; }
    lyric_search_revision_ = snapshot.revision;
    using Phase = lyrics::SearchPhase;
    if (snapshot.phase == Phase::results && !lyric_search_results_shown_) {
        lyric_search_results_shown_ = true;
        const auto best = lyrics::BestSearchResult(snapshot.results, lyric_search_artist_, lyric_search_title_);
        if (lyric_search_automatic_ && !snapshot.results.empty()) {
            // 0044BEED skips the choice dialog for a single result even
            // when AutoSelectDownload is unchecked.
            if (snapshot.results.size() == 1 || settings_.lyric.auto_select_download) {
                DownloadOnlineLyric(static_cast<int>(best)); return;
            }
            ShowOnlineLyricSearch(true);
            if (lyric_search_dialog_) lyric_download_deadline_ = GetTickCount64() + 15000;
        }
        if (lyric_search_dialog_) {
            const HWND list = GetDlgItem(lyric_search_dialog_, 1064);
            for (size_t index = 0; index < snapshot.results.size(); ++index) {
                LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(index);
                item.pszText = snapshot.results[index].artist.data();
                ListView_InsertItem(list, &item);
                ListView_SetItemText(list, item.iItem, 1, snapshot.results[index].title.data());
            }
            if (!snapshot.results.empty()) {
                ListView_SetItemState(list, static_cast<int>(best), LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(list, static_cast<int>(best), FALSE);
                UpdateOnlineLyricSelection();
                SendMessageW(lyric_search_dialog_, DM_SETDEFID, IDOK, 0);
            }
        }
    }
    UINT status = 0x817a;
    if (snapshot.phase == Phase::results) status = snapshot.results.empty() ? 0x817b : 0x8184;
    else if (snapshot.phase == Phase::downloading) status = 0x8179;
    else if (snapshot.phase == Phase::failed) status = 0x817b;
    else if (snapshot.phase == Phase::downloaded) {
        status = 0x8186;
        if (!lyric_search_saved_) {
            lyric_search_saved_ = true;
            if (lyric_download_path_.empty() || !lyrics::SaveDownloadedLyric(
                lyric_download_path_, snapshot.text, lyric_download_overwrite_)) status = 0x8182;
            else {
                LoadLyricsFrom(lyric_download_path_, lyric_download_associate_);
                UpdateDiscordPresence();
                if (lyric_search_automatic_) { CloseOnlineLyricSearch(); return; }
            }
        }
    }
    if (lyric_search_dialog_) {
        auto text = snapshot.phase == Phase::failed && !snapshot.error.empty() ? snapshot.error : ResourceText(status);
        if (const auto at = text.find(L"%s"); at != text.npos) text.replace(at, 2, lyric_download_path_.wstring());
        SetDlgItemTextW(lyric_search_dialog_, 1052, text.c_str());
        const bool busy = snapshot.phase == Phase::searching || snapshot.phase == Phase::downloading;
        EnableWindow(GetDlgItem(lyric_search_dialog_, 1046), !busy);
        EnableWindow(GetDlgItem(lyric_search_dialog_, 2090), !busy);
        EnableWindow(GetDlgItem(lyric_search_dialog_, IDOK),
            (snapshot.phase == Phase::results || snapshot.phase == Phase::downloaded) && !snapshot.results.empty());
        if (!snapshot.extra_title.empty()) SetDlgItemTextW(lyric_search_dialog_, 2269, snapshot.extra_title.c_str());
        ShowWindow(GetDlgItem(lyric_search_dialog_, 2269), WebLink(snapshot.extra_url) ? SW_SHOW : SW_HIDE);
        update_countdown();
    }
}

INT_PTR CALLBACK PlayerWindow::OnlineLyricDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<PlayerWindow*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        self->lyric_search_dialog_ = dialog;
        self->lyric_download_countdown_format_ = DialogText(dialog, 1052);
        SetDlgItemTextW(dialog, 1021, self->lyric_search_artist_.c_str());
        SetDlgItemTextW(dialog, 1009, self->lyric_search_title_.c_str());
        CheckDlgButton(dialog, 2064, self->settings_.lyric.auto_associate ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, 2067, self->settings_.lyric.auto_select_download ? BST_CHECKED : BST_UNCHECKED);
        self->PopulateLyricServices(dialog);
        self->InstallLyricServiceEditorButton(dialog);
        EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
        ShowWindow(GetDlgItem(dialog, 2269), SW_HIDE);
        for (const int id : {2269}) {
            const HWND link = GetDlgItem(dialog, id);
            if (link) SetWindowLongPtrW(link, GWL_STYLE, GetWindowLongPtrW(link, GWL_STYLE) | SS_NOTIFY);
        }
        const HWND list = GetDlgItem(dialog, 1064);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        for (int column = 0; column < 2; ++column) {
            // 0043BE2C splits string 0x8157 at '|'.
            auto headers = self->ResourceText(0x8157);
            const auto split = headers.find(L'|');
            auto label = split == headers.npos ? std::wstring{} :
                (column == 0 ? headers.substr(0, split) : headers.substr(split + 1));
            LVCOLUMNW item{}; item.mask = LVCF_TEXT | LVCF_WIDTH;
            item.cx = column ? 200 : 140; item.pszText = label.data();
            ListView_InsertColumn(list, column, &item);
        }
        SetTimer(dialog, 1, 100, nullptr);
        return TRUE;
    }
    if (!self) return FALSE;
    if (dialog == self->lyric_service_disabled_owner_ && IsWindow(self->lyric_service_editor_) &&
        (message == WM_COMMAND || message == WM_CLOSE || message == WM_NOTIFY ||
         (message == WM_SYSCOMMAND && (wparam & 0xfff0U) == SC_CLOSE))) {
        self->ShowLyricServiceEditor();
        return TRUE; // Also reject input commands queued before disabling the owner.
    }
    switch (message) {
    case WM_CLOSE: self->CloseOnlineLyricSearch(); return TRUE;
    case WM_DESTROY:
        if (self->lyric_service_disabled_owner_ == dialog) self->lyric_service_disabled_owner_ = nullptr;
        KillTimer(dialog, 1); self->lyric_search_dialog_ = nullptr;
        self->lyric_download_deadline_ = 0; return TRUE;
    case WM_TIMER: self->PollLyricServices(); self->PollOnlineLyricSearch(); return TRUE;
    case WM_LBUTTONDOWN: case WM_NCLBUTTONDOWN:
        self->CancelOnlineLyricCountdown(); break;
    case WM_NOTIFY: {
        const auto* notify = reinterpret_cast<NMHDR*>(lparam);
        if (notify->idFrom == 1064) {
            if (notify->code == NM_CLICK || notify->code == NM_DBLCLK || notify->code == LVN_KEYDOWN)
                self->CancelOnlineLyricCountdown();
            if (notify->code == LVN_ITEMCHANGED) self->UpdateOnlineLyricSelection();
            if (notify->code == NM_DBLCLK)
                self->DownloadOnlineLyric(ListView_GetNextItem(notify->hwndFrom, -1, LVNI_SELECTED));
        }
        break;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED || HIWORD(wparam) == EN_SETFOCUS)
            self->CancelOnlineLyricCountdown();
        switch (LOWORD(wparam)) {
        case IDCANCEL: self->CloseOnlineLyricSearch(); return TRUE;
        case IDOK:
            self->DownloadOnlineLyric(ListView_GetNextItem(GetDlgItem(dialog, 1064), -1, LVNI_SELECTED)); return TRUE;
        case 1046: self->StartOnlineLyricSearch(false); return TRUE;
        case 1009:
            if (HIWORD(wparam) == EN_SETFOCUS)
                SendMessageW(dialog, DM_SETDEFID, 1046, 0);
            return TRUE;
        case 2090:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                const auto index = SendDlgItemMessageW(dialog, 2090, CB_GETCURSEL, 0, 0);
                if (index >= 0) self->SelectLyricService(static_cast<int>(index));
            }
            return TRUE;
        case 2067:
            self->settings_.lyric.auto_select_download = IsDlgButtonChecked(dialog, 2067) == BST_CHECKED; return TRUE;
        case 2064:
            self->settings_.lyric.auto_associate = IsDlgButtonChecked(dialog, 2064) == BST_CHECKED; return TRUE;
        case 2185: self->ShowLyricServiceEditor(dialog); return TRUE;
        case 2269:
            if (self->lyric_search_) {
                const auto snapshot = self->lyric_search_->Snapshot();
                if (WebLink(snapshot.extra_url)) ShellExecuteW(dialog, L"open", snapshot.extra_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return TRUE;
        }
        break;
    }
    return FALSE;
}
} // namespace ttplayer::ui
