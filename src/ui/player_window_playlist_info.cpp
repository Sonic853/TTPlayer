#include "player_window_internal.h"
#include "file_info_probe_client.h"

#include "ttplayer/core/text.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
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
    std::uint64_t info_revision{};
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
    const std::filesystem::path& ttpcomm_path, std::stop_token stop,
    detail::PlaylistInfoProbeSession& session,
    const detail::FileInfoProbeMp3Policy& policy) {
    PlaylistInfoResult result;
    result.playlist_slot = playlist_slot;
    result.row_hint = row_hint;
    result.path = path;
    result.subtrack = subtrack;

    detail::FileInfoProbeProcessState process_state{};
    const auto probe = session.Read(
        stop, helper, addin_directory, path, ttpcomm_path, subtrack, 15000,
        &process_state, policy);
    if (!probe) {
        // A missing helper means this build cannot inspect the source.  A
        // launched helper which times out, crashes or emits a partial packet
        // did match the transaction but failed, so 00481759's -2 -> -1
        // failure transition must run and the following queue item can start.
        result.state = process_state ==
                detail::FileInfoProbeProcessState::launch_failed || process_state ==
                detail::FileInfoProbeProcessState::cancelled
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

// Only copied jobs and the protected delivery HWND cross the thread boundary.
// One coordinator owns one reusable isolated session; shutdown never joins an
// untrusted reader on the UI thread.
struct PlayerWindow::PlaylistInfoReceiver {
    struct Job {
        PlaylistInfoRequest request;
        std::uint64_t revision{};
        detail::FileInfoProbeMp3Policy policy;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::stop_source stop;
    std::optional<Job> job;
    HWND target{};
    bool stopped{};
};

struct PlayerWindow::PlaylistInfoIndex {
    size_t slot{};
    std::uint64_t revision{};
    std::unordered_map<std::wstring, std::vector<size_t>> rows;
};

const std::vector<size_t>& PlayerWindow::PlaylistInfoSourceRows(
    size_t index, const std::wstring& source) {
    const auto& list = playlists_.At(index);
    const auto slot = playlists_.Entries()[index].slot;
    if (!playlist_info_index_) playlist_info_index_ = std::make_shared<PlaylistInfoIndex>();
    auto& lookup = *playlist_info_index_;
    if (lookup.slot != slot || lookup.revision != list.OrderRevision()) {
        lookup.rows.clear();
        lookup.slot = slot; lookup.revision = list.OrderRevision();
        const auto& tracks = list.Tracks();
        for (size_t row = 0; row < tracks.size(); ++row)
            lookup.rows[SourceKey(tracks[row].path, tracks[row].subtrack)].push_back(row);
    }
    const auto found = lookup.rows.find(source);
    static const std::vector<size_t> empty;
    return found == lookup.rows.end() ? empty : found->second;
}

void PlayerWindow::RequestPlaylistTrackInfo(size_t playlist_index, size_t row,
                                            bool priority, bool visible_only) {
    if (settings_.playlist.read_info_mode == 2 || !sound_library_ ||
        playlist_index >= playlists_.Size()) return;
    const auto& tracks = playlists_.At(playlist_index).Tracks();
    if (row >= tracks.size()) return;
    const auto& track = tracks[row];
    if (track.duration_ms != -2 || track.path.empty() ||
        LooksLikeNetworkPath(track.path.native())) return;
    const auto slot = playlists_.Entries()[playlist_index].slot;
    const auto key = RequestKey(slot, track.path, track.subtrack);
    const auto checked = playlist_info_checked_.find(key);
    if (checked != playlist_info_checked_.end() && GetTickCount64() - checked->second < 5000) return;
    if (playlist_info_active_ && RequestKey(playlist_info_active_->playlist_slot,
            playlist_info_active_->path, playlist_info_active_->subtrack) == key) {
        playlist_info_active_->visible_only &= visible_only;
        return;
    }
    if (const auto existing = playlist_info_queued_keys_.find(key);
        existing != playlist_info_queued_keys_.end()) {
        // A visible request may promote bulk work, but never turns it into
        // cancellable off-screen work. list::splice preserves its iterator.
        existing->second->visible_only &= visible_only;
        if (priority) playlist_info_pending_.splice(
            playlist_info_pending_.begin(), playlist_info_pending_, existing->second);
        return;
    }
    PlaylistInfoRequest request{slot, row, track.path, track.subtrack, visible_only};
    const auto position = playlist_info_pending_.insert(priority ? playlist_info_pending_.begin() :
        playlist_info_pending_.end(), std::move(request));
    playlist_info_queued_keys_.emplace(key, position);
    StartNextPlaylistInfoRead();
}

void PlayerWindow::QueuePlaylistInfoRange(size_t index, size_t first, size_t count,
                                         bool priority, bool visible_only) {
    if (settings_.playlist.read_info_mode == 2 || index >= playlists_.Size() || !count) return;
    const size_t size = playlists_.At(index).Tracks().size();
    const size_t end = first > size ? size : first + std::min(count, size - first);
    if (priority) {
        for (size_t row = end; row > first; --row)
            RequestPlaylistTrackInfo(index, row - 1, true, visible_only);
    } else {
        for (size_t row = first; row < end; ++row)
            RequestPlaylistTrackInfo(index, row, false, visible_only);
    }
}

void PlayerWindow::PollPlaylistInfo() {
    if (settings_.playlist.read_info_mode == 2 || !sound_library_) {
        if (playlist_info_receiver_ || !playlist_info_pending_.empty()) ShutdownPlaylistInfoLoading();
        return;
    }
    const auto now = GetTickCount64();
    if (now - playlist_info_poll_tick_ >= 1000) {
        playlist_info_poll_tick_ = now;
        if (!settings_.playlist.library_mode) {
            const auto [first, count] = VisiblePlaylistInfoRange();
            QueuePlaylistInfoRange(playlists_.ActiveIndex(), first, count, true, true);
        }
    }
    StartNextPlaylistInfoRead();
}

void PlayerWindow::StartNextPlaylistInfoRead() {
    if (playlist_info_working_) return;
    if (settings_.playlist.read_info_mode == 2 || !sound_library_) {
        playlist_info_pending_.clear(); playlist_info_queued_keys_.clear(); return;
    }
    // Bound queue pruning per UI dispatch; the normal UI timer continues it.
    size_t examined{};
    while (!playlist_info_pending_.empty() && examined++ < 128) {
        auto request = std::move(playlist_info_pending_.front());
        playlist_info_queued_keys_.erase(RequestKey(request.playlist_slot, request.path, request.subtrack));
        playlist_info_pending_.pop_front();
        const auto index = playlists_.IndexOfSlot(request.playlist_slot);
        if (!index) continue;
        const auto& list = playlists_.At(*index);
        const auto& tracks = list.Tracks();
        const auto& rows = PlaylistInfoSourceRows(*index, SourceKey(request.path, request.subtrack));
        const auto [first, count] = VisiblePlaylistInfoRange();
        const auto row = std::find_if(rows.begin(), rows.end(), [&](size_t candidate) {
            return tracks[candidate].duration_ms == -2 && (!request.visible_only ||
                (!settings_.playlist.library_mode && *index == playlists_.ActiveIndex() &&
                 candidate >= first && candidate - first < count));
        });
        if (row == rows.end()) continue;
        request.row_hint = *row;
        if (!playlist_info_receiver_) {
            auto receiver = std::make_shared<PlaylistInfoReceiver>();
            receiver->target = window_;
            const auto runtime = RuntimeDirectory();
            try {
                std::thread([receiver, runtime] {
                    const auto stop = receiver->stop.get_token();
                    detail::PlaylistInfoProbeSession session;
                    for (;;) {
                        PlaylistInfoReceiver::Job job;
                        {
                            std::unique_lock lock(receiver->mutex);
                            receiver->wake.wait(lock, [&] { return receiver->stopped || receiver->job.has_value(); });
                            if (receiver->stopped) return;
                            job = std::move(*receiver->job); receiver->job.reset();
                        }
                        std::unique_ptr<PlaylistInfoResult> result;
                        try {
                            const auto& r = job.request;
                            result = std::make_unique<PlaylistInfoResult>(ReadPlaylistInfoIsolated(
                                r.playlist_slot, r.row_hint, r.path, r.subtrack, {},
                                runtime / L"AddIn", runtime / L"ttpcomm.dll", stop, session, job.policy));
                            result->info_revision = job.revision;
                        } catch (const std::exception&) { result.reset(); }
                        for (;;) {
                            bool posted{};
                            {
                                std::scoped_lock lock(receiver->mutex);
                                if (receiver->stopped || !receiver->target || !IsWindow(receiver->target)) return;
                                posted = PostMessageW(receiver->target, detail::kMsgPlaylistInfoReady,
                                    0, reinterpret_cast<LPARAM>(result.get())) != FALSE;
                            }
                            if (posted) { static_cast<void>(result.release()); break; }
                            if (stop.stop_requested()) return;
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    }
                }).detach();
                playlist_info_receiver_ = std::move(receiver);
            } catch (const std::system_error&) { continue; }
        }
        detail::FileInfoProbeMp3Policy policy;
        policy.read_priority = static_cast<std::uint32_t>(settings_.general.mp3_read_tag_priority);
        policy.write_type = static_cast<std::uint32_t>(settings_.general.mp3_write_tag_type);
        policy.id3v2_encoding = static_cast<std::uint32_t>(settings_.general.mp3_id3v2_encoding);
        policy.id3v2_padding = settings_.general.mp3_id3v2_padding ? 1U : 0U;
        playlist_info_active_ = request; playlist_info_working_ = true;
        {
            std::scoped_lock lock(playlist_info_receiver_->mutex);
            playlist_info_receiver_->job = PlaylistInfoReceiver::Job{std::move(request), list.InfoRevision(), policy};
        }
        playlist_info_receiver_->wake.notify_one();
        return;
    }
}

LRESULT PlayerWindow::ApplyPlaylistInfoResult(LPARAM value) {
    std::unique_ptr<PlaylistInfoResult> result(reinterpret_cast<PlaylistInfoResult*>(value));
    const auto active = std::move(playlist_info_active_);
    playlist_info_active_.reset(); playlist_info_working_ = false;
    if (result && settings_.playlist.read_info_mode != 2) {
        const auto index = playlists_.IndexOfSlot(result->playlist_slot);
        if (index) {
            auto& list = playlists_.At(*index);
            if (list.InfoRevision() != result->info_revision) {
                // A tag edit or playback update won the race. Re-read only if
                // the source still needs information, preserving bulk intent.
                if (active) {
                    const auto rows = PlaylistInfoSourceRows(*index, SourceKey(result->path, result->subtrack));
                    const auto unread = std::find_if(rows.begin(), rows.end(), [&](size_t row) {
                        return list.Tracks()[row].duration_ms == -2;
                    });
                    if (unread != rows.end()) RequestPlaylistTrackInfo(*index, *unread, true, active->visible_only);
                }
            } else if (result->state == PlaylistInfoState::unavailable) {
                if (playlist_info_checked_.size() >= 2048) playlist_info_checked_.clear();
                playlist_info_checked_[RequestKey(result->playlist_slot, result->path, result->subtrack)] = GetTickCount64();
            } else {
                std::vector<size_t> changed_rows;
                const auto& rows = PlaylistInfoSourceRows(*index, SourceKey(result->path, result->subtrack));
                for (const auto row : rows) {
                    if (list.Tracks()[row].duration_ms != -2) continue;
                    bool changed{};
                    if (result->state == PlaylistInfoState::failed) changed = list.SetDuration(row, -1);
                    else {
                        changed |= list.SetDuration(row, result->duration_ms);
                        changed |= list.SetMetadata(row, result->title, result->artist, result->album);
                        changed |= list.SetExtendedMetadata(row, result->metadata, result->media_type,
                            result->bitrate_bps, result->sample_rate_hz);
                    }
                    if (changed) changed_rows.push_back(row);
                }
                if (!changed_rows.empty()) {
                    playlists_.MarkDirty(*index);
                    if (*index == playlists_.ActiveIndex()) InvalidatePlaylistInfoRows(changed_rows);
                    if (playlist_item_tooltip_ && IsWindow(playlist_item_tooltip_))
                        SendMessageW(playlist_item_tooltip_, TTM_UPDATE, 0, 0);
                }
            }
        }
    }
    StartNextPlaylistInfoRead();
    return 0;
}

void PlayerWindow::ShutdownPlaylistInfoLoading() {
    playlist_info_pending_.clear(); playlist_info_queued_keys_.clear();
    playlist_info_checked_.clear(); playlist_info_index_.reset();
    if (playlist_info_receiver_) {
        playlist_info_receiver_->stop.request_stop();
        {
            std::scoped_lock lock(playlist_info_receiver_->mutex);
            playlist_info_receiver_->stopped = true; playlist_info_receiver_->target = nullptr;
        }
        playlist_info_receiver_->wake.notify_one();
    }
    playlist_info_active_.reset(); playlist_info_working_ = false;
    if (window_) {
        MSG message{};
        while (PeekMessageW(&message, window_, detail::kMsgPlaylistInfoReady,
                            detail::kMsgPlaylistInfoReady, PM_REMOVE))
            delete reinterpret_cast<PlaylistInfoResult*>(message.lParam);
    }
    playlist_info_receiver_.reset();
}

} // namespace ttplayer::ui
