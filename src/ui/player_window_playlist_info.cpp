#include "player_window_internal.h"
#include "file_info_probe_client.h"

#include "ttplayer/core/text.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <mutex>
#include <new>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace ttplayer::ui {
namespace {

enum class PlaylistInfoState { success, failed, unavailable };

struct PlaylistInfoResult {
    size_t playlist_slot{};
    size_t row_hint{};
    std::filesystem::path path;
    int subtrack{};
    PlaylistInfoState state{PlaylistInfoState::failed};
    int duration_ms{-2};
    std::string title;
    std::string artist;
    std::string album;
    std::string media_type;
    std::uint32_t bitrate_bps{};
    std::uint32_t sample_rate_hz{};
    std::vector<std::pair<std::string, std::string>> metadata;
};

bool LooksLikeNetworkPath(std::wstring_view value) {
    const size_t separator = value.find(L"://");
    return separator != std::wstring_view::npos && separator != 0;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

std::wstring SourceKey(const std::filesystem::path& path, int subtrack) {
    std::wstring key = Lower(path.native());
    key.push_back(L'\x1f');
    key += std::to_wstring(subtrack);
    return key;
}

std::wstring RequestKey(size_t playlist_slot,
                        const std::filesystem::path& path, int subtrack) {
    std::wstring key = std::to_wstring(playlist_slot);
    key.push_back(L'\x1e');
    key += SourceKey(path, subtrack);
    return key;
}

bool IsSameSource(const playlist::Track& track,
                  const std::filesystem::path& path, int subtrack) {
    return track.subtrack == subtrack &&
        SourceKey(track.path, track.subtrack) == SourceKey(path, subtrack);
}

std::optional<size_t> FindUnreadSourceRow(
    const std::vector<playlist::Track>& tracks,
    const std::filesystem::path& path, int subtrack, size_t row_hint) {
    const auto matches = [&](size_t row) {
        return row < tracks.size() && tracks[row].duration_ms == -2 &&
            IsSameSource(tracks[row], path, subtrack);
    };
    if (matches(row_hint)) return row_hint;
    for (size_t row = 0; row < tracks.size(); ++row) {
        if (matches(row)) return row;
    }
    return std::nullopt;
}

bool EqualsAsciiInsensitive(std::wstring_view left,
                            std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        wchar_t lhs = left[index];
        wchar_t rhs = right[index];
        if (lhs >= L'A' && lhs <= L'Z') lhs += L'a' - L'A';
        if (rhs >= L'A' && rhs <= L'Z') rhs += L'a' - L'A';
        if (lhs != rhs) return false;
    }
    return true;
}

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    try { return core::WideToUtf8(value); }
    catch (const std::exception&) { return {}; }
}

std::wstring MetadataValue(
    const detail::FileInfoProbeReadResult& probe, std::wstring_view name) {
    const auto found = std::find_if(probe.metadata.begin(),
        probe.metadata.end(),
        [name](const auto& entry) {
            return EqualsAsciiInsensitive(entry.name, name);
        });
    return found == probe.metadata.end() ? std::wstring{} : found->value;
}

bool ReaderUnavailable(HRESULT status) {
    // The pre-open HasReaderForPath check is encoded as NOT_SUPPORTED by the
    // helper.  E_NOINTERFACE can instead be a matched creator/Open failure and
    // therefore follows 00481759's ordinary failed-item (-1) path.
    return status == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

std::filesystem::path RuntimeDirectory() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return {};
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}

