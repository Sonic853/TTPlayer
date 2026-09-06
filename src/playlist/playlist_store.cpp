#include "ttplayer/playlist/playlist_store.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <cwchar>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace ttplayer::playlist {
namespace {
unsigned long long TickCount() noexcept { return GetTickCount64(); }

constexpr size_t kMaximumStoredSlots = 1000;

std::filesystem::path NumberedSlotPath(const std::filesystem::path& directory,
                                       size_t slot, int digits = 4) {
    wchar_t name[32]{};
    swprintf_s(name, digits == 3 ? L"%03zu.ttbl" : L"%04zu.ttbl", slot);
    return directory / name;
}

std::filesystem::path CompactionMarkerPath(
    const std::filesystem::path& directory) {
    return directory / L".ttbl-compact";
}

std::filesystem::path CompactionStagePath(
    const std::filesystem::path& directory, size_t slot) {
    return std::filesystem::path(
        NumberedSlotPath(directory, slot).wstring() + L".compact.tmp");
}

template<typename T>
bool ReadMarkerValue(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return input.good();
}

template<typename T>
void WriteMarkerValue(std::ostream& output, T value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool ReadCompactionMarker(const std::filesystem::path& marker,
                          std::vector<size_t>& sources,
                          size_t* active_index = nullptr) {
    std::ifstream input(marker, std::ios::binary);
    if (!input) return false;
    std::array<char, 4> magic{};
    input.read(magic.data(), magic.size());
    std::uint32_t count{};
    std::uint32_t active{};
    if (!input || magic != std::array<char, 4>{'T', 'T', 'C', '2'} ||
        !ReadMarkerValue(input, count) || count == 0 ||
        count > kMaximumStoredSlots || !ReadMarkerValue(input, active) ||
        active >= count) return false;
    sources.clear();
    sources.reserve(count);
    std::set<size_t> unique;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t source{};
        if (!ReadMarkerValue(input, source) || source >= kMaximumStoredSlots ||
            !unique.insert(source).second) return false;
        sources.push_back(source);
    }
    if (input.peek() != std::char_traits<char>::eof()) return false;
    if (active_index) *active_index = active;
    return true;
}

bool RecoverCompaction(const std::filesystem::path& directory) {
    const auto marker = CompactionMarkerPath(directory);
    std::error_code ignored;
    if (!std::filesystem::exists(marker, ignored)) return true;

    std::vector<size_t> sources;
    if (!ReadCompactionMarker(marker, sources)) return false;
    for (size_t destination = 0; destination < sources.size(); ++destination) {
        if (sources[destination] == destination) continue;
        const auto staged = CompactionStagePath(directory, destination);
        const auto target = NumberedSlotPath(directory, destination);
        if (std::filesystem::exists(staged, ignored)) {
            if (!MoveFileExW(staged.c_str(), target.c_str(),
                             MOVEFILE_REPLACE_EXISTING |
                             MOVEFILE_WRITE_THROUGH)) return false;
        } else if (!std::filesystem::exists(target, ignored)) {
            return false;
        }
    }
    for (size_t destination = 0; destination < sources.size(); ++destination) {
        if (!std::filesystem::exists(NumberedSlotPath(directory, destination),
                                     ignored)) return false;
    }

    // Only source slots named by our committed transaction are eligible for
    // cleanup.  Other files in PlayList, including unrelated *.ttbl files,
    // are never enumerated or removed.
    for (const size_t source : sources) {
        if (source < sources.size()) continue;
        const auto old = NumberedSlotPath(directory, source);
        std::filesystem::remove(old, ignored);
        std::filesystem::remove(
            std::filesystem::path(old.wstring() + L".tmp"), ignored);
    }
    for (size_t destination = 0; destination < sources.size(); ++destination)
        std::filesystem::remove(CompactionStagePath(directory, destination),
                                ignored);
    std::filesystem::remove(marker, ignored);
    return true;
}
}

