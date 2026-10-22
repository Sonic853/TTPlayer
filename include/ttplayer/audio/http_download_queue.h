#pragma once
#include <windows.h>
#include <objidl.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::audio {
// 5.7.9: 004D025D / 004D05F1. These are names/locations, NOT a map of
// downloaded byte ranges. In particular an old preallocated temp is not resumable.
struct LegacyCacheEntry {
    std::wstring title, temp, save, sourceurl, excludeurl, tag, playtag;
};
std::vector<LegacyCacheEntry> ReadLegacyCacheList(const std::filesystem::path& file);
std::filesystem::path ResolveLegacySavedFile(const std::vector<LegacyCacheEntry>& entries,
    std::wstring_view url, const std::vector<std::filesystem::path>& roots);
std::wstring HttpDownloadKey(std::wstring url);
std::wstring LegacyCacheTitle(std::wstring_view url, std::wstring_view title,
                             std::wstring_view artist);
bool IsHttpDownloadMediaPath(const std::filesystem::path& path);

enum class DownloadState { waiting, running, complete, canceled, failed, paused };
struct HttpDownload {
    uint64_t id{};
    LegacyCacheEntry entry;
    std::wstring display_title;
    std::wstring tid;
    std::filesystem::path destination;
    DownloadState state{DownloadState::waiting};
    uint64_t received{}, total{};
    HRESULT result{S_OK};
    bool prefetch{}, replace{};
};
// Implemented only in ttpbrowser.dll. Playback uses the read-only helpers above.
// The host's existing network producer returns a consumer stream; the callback
// duplicates the cancellation event before retaining it beyond the call.
class HttpDownloadQueue {
public:
    using StreamOpener = std::function<HRESULT(const std::wstring&, HANDLE,
                                               IStream**, BOOL*)>;
    HttpDownloadQueue(std::filesystem::path state_file, StreamOpener opener);
    ~HttpDownloadQueue();
    HttpDownloadQueue(const HttpDownloadQueue&) = delete;
    HttpDownloadQueue& operator=(const HttpDownloadQueue&) = delete;
    void Configure(int maximum, std::filesystem::path legacy_cache_folder = {});
    uint64_t Enqueue(std::wstring url, std::wstring title,
                     std::filesystem::path destination = {}, bool replace = false,
                     std::wstring artist = {}, std::wstring tid = {});
    void Cancel(uint64_t id);
    // Stop this consumer and persist a paused task. Retry starts it again;
    // any byte reuse belongs to the host producer's verified cache journal.
    void Pause(uint64_t id);
    void Retry(uint64_t id);
    // Remove queue records only. Running work is canceled before its private
    // temporary file can be published; completed destination files are kept.
    bool Remove(uint64_t id);
    // Complete, canceled and failed records; waiting/running/paused work stays.
    size_t ClearHistory();
    // Explicit import of cachelist v2 or tasklist v1; never modifies the input.
    size_t ImportLegacyList(const std::filesystem::path& file,
        const std::filesystem::path& folder, bool artist_folder, bool replace);
    // One speculative successor (00460904); a new prediction cancels the old
    // one. Explicit downloads always take precedence and survive replacement.
    void Prefetch(std::wstring url);
    std::vector<HttpDownload> Snapshot() const;
    HRESULT StorageResult() const noexcept;
    // Nonempty when replacement recovery had to retain the old file separately.
    std::filesystem::path RecoveryBackupPath() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::filesystem::path HttpDownloadDestination(const std::filesystem::path& folder,
    std::wstring_view url, std::wstring title, std::wstring artist, bool artist_folder);
} // namespace ttplayer::audio