PlaylistInfoResult ReadPlaylistInfoIsolated(
    size_t playlist_slot, size_t row_hint, const std::filesystem::path& path,
    int subtrack, const std::filesystem::path& helper,
    const std::filesystem::path& addin_directory,
    const std::filesystem::path& ttpcomm_path, std::stop_token stop) {
    PlaylistInfoResult result;
    result.playlist_slot = playlist_slot;
    result.row_hint = row_hint;
    result.path = path;
    result.subtrack = subtrack;

    detail::FileInfoProbeProcessState process_state{};
    const auto probe = detail::RunPlaylistInfoReadProbe(
        stop, helper, addin_directory, path, ttpcomm_path, subtrack, 15000,
        &process_state);
    if (!probe) {
        // A missing helper means this build cannot inspect the source.  A
        // launched helper which times out, crashes or emits a partial packet
        // did match the transaction but failed, so 00481759's -2 -> -1
        // failure transition must run and the following queue item can start.
        result.state = process_state ==
                detail::FileInfoProbeProcessState::launch_failed
            ? PlaylistInfoState::unavailable
            : PlaylistInfoState::failed;
        return result;
    }
    if (ReaderUnavailable(probe->status)) {
        result.state = PlaylistInfoState::unavailable;
        return result;
    }
    if (FAILED(probe->status)) return result;

    result.state = PlaylistInfoState::success;
    result.duration_ms = static_cast<int>(std::clamp<std::int64_t>(
        probe->duration_ms, 0, INT_MAX));

    result.title = Utf8(MetadataValue(*probe, L"title"));
    auto artist = MetadataValue(*probe, L"artist");
    if (artist.empty()) artist = MetadataValue(*probe, L"author");
    result.artist = Utf8(artist);
    result.album = Utf8(MetadataValue(*probe, L"album"));
    result.media_type = Utf8(probe->codec);
    const std::uint64_t encoded = probe->encoded_bits_per_second != 0
        ? probe->encoded_bits_per_second
        : static_cast<std::uint64_t>(probe->format.nAvgBytesPerSec) * 8U;
    // CPlayItem reserves bit 31 for the VBR display flag.
    result.bitrate_bps = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(encoded, 0x7fffffffU));
    result.sample_rate_hz = probe->format.nSamplesPerSec;
    result.metadata.reserve(probe->metadata.size());
    for (const auto& entry : probe->metadata) {
        auto name = Utf8(entry.name);
        auto value = Utf8(entry.value);
        if (!name.empty())
            result.metadata.emplace_back(std::move(name), std::move(value));
    }
    return result;
}

} // namespace

// Third-party Open/Metadata vtable slots are not cancellable.  Run them in the
// same bounded helper used by File Properties: timing out kills only that
// process/job, never an in-process thread.  The detached coordinator therefore
// has a hard upper bound and owns neither a plug-in module nor a reader object.
struct PlayerWindow::PlaylistInfoReceiver {
    std::mutex mutex;
    std::stop_source stop;
    HWND target{};
    bool stopped{};
};

void PlayerWindow::RequestPlaylistTrackInfo(size_t playlist_index, size_t row,
                                            bool priority) {
    if (settings_.playlist.read_info_mode == 2 || !sound_library_ ||
        playlist_index >= playlists_.Size()) return;
    const auto& tracks = playlists_.At(playlist_index).Tracks();
    if (row >= tracks.size()) return;
    const auto& track = tracks[row];
    if (track.duration_ms != -2 || track.path.empty() ||
        LooksLikeNetworkPath(track.path.native())) return;

    const auto source_key = SourceKey(track.path, track.subtrack);
    if (playlist_info_unavailable_sources_.contains(source_key)) return;
    const size_t playlist_slot = playlists_.Entries()[playlist_index].slot;
    const auto request_key = RequestKey(
        playlist_slot, track.path, track.subtrack);
    if (playlist_info_queued_keys_.contains(request_key)) {
        if (priority) {
            const auto pending = std::find_if(
                playlist_info_pending_.begin(), playlist_info_pending_.end(),
                [&request_key](const auto& request) {
                    return RequestKey(request.playlist_slot, request.path,
                                      request.subtrack) == request_key;
                });
            if (pending != playlist_info_pending_.end()) {
                auto request = std::move(*pending);
                playlist_info_pending_.erase(pending);
                playlist_info_pending_.push_front(std::move(request));
            }
        }
        return;
    }

    PlaylistInfoRequest request{
        playlist_slot, row, track.path, track.subtrack};
    if (priority)
        playlist_info_pending_.push_front(std::move(request));
    else
        playlist_info_pending_.push_back(std::move(request));
    playlist_info_queued_keys_.insert(request_key);
    StartNextPlaylistInfoRead();
}