std::filesystem::path PlaylistStore::SlotPath(size_t slot, int digits) const {
    return NumberedSlotPath(directory_, slot, digits);
}

void PlaylistStore::Load(const std::filesystem::path& directory, int scan_hint,
                         int active_slot, bool legacy_generation) {
    directory_ = directory;
    entries_.clear();
    active_index_ = 0;
    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);
    std::vector<size_t> compacted_sources;
    size_t compacted_active{};
    if (ReadCompactionMarker(CompactionMarkerPath(directory_),
                             compacted_sources, &compacted_active))
        active_slot = static_cast<int>(compacted_active);
    static_cast<void>(RecoverCompaction(directory_));

    const int slots = std::clamp(scan_hint, 100, 1000);
    for (int slot = 0; slot < slots; ++slot) {
        auto path = SlotPath(static_cast<size_t>(slot));
        bool loaded_legacy = false;
        const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
        if (!std::filesystem::exists(path, ignored) && std::filesystem::exists(temporary, ignored)) {
            MoveFileExW(temporary.c_str(), path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        if (!std::filesystem::exists(path, ignored) && legacy_generation) {
            const auto old_path = SlotPath(static_cast<size_t>(slot), 3);
            if (std::filesystem::exists(old_path, ignored)) {
                path = old_path;
                loaded_legacy = true;
            }
        }
        if (!std::filesystem::exists(path, ignored)) continue;
        try {
            Entry entry;
            entry.slot = static_cast<size_t>(slot);
            entry.playlist.LoadTtbl(path);
            if (loaded_legacy) {
                entry.dirty_edits = 1;
                entry.dirty_since = TickCount();
            }
            entries_.push_back(std::move(entry));
        } catch (const std::exception&) {
            // 004783FA skips an unreadable slot and continues scanning the
            // remaining numbered files instead of aborting application start.
        }
    }

    if (entries_.empty()) entries_.push_back({Playlist{}, 0, 1, TickCount()});
    const auto active = std::find_if(entries_.begin(), entries_.end(), [active_slot](const Entry& item) {
        return active_slot >= 0 && item.slot == static_cast<size_t>(active_slot);
    });
    if (active != entries_.end())
        active_index_ = static_cast<size_t>(active - entries_.begin());
}

size_t PlaylistStore::ActiveSlot() const noexcept {
    return entries_.empty() ? 0 : entries_[active_index_].slot;
}

bool PlaylistStore::SetActive(size_t index) noexcept {
    if (index >= entries_.size() || index == active_index_) return false;
    active_index_ = index;
    return true;
}

size_t PlaylistStore::NewList(std::wstring title) {
    size_t slot = 0;
    while (std::any_of(entries_.begin(), entries_.end(), [slot](const Entry& item) {
        return item.slot == slot;
    })) ++slot;
    Entry entry;
    entry.slot = slot;
    entry.playlist.SetTitle(std::move(title));
    entries_.push_back(std::move(entry));
    active_index_ = entries_.size() - 1;
    MarkDirty();
    return active_index_;
}

bool PlaylistStore::Rename(size_t index, std::wstring title) {
    // CPlayListWnd's LVN_ENDLABELEDITW handler (00489B06) accepts the label
    // only when pszText is non-null and its first character is non-zero.
    // Whitespace is deliberately retained: the original does not trim it.
    if (index >= entries_.size() || title.empty()) return false;
    auto& entry = entries_[index];
    entry.playlist.SetTitle(std::move(title));
    if (entry.dirty_edits == 0) entry.dirty_since = TickCount();
    ++entry.dirty_edits;
    return true;
}

size_t PlaylistStore::AddList(const std::filesystem::path& path,
                              const LoadOptions& options) {
    return InsertList(path, entries_.size(), options);
}

size_t PlaylistStore::InsertList(const std::filesystem::path& path,
                                 size_t index,
                                 const LoadOptions& options) {
    Playlist loaded;
    loaded.SetTitle(path.stem().wstring());
    loaded.LoadFromFile(path, options);
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
    // 00476550 returns -1 when the M3U parser did not produce a single row.
    // In particular, an empty or comments-only list is consumed by the drop
    // target but never becomes an empty catalogue entry.
    if ((extension == L".m3u" || extension == L".m3u8") &&
        loaded.Tracks().empty()) return npos;
    size_t slot = 0;
    while (std::any_of(entries_.begin(), entries_.end(), [slot](const Entry& item) {
        return item.slot == slot;
    })) ++slot;
    index = std::min(index, entries_.size());
    entries_.insert(entries_.begin() + static_cast<ptrdiff_t>(index),
                    {std::move(loaded), slot, 1, TickCount()});
    active_index_ = index;
    return active_index_;
}

bool PlaylistStore::ReplaceList(size_t index,
                                const std::filesystem::path& path,
                                const LoadOptions& options) {
    if (index >= entries_.size()) return false;
    Playlist loaded;
    loaded.SetTitle(path.stem().wstring());
    loaded.LoadFromFile(path, options);
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
    if ((extension == L".m3u" || extension == L".m3u8") &&
        loaded.Tracks().empty()) return false;

    auto& entry = entries_[index];
    entry.playlist = std::move(loaded);
    entry.dirty_edits = 1;
    entry.dirty_since = TickCount();
    active_index_ = index;
    return true;
}

bool PlaylistStore::Delete(size_t index) {
    if (entries_.size() <= 1 || index >= entries_.size()) return false;
    const size_t slot = entries_[index].slot;
    std::error_code ignored;
    std::filesystem::remove(SlotPath(slot), ignored);
    std::filesystem::remove(std::filesystem::path(SlotPath(slot).wstring() + L".tmp"),
                            ignored);
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(index));
    if (active_index_ > index) --active_index_;
    else if (active_index_ == index) active_index_ = std::min(index, entries_.size() - 1);
    return true;
}

