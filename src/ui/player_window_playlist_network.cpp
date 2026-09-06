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
constexpr UINT_PTR kReportCloseTimer = 1;

struct ReportDialogState {
    std::wstring track;
    std::wstring submitted_text;
    int reason{1};
    bool submitted{};
};

INT_PTR CALLBACK ReportDialogProc(HWND dialog, UINT message,
                                  WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ReportDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        state = reinterpret_cast<ReportDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        if (!state) return FALSE;
        SetDlgItemTextW(dialog, kReportTrack, state->track.c_str());
        CheckRadioButton(dialog, kReportUnavailable, kReportWrongSong,
                         kReportUnavailable);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kReportUnavailable:
        case kReportWrongSong:
            if (state && !state->submitted) {
                state->reason = LOWORD(wparam) == kReportUnavailable ? 1 : 2;
                CheckRadioButton(dialog, kReportUnavailable, kReportWrongSong,
                                 LOWORD(wparam));
            }
            return TRUE;
        case IDOK:
            if (!state || state->submitted) return TRUE;
            state->reason = IsDlgButtonChecked(dialog, kReportWrongSong) ==
                                    BST_CHECKED
                                ? 2
                                : 1;
            state->submitted = true;
            SetDlgItemTextW(dialog, kReportStatus,
                            state->submitted_text.c_str());
            EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
            EnableWindow(GetDlgItem(dialog, kReportUnavailable), FALSE);
            EnableWindow(GetDlgItem(dialog, kReportWrongSong), FALSE);
            InvalidateRect(GetDlgItem(dialog, kReportStatus), nullptr, TRUE);
            SetTimer(dialog, kReportCloseTimer, 3000, nullptr);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_CTLCOLORSTATIC:
        if (state && state->submitted &&
            reinterpret_cast<HWND>(lparam) ==
                GetDlgItem(dialog, kReportStatus)) {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            // FUN_0047DED9 draws the acknowledgement using COLORREF 0xff.
            SetTextColor(dc, RGB(255, 0, 0));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetStockObject(NULL_BRUSH));
        }
        break;

    case WM_TIMER:
        if (wparam == kReportCloseTimer) {
            KillTimer(dialog, kReportCloseTimer);
            EndDialog(dialog, IDYES); // 0047DBC9 returns the native value 6.
            return TRUE;
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
        state.submitted_text = ResourceText(0x84d0);
        const INT_PTR result = DialogBoxParamW(
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
