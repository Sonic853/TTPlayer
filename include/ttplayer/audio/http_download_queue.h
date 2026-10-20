#pragma once
#include "ttplayer/audio/audio_engine.h"

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

enum class DownloadState { waiting, running, complete, canceled, failed, paused };
struct HttpDownload {
    uint64_t id{};
    LegacyCacheEntry entry;
    std::wstring display_title;
    std::filesystem::path destination;
    DownloadState state{DownloadState::waiting};
    uint64_t received{}, total{};
    HRESULT result{S_OK};
    bool prefetch{}, replace{};
};
// Owned by the player, not by an individual sound/decoder. Stopping playback
// does not cancel user downloads. Shutdown joins every worker before unloading.
class HttpDownloadQueue {
public:
    explicit HttpDownloadQueue(std::filesystem::path state_file);
    ~HttpDownloadQueue();
    HttpDownloadQueue(const HttpDownloadQueue&) = delete;
    HttpDownloadQueue& operator=(const HttpDownloadQueue&) = delete;
    void Configure(const PlaybackOptions& options, int maximum);
    uint64_t Enqueue(std::wstring url, std::wstring title,
                     std::filesystem::path destination = {}, bool replace = false,
                     std::wstring artist = {});
    void Cancel(uint64_t id);
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
