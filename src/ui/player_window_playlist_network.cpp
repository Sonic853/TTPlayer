#include "disc_lookup_dialog.h"
#include "ttplayer/core/text.h"
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

void PlayerWindow::QueryDiscInformation(const std::filesystem::path& path, bool automatic) {
    if (disc_query_active_) return;
    disc_query_active_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{disc_query_active_};
    const auto source = path; // Modal dispatch can change the playlist.
    const auto result = ShowDiscLookupDialog(playlist_window_ ? playlist_window_ : window_,
        source, sound_library_, ttpcomm_module_, settings_.network, automatic);
    if (!result) return;
    const bool cue = _wcsicmp(source.extension().c_str(), L".cue") == 0;
    for (size_t index = 0; index < playlists_.Size(); ++index) {
        auto& list = playlists_.At(index); bool changed{};
        for (size_t row = 0; row < list.Tracks().size(); ++row) {
            auto track = list.Tracks()[row]; unsigned number{};
            if (cue) {
                if (_wcsicmp(track.path.c_str(), source.c_str())) continue;
                number = track.subtrack;
            } else {
                if (_wcsicmp(track.path.extension().c_str(), L".cda") ||
                    _wcsicmp(track.path.root_name().c_str(), source.root_name().c_str())) continue;
                try { number = audio::CdaTrackNumber(track.path); } catch (...) { continue; }
            }
            const auto found = result->tracks.find(number); if (found == result->tracks.end()) continue;
            for (const auto& [name,value] : found->second) {
                const auto key = core::WideToUtf8(name), text = core::WideToUtf8(value);
                auto field = std::find_if(track.metadata.begin(), track.metadata.end(),
                    [&](const auto& f) { return _stricmp(f.first.c_str(), key.c_str()) == 0; });
                if (field == track.metadata.end()) track.metadata.emplace_back(key, text); else field->second = text;
                if (!_wcsicmp(name.c_str(), L"Title")) track.title = text;
                if (!_wcsicmp(name.c_str(), L"Artist")) track.artist = text;
                if (!_wcsicmp(name.c_str(), L"Album")) track.album = text;
            }
            changed |= list.SetTrack(row, std::move(track));
        }
        if (changed) playlists_.MarkDirty(index);
    }
    RefreshPlaylist(); RefreshTrackInformation();
}
void PlayerWindow::QueueAutomaticDiscQuery(const playlist::Track& track) {
    if (!settings_.network.freedb_auto_query || disc_query_active_ || pending_disc_query_ ||
        _wcsicmp(track.path.extension().c_str(), L".cda") || !track.artist.empty() || !track.album.empty() ||
        GetDriveTypeW(track.path.root_path().c_str()) != DRIVE_CDROM) return;
    // Per insertion/session suppression; the complete TOC is read on the worker.
    auto key = track.path.root_name().wstring();
    for (const auto& [name,value] : track.metadata) if (!_stricmp(name.c_str(), "CDDBSerialNumber")) key += core::Utf8ToWide(value);
    if (!queried_discs_.insert(key).second) return;
    pending_disc_query_ = track.path;
    PostMessageW(window_, kMsgDiscLookup, 0, 0);
}
void PlayerWindow::RunAutomaticDiscQuery() {
    if (!pending_disc_query_) return;
    const auto path = *pending_disc_query_; pending_disc_query_.reset();
    if (settings_.network.freedb_auto_query) QueryDiscInformation(path, true);
}

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

    if (action == LegacyPlaylistNetworkAction::freedb) {
        QueryDiscInformation(track->path);
        return true;
    }

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