std::optional<size_t> PlaylistStore::IndexOfSlot(size_t slot) const noexcept {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [slot](const Entry& entry) { return entry.slot == slot; });
    if (found == entries_.end()) return std::nullopt;
    return static_cast<size_t>(found - entries_.begin());
}

bool PlaylistStore::ReorderList(size_t old_index, size_t new_index) {
    if (old_index >= entries_.size() || new_index >= entries_.size() ||
        old_index == new_index) return false;

    const size_t active_slot = ActiveSlot();
    if (old_index < new_index) {
        std::rotate(entries_.begin() + static_cast<ptrdiff_t>(old_index),
                    entries_.begin() + static_cast<ptrdiff_t>(old_index + 1),
                    entries_.begin() + static_cast<ptrdiff_t>(new_index + 1));
    } else {
        std::rotate(entries_.begin() + static_cast<ptrdiff_t>(new_index),
                    entries_.begin() + static_cast<ptrdiff_t>(old_index),
                    entries_.begin() + static_cast<ptrdiff_t>(old_index + 1));
    }
    active_index_ = IndexOfSlot(active_slot).value_or(0);
    return true;
}

void PlaylistStore::SortByTitle(bool ascending) {
    if (entries_.size() < 2) return;
    const size_t active_slot = ActiveSlot();
    std::stable_sort(entries_.begin(), entries_.end(), [ascending](
        const Entry& left, const Entry& right) {
        const int result = CompareLegacyLogicalText(
            left.playlist.Title().c_str(), right.playlist.Title().c_str());
        return ascending ? result < 0 : result > 0;
    });
    const auto active = std::find_if(entries_.begin(), entries_.end(),
        [active_slot](const Entry& entry) { return entry.slot == active_slot; });
    active_index_ = active == entries_.end() ? 0 :
        static_cast<size_t>(active - entries_.begin());
}

void PlaylistStore::MarkDirty() {
    MarkDirty(active_index_);
}

