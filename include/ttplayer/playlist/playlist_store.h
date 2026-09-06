#pragma once

#include "ttplayer/playlist/playlist.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace ttplayer::playlist {

// CPlayListManager_LoadStoredLists (004783FA) scans at least 100 numbered
// slots, keeps the configured active slot, and CPlayListManager_FlushDirtyLists
// (00480DE3) commits modified lists through an adjacent .tmp file.
class PlaylistStore {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    struct Entry {
        Playlist playlist;
        size_t slot{};
        unsigned int dirty_edits{};
        unsigned long long dirty_since{};
    };

    void Load(const std::filesystem::path& directory, int scan_hint, int active_slot,
              bool legacy_generation);
    [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] size_t Size() const noexcept { return entries_.size(); }
    [[nodiscard]] size_t ActiveIndex() const noexcept { return active_index_; }
    [[nodiscard]] size_t ActiveSlot() const noexcept;
    [[nodiscard]] Playlist& Active() noexcept { return entries_[active_index_].playlist; }
    [[nodiscard]] const Playlist& Active() const noexcept {
        return entries_[active_index_].playlist;
    }
    [[nodiscard]] const std::vector<Entry>& Entries() const noexcept { return entries_; }
    [[nodiscard]] Playlist& At(size_t index) noexcept { return entries_[index].playlist; }
    [[nodiscard]] const Playlist& At(size_t index) const noexcept {
        return entries_[index].playlist;
    }

    bool SetActive(size_t index) noexcept;
    size_t NewList(std::wstring title = {});
    bool Rename(size_t index, std::wstring title);
    size_t AddList(const std::filesystem::path& path,
                   const LoadOptions& options = {});
    size_t InsertList(const std::filesystem::path& path, size_t index,
                      const LoadOptions& options = {});
    bool ReplaceList(size_t index, const std::filesystem::path& path,
                     const LoadOptions& options = {});
    bool Delete(size_t index);
    bool DeleteActive() { return Delete(active_index_); }
    // FUN_0047835F moves the catalogue entry object while its numbered TTBL
    // identity remains attached to that object.  The active entry therefore
    // follows its stable slot rather than retaining the old numerical row.
    bool ReorderList(size_t old_index, size_t new_index);
    [[nodiscard]] std::optional<size_t> IndexOfSlot(size_t slot) const noexcept;
    void SortByTitle(bool ascending);
    void MarkDirty();
    void MarkDirty(size_t index, unsigned int edits = 1);
    void FlushDirty(bool force);

private:
    [[nodiscard]] std::filesystem::path SlotPath(size_t slot, int digits = 4) const;
    void SaveEntry(Entry& entry);
    bool CompactSlots();

    std::filesystem::path directory_;
    std::vector<Entry> entries_;
    size_t active_index_{};
};

} // namespace ttplayer::playlist
