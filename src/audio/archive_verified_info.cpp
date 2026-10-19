#include "ttplayer/audio/archive_verified_info.h"
#include "ttplayer/audio/cue_sheet.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <utility>

namespace ttplayer::audio {

class ArchiveInfoVerificationState {
public:
    explicit ArchiveInfoVerificationState(ArchiveCacheIdentity value)
        : identity(std::move(value)) {}
    ArchiveCacheIdentity identity;
    mutable std::atomic_bool completed{};
};

namespace {

constexpr size_t kMaximumEntries = 32;
constexpr size_t kMaximumInformationBytes = 1024 * 1024;
constexpr size_t kMaximumCacheBytes = 4 * 1024 * 1024;

struct VerifiedInfo {
    ArchiveCacheIdentity identity;
    BuiltinFileInfo info;
    size_t bytes{};
};

struct CacheEntry {
    std::wstring key;
    std::shared_ptr<const VerifiedInfo> info;
    std::weak_ptr<const ArchiveInfoVerificationState> pending;
};

std::mutex cache_mutex;
std::list<CacheEntry> cache;
size_t cache_bytes{};

std::wstring SourceKey(const std::filesystem::path& logical_path) {
    ArchiveMemberPath path;
    if (!ParseArchiveMemberPath(logical_path.native(), path)) return {};
    auto key = std::filesystem::absolute(path.archive).lexically_normal().native();
    key.push_back(L'|');
    // Preserve the original member spelling's dot components. The archive
    // service intentionally does not canonicalize paths inside an archive.
    key += path.member;
    std::replace(key.begin(), key.end(), L'/', L'\\');
    if (!key.empty()) CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}

auto Find(const std::wstring& key) {
    return std::find_if(cache.begin(), cache.end(), [&](const auto& entry) {
        return entry.key == key;
    });
}

void Trim() {
    while (cache.size() > kMaximumEntries || cache_bytes > kMaximumCacheBytes) {
        if (cache.back().info) cache_bytes -= cache.back().info->bytes;
        cache.pop_back();
    }
}

std::shared_ptr<const VerifiedInfo> Lookup(const std::filesystem::path& path) {
    const auto key = SourceKey(path);
    if (key.empty()) return {};
    std::shared_ptr<const VerifiedInfo> info;
    {
        std::scoped_lock lock(cache_mutex);
        const auto entry = Find(key);
        if (entry == cache.end()) return {};
        info = entry->info;
        cache.splice(cache.begin(), cache, entry);
    }
    // No filesystem calls while holding the shared cache lock.
    if (!info || !ArchiveCacheIdentityFresh(info->identity)) return {};
    return info;
}

bool Pending(const std::filesystem::path& path) {
    const auto key = SourceKey(path);
    if (key.empty()) return false;
    std::shared_ptr<const ArchiveInfoVerificationState> pending;
    {
        std::scoped_lock lock(cache_mutex);
        const auto entry = Find(key);
        if (entry == cache.end()) return false;
        pending = entry->pending.lock();
    }
    return pending && !pending->completed.load(std::memory_order_acquire) &&
        ArchiveCacheIdentityFresh(pending->identity);
}

struct CueSnapshot {
    CueSheet sheet;
    ArchiveCacheIdentity identity;
};

std::optional<CueSnapshot> CachedCue(const std::filesystem::path& path) {
    ArchiveMemberPath member;
    if (!ParseArchiveMemberPath(path.native(), member)) return std::nullopt;
    std::vector<unsigned char> bytes;
    ArchiveCacheIdentity identity;
    if (!TryReadCachedArchiveMember(member, bytes, &identity)) return std::nullopt;
    return CueSnapshot{CueSheet::LoadFromMemory(bytes, path), std::move(identity)};
}

} // namespace

ArchiveInfoVerification BeginArchiveInfoVerification(
    const std::filesystem::path& path, const ArchiveCacheIdentity& identity) noexcept {
    try {
        if (!identity || !ArchiveCacheIdentityFresh(identity)) return {};
        const auto key = SourceKey(path);
        if (key.empty()) return {};
        auto pending = std::make_shared<ArchiveInfoVerificationState>(identity);
        std::scoped_lock lock(cache_mutex);
        auto entry = Find(key);
        if (entry == cache.end()) {
            cache.push_front({key, {}, pending});
        } else {
            entry->pending = pending;
            cache.splice(cache.begin(), cache, entry);
        }
        Trim();
        return pending;
    } catch (...) { return {}; }
}

void PublishVerifiedArchiveInfo(const std::filesystem::path& path,
    const BuiltinFileInfo& info, const ArchiveCacheIdentity& identity) noexcept {
    try {
        if (!identity || !ArchiveCacheIdentityFresh(identity)) return;
        const auto key = SourceKey(path);
        if (key.empty()) return;
        if (info.codec.size() > kMaximumInformationBytes / sizeof(wchar_t) ||
            key.size() > kMaximumInformationBytes / sizeof(wchar_t)) return;
        size_t bytes = sizeof(VerifiedInfo) + sizeof(CacheEntry) +
            (info.codec.size() + key.size()) * sizeof(wchar_t);
        if (info.metadata.size() > 4096) return;
        for (const auto& field : info.metadata) {
            if (field.name.size() > kMaximumInformationBytes / sizeof(wchar_t) ||
                field.value.size() > kMaximumInformationBytes / sizeof(wchar_t)) return;
            bytes += sizeof(BuiltinMetadataEntry) +
                (field.name.size() + field.value.size()) * sizeof(wchar_t);
            if (bytes > kMaximumInformationBytes) return;
        }
        if (bytes > kMaximumInformationBytes) return;
        auto value = std::make_shared<VerifiedInfo>();
        value->identity = identity;
        value->info.capabilities = info.capabilities & ~4U;
        value->info.format = info.format;
        value->info.duration_ms = info.duration_ms;
        value->info.encoded_bits_per_second = info.encoded_bits_per_second;
        value->info.codec = info.codec;
        value->info.metadata = info.metadata;
        // The playlist never requests cover bytes. Keep its cache independent
        // of potentially large art and the small-member cache's eviction.
        value->bytes = bytes;
        std::scoped_lock lock(cache_mutex);
        auto entry = Find(key);
        if (entry == cache.end()) {
            cache.push_front({key, value, {}});
        } else {
            if (entry->info) cache_bytes -= entry->info->bytes;
            entry->info = value;
            if (const auto pending = entry->pending.lock();
                pending && pending->identity == identity)
                pending->completed.store(true, std::memory_order_release);
            cache.splice(cache.begin(), cache, entry);
        }
        cache_bytes += bytes;
        Trim();
    } catch (...) {}
}

std::optional<BuiltinFileInfo> TryReadVerifiedArchiveInfo(
    const std::filesystem::path& path, int subtrack) noexcept {
    try {
        if (_wcsicmp(path.extension().c_str(), L".cue") != 0) {
            const auto info = Lookup(path);
            return info ? std::optional<BuiltinFileInfo>(info->info) : std::nullopt;
        }
        const auto cue = CachedCue(path);
        if (!cue) return std::nullopt;
        const auto* track = cue->sheet.FindTrack(subtrack);
        if (!track || !track->has_index01) return std::nullopt;
        // Do not guess the selected fallback from whichever candidate happens
        // to be cached. The explicit FILE target may exist and decode even if
        // a previously played same-name fallback has verified information.
        const auto info = Lookup(track->audio_path);
        if (info) {
            const auto start = CueFrameToMilliseconds(track->start_frame);
            if (info->info.duration_ms && start > info->info.duration_ms) return std::nullopt;
            BuiltinFileInfo result = info->info;
            const auto duration = track->DurationMilliseconds();
            result.duration_ms = static_cast<DWORD>(std::clamp<std::int64_t>(
                duration >= 0 ? duration : static_cast<std::int64_t>(result.duration_ms) - start,
                0, MAXDWORD));
            result.metadata.clear();
            for (const auto& [name, value] : track->metadata)
                result.metadata.push_back({name, value});
            if (!ArchiveCacheIdentityFresh(cue->identity) ||
                !ArchiveCacheIdentityFresh(info->identity)) return std::nullopt;
            return result;
        }
    } catch (...) {}
    return std::nullopt;
}

bool IsArchiveInfoVerificationPending(const std::filesystem::path& path,
    int subtrack) noexcept {
    try {
        if (_wcsicmp(path.extension().c_str(), L".cue") != 0) return Pending(path);
        const auto cue = CachedCue(path);
        if (!cue) return false;
        const auto* track = cue->sheet.FindTrack(subtrack);
        if (!track || !track->has_index01) return false;
        if (Pending(track->audio_path)) return ArchiveCacheIdentityFresh(cue->identity);
    } catch (...) {}
    return false;
}

} // namespace ttplayer::audio