void PlayerWindow::QueuePlaylistInfoRange(size_t playlist_index, size_t first,
                                          size_t count, bool priority) {
    if (settings_.playlist.read_info_mode == 2 ||
        playlist_index >= playlists_.Size() || count == 0) return;
    const size_t size = playlists_.At(playlist_index).Tracks().size();
    const size_t end = first > size ? size
        : std::min(size, first + std::min(count, size - first));
    // Push-front requests must be visited backwards to retain row order.
    if (priority) {
        for (size_t row = end; row > first; --row)
            RequestPlaylistTrackInfo(playlist_index, row - 1, true);
    } else {
        for (size_t row = first; row < end; ++row)
            RequestPlaylistTrackInfo(playlist_index, row, false);
    }
}

void PlayerWindow::StartNextPlaylistInfoRead() {
    if (playlist_info_working_) return;
    if (settings_.playlist.read_info_mode == 2 || !sound_library_) {
        playlist_info_pending_.clear();
        playlist_info_queued_keys_.clear();
        return;
    }

    while (!playlist_info_pending_.empty()) {
        auto request = std::move(playlist_info_pending_.front());
        playlist_info_pending_.pop_front();
        const auto request_key = RequestKey(
            request.playlist_slot, request.path, request.subtrack);
        const auto playlist_index =
            playlists_.IndexOfSlot(request.playlist_slot);
        if (!playlist_index) {
            playlist_info_queued_keys_.erase(request_key);
            continue;
        }
        const auto& tracks = playlists_.At(*playlist_index).Tracks();
        const auto source_row = FindUnreadSourceRow(
            tracks, request.path, request.subtrack, request.row_hint);
        if (!source_row) {
            playlist_info_queued_keys_.erase(request_key);
            continue;
        }
        request.row_hint = *source_row;

        if (!playlist_info_receiver_) {
            try {
                playlist_info_receiver_ =
                    std::make_shared<PlaylistInfoReceiver>();
            } catch (const std::exception&) {
                playlist_info_queued_keys_.erase(request_key);
                continue;
            }
            playlist_info_receiver_->target = window_;
        }
        playlist_info_active_ = request;
        playlist_info_working_ = true;
        const auto receiver = playlist_info_receiver_;
        const auto runtime = RuntimeDirectory();
        const auto helper = runtime / L"ttplayer_file_info_probe.exe";
        const auto addin_directory = runtime / L"AddIn";
        const auto ttpcomm_path = runtime / L"ttpcomm.dll";
        const auto stop = receiver->stop.get_token();
        const auto active_key = request_key;
        try {
            std::thread(
                [request = std::move(request), helper, addin_directory,
                 ttpcomm_path, receiver, stop] {
                std::unique_ptr<PlaylistInfoResult> result;
                try {
                    result = std::make_unique<PlaylistInfoResult>(
                        ReadPlaylistInfoIsolated(
                            request.playlist_slot, request.row_hint,
                            request.path,
                            request.subtrack, helper, addin_directory,
                            ttpcomm_path, stop));
                } catch (const std::exception&) {
                    // Allocation/conversion failure is not a decoder verdict.
                    // Complete the queue transaction without changing -2.
                    result.reset();
                }
                // A full destination queue makes PostMessage fail without a
                // completion reaching the UI thread.  Dropping it would leave
                // playlist_info_working_ set forever. Retry outside the
                // receiver lock so shutdown can request cancellation and
                // invalidate the target while the queue drains.
                for (;;) {
                    bool posted{};
                    {
                        std::scoped_lock lock(receiver->mutex);
                        if (receiver->stopped || !receiver->target ||
                            !IsWindow(receiver->target)) break;
                        posted = PostMessageW(
                            receiver->target,
                            detail::kMsgPlaylistInfoReady, 0,
                            reinterpret_cast<LPARAM>(result.get())) != FALSE;
                    }
                    if (posted) {
                        static_cast<void>(result.release());
                        break;
                    }
                    if (stop.stop_requested()) break;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                }
                }).detach();
        } catch (const std::system_error&) {
            playlist_info_queued_keys_.erase(active_key);
            playlist_info_active_.reset();
            playlist_info_working_ = false;
            continue;
        }
        return;
    }
}

