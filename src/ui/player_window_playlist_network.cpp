#include "disc_lookup_dialog.h"
#include "ttplayer/core/text.h"
#include "ttplayer/i18n/i18n.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include "ttplayer/ui/player_window.h"

#include "player_window_internal.h"
#include "ttplayer/ui/playlist_network_commands.h"
#include "../app/resource_ids.h"
#include "modern_file_dialog.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
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

struct DownloadsDialogState {
    audio::HttpDownloadQueue* queue;
    std::filesystem::path folder;
    bool artist_folder{},replace{};
    std::filesystem::path reported_recovery;
};
uint64_t SelectedDownloadId(HWND list) {
    LVITEMW item{};
    item.mask=LVIF_PARAM;
    item.iItem=ListView_GetNextItem(list,-1,LVNI_SELECTED);
    return item.iItem>=0 && ListView_GetItem(list,&item)
        ? static_cast<uint64_t>(item.lParam) : 0;
}
INT_PTR CALLBACK DownloadsProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state=reinterpret_cast<DownloadsDialogState*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG) {
        SetWindowLongPtrW(dialog,DWLP_USER,lparam);
        const auto list=GetDlgItem(dialog,IDC_HTTP_DOWNLOAD_LIST);
        ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        const wchar_t* names[]={L"歌曲",L"状态",L"已下载 / 总大小",L"保存位置"};
        const int widths[]={150,115,140,260};
        for(int i=0;i<4;++i){LVCOLUMNW c{};c.mask=LVCF_TEXT|LVCF_WIDTH;c.pszText=const_cast<wchar_t*>(names[i]);c.cx=widths[i];ListView_InsertColumn(list,i,&c);}
        SetTimer(dialog,1,500,nullptr);SendMessageW(dialog,WM_TIMER,1,0);return TRUE;
    }
    if(!state) return FALSE;
    const auto queue=state->queue;
    if(message==WM_TIMER) {
        const auto list=GetDlgItem(dialog,IDC_HTTP_DOWNLOAD_LIST);const int selected=ListView_GetNextItem(list,-1,LVNI_SELECTED);
        LPARAM id{};if(selected>=0){LVITEMW item{};item.mask=LVIF_PARAM;item.iItem=selected;ListView_GetItem(list,&item);id=item.lParam;}
        SendMessageW(list,WM_SETREDRAW,FALSE,0);ListView_DeleteAllItems(list);
        for(const auto& job:queue->Snapshot()) {
            if(job.prefetch) continue;
            const auto title=job.display_title.empty()?job.entry.sourceurl:job.display_title;
            LVITEMW item{};item.mask=LVIF_TEXT|LVIF_PARAM;item.iItem=ListView_GetItemCount(list);
            item.pszText=const_cast<wchar_t*>(title.c_str());item.lParam=static_cast<LPARAM>(job.id);
            const auto row=ListView_InsertItem(list,&item);
            const wchar_t* statuses[]={L"等待",L"下载中",L"完成",L"已取消",L"失败",L"已停止"};
            std::wstring status=statuses[static_cast<int>(job.state)];
            if(job.state==audio::DownloadState::failed){wchar_t code[32];swprintf_s(code,L" (0x%08X)",static_cast<unsigned>(job.result));status+=code;}
            auto bytes=std::to_wstring(job.received/1024)+L" / "+std::to_wstring(job.total/1024)+L" KiB";
            auto path=job.destination.wstring();
            ListView_SetItemText(list,row,1,status.data());ListView_SetItemText(list,row,2,bytes.data());ListView_SetItemText(list,row,3,path.data());
            if(item.lParam==id) ListView_SetItemState(list,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        }
        SendMessageW(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,nullptr,FALSE);
        SetDlgItemTextW(dialog,IDC_HTTP_DOWNLOAD_STATUS,FAILED(queue->StorageResult())?
            L"下载列表无法保存，或正由另一个播放器使用。":L"关闭窗口后继续下载。右键可移除记录，保留已下载文件。");
        const auto backup=queue->RecoveryBackupPath();
        if(!backup.empty() && backup!=state->reported_recovery) {
            state->reported_recovery=backup;
            const auto notice=L"下载列表替换失败，原文件备份保留于：\n"+backup.wstring();
            MessageBoxW(dialog,notice.c_str(),L"下载列表保存失败",MB_OK|MB_ICONERROR);
        }
        return TRUE;
    }
    if(message==WM_CONTEXTMENU &&
        reinterpret_cast<HWND>(wparam)==GetDlgItem(dialog,IDC_HTTP_DOWNLOAD_LIST)) {
        const auto list=GetDlgItem(dialog,IDC_HTTP_DOWNLOAD_LIST);
        POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        if(point.x==-1 && point.y==-1) {
            RECT rect{};
            const auto selected=ListView_GetNextItem(list,-1,LVNI_SELECTED);
            if(selected<0 || !ListView_GetItemRect(list,selected,&rect,LVIR_BOUNDS))
                GetClientRect(list,&rect);
            point={rect.left+8,rect.top+8};ClientToScreen(list,&point);
        } else {
            LVHITTESTINFO hit{};hit.pt=point;ScreenToClient(list,&hit.pt);
            const auto row=ListView_HitTest(list,&hit);
            ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
            if(row>=0) ListView_SetItemState(list,row,LVIS_SELECTED|LVIS_FOCUSED,
                                            LVIS_SELECTED|LVIS_FOCUSED);
        }
        const auto id=SelectedDownloadId(list);
        const auto jobs=queue->Snapshot();
        const bool history=std::any_of(jobs.begin(),jobs.end(),[](const auto& job) {
            return !job.prefetch && (job.state==audio::DownloadState::complete ||
                job.state==audio::DownloadState::canceled || job.state==audio::DownloadState::failed);
        });
        const auto menu=CreatePopupMenu();
        if(!menu) return TRUE;
        AppendMenuW(menu,MF_STRING|(id?MF_ENABLED:MF_GRAYED),1,L"移除记录（保留文件）");
        AppendMenuW(menu,MF_STRING|(history?MF_ENABLED:MF_GRAYED),2,L"清理已结束记录（保留文件）");
        const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,
                                         point.x,point.y,0,dialog,nullptr);
        DestroyMenu(menu);
        if(command==1) queue->Remove(id);
        else if(command==2) queue->ClearHistory();
        SendMessageW(dialog,WM_TIMER,1,0);
        return TRUE;
    }
    if(message==WM_NOTIFY) {
        const auto* header=reinterpret_cast<const NMHDR*>(lparam);
        if(header && header->idFrom==IDC_HTTP_DOWNLOAD_LIST && header->code==LVN_KEYDOWN) {
            const auto* key=reinterpret_cast<const NMLVKEYDOWN*>(lparam);
            if(key->wVKey==VK_DELETE) {
                queue->Remove(SelectedDownloadId(header->hwndFrom));
                SendMessageW(dialog,WM_TIMER,1,0);
                return TRUE;
            }
        }
    }
    if(message==WM_COMMAND) {
        if(LOWORD(wparam)==IDCANCEL){EndDialog(dialog,IDCANCEL);return TRUE;}
        if(LOWORD(wparam)==IDC_HTTP_DOWNLOAD_IMPORT) {
            ModernOpenFileOptions options;options.owner=dialog;options.title=L"导入原版缓存索引或下载队列";
            options.filters={{L"XML 列表",L"*.xml"},{L"所有文件",L"*.*"}};
            if(const auto selected=ModernOpenFile(options)) {
                const auto count=queue->ImportLegacyList(*selected,state->folder,state->artist_folder,state->replace);
                const auto notice=count?L"已导入 "+std::to_wstring(count)+L" 个任务。未完成项已停止，可选择后点击“重新下载”。":L"没有可导入的任务：列表可能已存在、格式不兼容或来源不是 HTTP/HTTPS。";
                MessageBoxW(dialog,notice.c_str(),L"导入旧列表",MB_OK|MB_ICONINFORMATION);
            }return TRUE;
        }
        const auto list=GetDlgItem(dialog,IDC_HTTP_DOWNLOAD_LIST);LVITEMW item{};item.mask=LVIF_PARAM;
        item.iItem=ListView_GetNextItem(list,-1,LVNI_SELECTED);
        if(item.iItem>=0 && ListView_GetItem(list,&item)) {
            if(LOWORD(wparam)==IDC_HTTP_DOWNLOAD_CANCEL) queue->Cancel(item.lParam);
            if(LOWORD(wparam)==IDC_HTTP_DOWNLOAD_RETRY) queue->Retry(item.lParam);
        }
        SendMessageW(dialog,WM_TIMER,1,0);return TRUE;
    }
    if(message==WM_CLOSE){EndDialog(dialog,IDCANCEL);return TRUE;}
    if(message==WM_DESTROY) KillTimer(dialog,1);
    return FALSE;
}

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