void PlaylistStore::MarkDirty(size_t index, unsigned int edits) {
    if (index >= entries_.size() || edits == 0) return;
    auto& entry = entries_[index];
    if (entry.dirty_edits == 0) entry.dirty_since = TickCount();
    if (edits > std::numeric_limits<unsigned int>::max() - entry.dirty_edits)
        entry.dirty_edits = std::numeric_limits<unsigned int>::max();
    else
        entry.dirty_edits += edits;
}

void PlaylistStore::SaveEntry(Entry& entry) {
    const auto target = SlotPath(entry.slot);
    const auto temporary = std::filesystem::path(target.wstring() + L".tmp");
    entry.playlist.SaveTtbl(temporary);
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("cannot commit TTBL: " + std::to_string(error));
    }
    entry.dirty_edits = 0;
    entry.dirty_since = 0;
}

bool PlaylistStore::CompactSlots() {
    if (entries_.empty() || entries_.size() > kMaximumStoredSlots) return false;
    bool needed{};
    for (size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].slot >= kMaximumStoredSlots) return false;
        needed = needed || entries_[index].slot != index;
    }
    if (!needed) return true;

    std::vector<std::filesystem::path> staged;
    staged.reserve(entries_.size());
    try {
        for (size_t index = 0; index < entries_.size(); ++index) {
            if (entries_[index].slot == index) continue;
            const auto path = CompactionStagePath(directory_, index);
            staged.push_back(path);
            entries_[index].playlist.SaveTtbl(path);
        }
    } catch (const std::exception&) {
        std::error_code ignored;
        for (const auto& path : staged) std::filesystem::remove(path, ignored);
        return false;
    }

    const auto marker = CompactionMarkerPath(directory_);
    const auto marker_temporary =
        std::filesystem::path(marker.wstring() + L".tmp");
    {
        std::ofstream output(marker_temporary,
                             std::ios::binary | std::ios::trunc);
        output.write("TTC2", 4);
        WriteMarkerValue(output,
            static_cast<std::uint32_t>(entries_.size()));
        WriteMarkerValue(output, static_cast<std::uint32_t>(active_index_));
        for (const auto& entry : entries_)
            WriteMarkerValue(output, static_cast<std::uint32_t>(entry.slot));
        output.flush();
        output.close();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(marker_temporary, ignored);
            for (const auto& path : staged)
                std::filesystem::remove(path, ignored);
            return false;
        }
    }
    if (!MoveFileExW(marker_temporary.c_str(), marker.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(marker_temporary, ignored);
        for (const auto& path : staged) std::filesystem::remove(path, ignored);
        return false;
    }

    // Once the marker is durable, Load can finish any interrupted sequence.
    // Publish the compact slot identities in memory before committing files
    // so ActiveSlot and the persisted ActiveList setting stay in sync.
    for (size_t index = 0; index < entries_.size(); ++index)
        entries_[index].slot = index;
    if (RecoverCompaction(directory_)) return true;
    for (auto& entry : entries_) {
        if (entry.dirty_edits == 0) entry.dirty_since = TickCount();
        entry.dirty_edits = std::max(1U, entry.dirty_edits);
    }
    return false;
}

void PlaylistStore::FlushDirty(bool force) {
    // Complete a published transaction before any later edit is saved;
    // otherwise its older staged image could overwrite the new edit.
    if (!RecoverCompaction(directory_)) return;
    const auto now = TickCount();
    bool all_saved = true;
    for (auto& entry : entries_) {
        if (entry.dirty_edits == 0) continue;
        // Exact thresholds visible at 00480E75: elapsed > 29999 ms or edits > 4.
        if (!force && entry.dirty_edits <= 4 && now - entry.dirty_since < 30000) continue;
        try { SaveEntry(entry); }
        catch (const std::exception&) {
            // The original retains the dirty counter after a failed commit so
            // the next timer tick retries without destroying the old TTBL.
            all_saved = false;
        }
    }
    if (force && all_saved) static_cast<void>(CompactSlots());
}

} // namespace ttplayer::playlist