LRESULT PlayerWindow::ApplyPlaylistInfoResult(LPARAM value) {
    std::unique_ptr<PlaylistInfoResult> result(
        reinterpret_cast<PlaylistInfoResult*>(value));

    if (playlist_info_active_) {
        playlist_info_queued_keys_.erase(RequestKey(
            playlist_info_active_->playlist_slot,
            playlist_info_active_->path,
            playlist_info_active_->subtrack));
    }
    playlist_info_active_.reset();
    playlist_info_working_ = false;
    if (!result) {
        StartNextPlaylistInfoRead();
        return 0;
    }

    if (result->state == PlaylistInfoState::unavailable) {
        playlist_info_unavailable_sources_.insert(
            SourceKey(result->path, result->subtrack));
    } else if (const auto playlist_index =
                   playlists_.IndexOfSlot(result->playlist_slot)) {
        auto& list = playlists_.At(*playlist_index);
        const auto& tracks = list.Tracks();
        bool changed{};
        // Path/subtrack is the surviving CPlayItem source identity. Apply the
        // single read to every still-unread duplicate in this list: requests
        // for those rows intentionally share one slot/source queue key.
        for (size_t row = 0; row < tracks.size(); ++row) {
            if (tracks[row].duration_ms != -2 ||
                !IsSameSource(tracks[row], result->path,
                              result->subtrack)) continue;
            if (result->state == PlaylistInfoState::failed) {
                changed |= list.SetDuration(row, -1);
            } else {
                changed |= list.SetDuration(row, result->duration_ms);
                changed |= list.SetMetadata(
                    row, result->title, result->artist, result->album);
                changed |= list.SetExtendedMetadata(
                    row, result->metadata, result->media_type,
                    result->bitrate_bps,
                    result->sample_rate_hz);
            }
        }
        if (changed) {
            playlists_.MarkDirty(*playlist_index);
            if (*playlist_index == playlists_.ActiveIndex()) {
                // 00481759 calls FUN_0047FD7D for the completed item; it does
                // not rebuild either ListCtrl or disturb selection.
                if (playlist_window_)
                    InvalidateRect(playlist_window_, nullptr, FALSE);
                if (playlist_view_)
                    InvalidateRect(playlist_view_, nullptr, FALSE);
            } else if (playlist_window_) {
                InvalidateRect(playlist_window_, nullptr, FALSE);
            }
            if (tooltip_ && IsWindow(tooltip_))
                SendMessageW(tooltip_, TTM_UPDATE, 0, 0);
        }
    }

    StartNextPlaylistInfoRead();
    return 0;
}

void PlayerWindow::ShutdownPlaylistInfoLoading() {
    playlist_info_pending_.clear();
    playlist_info_queued_keys_.clear();
    if (playlist_info_receiver_) {
        // Cancellation closes the helper's job/process within the client's
        // bounded wait; shutdown never joins a legacy reader call.
        playlist_info_receiver_->stop.request_stop();
        std::scoped_lock lock(playlist_info_receiver_->mutex);
        playlist_info_receiver_->stopped = true;
        playlist_info_receiver_->target = nullptr;
    }
    playlist_info_active_.reset();
    playlist_info_working_ = false;

    if (!window_) return;
    MSG message{};
    while (PeekMessageW(&message, window_, detail::kMsgPlaylistInfoReady,
                        detail::kMsgPlaylistInfoReady, PM_REMOVE)) {
        delete reinterpret_cast<PlaylistInfoResult*>(message.lParam);
    }
    playlist_info_receiver_.reset();
}

} // namespace ttplayer::ui