void PlayerWindow::ConfigureNetworkDownloads() {
    if(!network_downloads_) {
        const auto root=settings_.source_path.empty()?PlayerRuntimeDirectory():settings_.source_path.parent_path();
        network_downloads_=std::make_unique<audio::HttpDownloadQueue>(root/L"HttpDownloads"/L"cachelist.xml");
    }
    const auto options = PlaybackOptionsFromSettings(settings_, PlayerRuntimeDirectory());
    network_downloads_->Configure(options,settings_.network.max_download_tasks);
}
void PlayerWindow::QueueNetworkDownload(const playlist::Track& track) {
    const auto url=audio::HttpDownloadKey(track.path.wstring());if(url.empty() || settings_.network.download_folder.empty()) return;
    const auto title=core::Utf8ToWide(track.title),artist=core::Utf8ToWide(track.artist);
    auto destination=audio::HttpDownloadDestination(settings_.network.download_folder,url,title,artist,settings_.network.create_folder_by_artist);
    network_downloads_->Enqueue(url,title,std::move(destination),settings_.network.replace_file,artist);
}
void PlayerWindow::ShowNetworkDownloads() {
    if(network_download_dialog_open_) return;
    network_download_dialog_open_=true;
    DownloadsDialogState state{network_downloads_.get(),settings_.network.download_folder,
        settings_.network.create_folder_by_artist,settings_.network.replace_file};
    ShowWtlModalDialog(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_HTTP_DOWNLOADS),
        playlist_window_?playlist_window_:window_,DownloadsProc,reinterpret_cast<LPARAM>(&state));
    network_download_dialog_open_=false;
}
void PlayerWindow::PollNetworkDownloads() {
    ConfigureNetworkDownloads();
    const auto state=audio_->State();
    const bool active=state==audio::PlaybackState::opening || state==audio::PlaybackState::playing || state==audio::PlaybackState::paused;
    const auto url=active && opened_track_?audio::HttpDownloadKey(opened_track_->path.wstring()):std::wstring{};
    if(settings_.network.download_when_listen && !url.empty() && url!=network_auto_download_url_ && playback_source_open_) {
        QueueNetworkDownload(*opened_track_);network_auto_download_url_=url;
    }
    if(!settings_.network.download_when_listen || url.empty()) network_auto_download_url_.clear();
    std::wstring next;
    // 00460904: modes 0/1 have no successor; sequential modes do not wrap this
    // speculative request at the last row. Random preview must not advance it.
    if(active && playback_source_open_ && settings_.network.cache_enabled &&
       network_prefetch_after_ && GetTickCount64()>=network_prefetch_after_ && HasPlaybackTrack()) {
        const auto& tracks=PlaybackPlaylist().Tracks();std::optional<size_t> row;
        if((settings_.player.play_mode==2 || settings_.player.play_mode==3) && *current_+1<tracks.size()) row=*current_+1;
        else if(settings_.player.play_mode==4) {
            PrepareRandomPlaybackOrder();const auto preview=random_playback_order_.TrySelect(
                NavigationTrackCount(),NavigationPlayingRow(),playlist::RandomPlaybackOrder::Request::preview_next);
            if(!preview.pending) row=preview.row;
        }
        if(row && *row<tracks.size() && *row!=*current_) next=audio::HttpDownloadKey(tracks[*row].path.wstring());
    }
    network_downloads_->Prefetch(next);
}

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

    if(action==LegacyPlaylistNetworkAction::download) {
        ConfigureNetworkDownloads();
        if(playlist_selected_rows_.empty()) QueueNetworkDownload(*track);
        else for(auto selected:playlist_selected_rows_) if(const auto* item=VisiblePlaylistTrack(selected)) QueueNetworkDownload(*item);
        ShowNetworkDownloads();return true;
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
