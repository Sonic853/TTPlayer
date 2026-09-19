#include "ttplayer/i18n/i18n.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include "ttplayer/ui/player_window.h"

#include "player_window_internal.h"
#include "ttplayer/ui/playlist_network_commands.h"

#include <windows.h>

#include <string>

namespace ttplayer::ui {
using namespace detail;

namespace {

constexpr UINT kReportDialog = 396;
constexpr int kReportUnavailable = 1170;
constexpr int kReportWrongSong = 1171;
constexpr int kReportStatus = 1172;
constexpr int kReportTrack = 2276;
struct ReportDialogState {
    std::wstring track;
};

INT_PTR CALLBACK ReportDialogProc(HWND dialog, UINT message,
                                  WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_INITDIALOG: {
        const auto* state = reinterpret_cast<const ReportDialogState*>(lparam);
        if (!state) return FALSE;
        SetDlgItemTextW(dialog, kReportTrack, state->track.c_str());
        CheckRadioButton(dialog, kReportUnavailable, kReportWrongSong, kReportUnavailable);
        // The resource's 0x84D0 acknowledgement is only valid AFTER an
        // actual submission. The optional reporting backend is absent.
        SetDlgItemTextW(dialog, kReportStatus, i18n::Literal(L"报告服务不可用，尚未提交。"));
        for (int control : {IDOK, kReportUnavailable, kReportWrongSong})
            EnableWindow(GetDlgItem(dialog, control), FALSE);
        SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
        SetFocus(GetDlgItem(dialog, IDCANCEL));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL) EndDialog(dialog, IDCANCEL);
        return TRUE; // Directly dispatched IDOK must not simulate success.
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lparam) == GetDlgItem(dialog, kReportStatus)) {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, RGB(255, 0, 0));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetStockObject(NULL_BRUSH));
        }
        break;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

} // namespace

bool PlayerWindow::HandleLegacyPlaylistNetworkCommand(UINT command) {
    LegacyPlaylistNetworkAction action{};
    switch (command) {
    case kPlaylistFreeDb:
        action = LegacyPlaylistNetworkAction::freedb;
        break;
    case kPlaylistDownload:
        action = LegacyPlaylistNetworkAction::download;
        break;
    case kPlaylistReportOnline:
        action = LegacyPlaylistNetworkAction::report;
        break;
    default:
        return false;
    }

    // All three original handlers resolve the focused Files row first.  A
    // stale command with no valid row is a consumed no-op.
    std::optional<size_t> row = playlist_selection_;
    if (!row && !playlist_selected_rows_.empty())
        row = *playlist_selected_rows_.begin();
    const auto* track = row ? VisiblePlaylistTrack(*row) : nullptr;
    if (!track) return true;

    if (!LegacyNetworkActionAcceptsTrack(action, *track)) return true;

    if (action == LegacyPlaylistNetworkAction::report) {
        ReportDialogState state;
        state.track = BuildLegacyReportTrackText(
            *track, ResourceText(0x84d1));
        const INT_PTR result = ShowWtlModalDialog(
            ResourceModule(), MAKEINTRESOURCEW(kReportDialog),
            playlist_window_ ? playlist_window_ : window_, ReportDialogProc,
            reinterpret_cast<LPARAM>(&state));
        // Cancel is a consumed no-op in FUN_004878A4.  A missing/corrupt
        // dialog resource falls through to the explicit backend notice.
        if (result != IDYES && result != -1) return true;
    }

    // TTPlayer delegated these operations to a FreeDB HTTP worker or its
    // optional Music Window/download-manager module.  Neither private
    // service is present in the rebuild.  Never wait on a retired endpoint
    // or claim success; expose the executable's resource-backed failure.
    const auto notice = LegacyBackendUnavailableNotice(action);
    auto caption = ResourceText(notice.caption_resource);
    auto message = ResourceText(notice.message_resource);
    if (caption.empty()) caption = ResourceText(0x80);
    if (message.empty()) message = ResourceText(0x828e);
    MessageBoxW(playlist_window_ ? playlist_window_ : window_,
        message.c_str(), caption.c_str(),
        MB_OK | (action == LegacyPlaylistNetworkAction::freedb
            ? MB_ICONEXCLAMATION : MB_ICONINFORMATION));
    return true;
}

} // namespace ttplayer::ui
