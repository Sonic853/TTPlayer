#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttplayer::playlist {
// FUN_004C54D1 resolves StrCmpLogicalW at run time and falls back to
// lstrcmpiW when it is unavailable.  Playlist and catalogue sorting share
// this comparison so numeric text has the same order in both controls.
[[nodiscard]] int CompareLegacyLogicalText(const wchar_t* left,
                                           const wchar_t* right) noexcept;

enum class PlayMode { sequential, repeat_all, repeat_one, shuffle };
struct Track {
    std::filesystem::path path;
    std::string title;
    std::string artist;
    // CPlayItem +0x10: -2 means not read yet, -1 means reader/open failed,
    // and non-negative values are the known duration in milliseconds.
    int duration_ms{-2};
    int subtrack{}; // One-based CUE TRACK number; zero means the whole file.
    std::string album;
    // CPlayItem's persisted 0x40 field.  TTPlayer accepts zero (unrated) and
    // the five values exposed by commands 32741..32745.
    int rating{};
    int track_number{};
    // Values used by the native title formatter's %C and %B fields.  The
    // creator type may contain a pipe-delimited description; display uses
    // only its first component.
    std::string media_type;
    std::uint32_t bitrate_bps{};
    // CPlayItem's arbitrary metadata bag (the TTBL 0x20 key/value segment).
    // Keep original spellings/order while lookups remain ASCII-insensitive.
    std::vector<std::pair<std::string, std::string>> metadata;
    // Runtime decoder information used by %(Format). It is not part of the
    // v5 TTBL item payload and is repopulated when sound information is read.
    std::uint32_t sample_rate_hz{};
};

enum class SortKey {
    display_title,
    file_name,
    path,
    album,
    rating,
    file_time,
    track_number,
    duration,
};

struct LoadOptions {
    // CSettings /PlayList IgnoreBadFiles. The native default is zero, so
    // callers which do not supply settings retain unresolved local entries.
    bool ignore_bad_files{};
};

class Playlist {
public:
    void Add(Track track);
    void Insert(size_t index, Track track);
    void InsertRange(size_t index, std::vector<Track> tracks);
    bool Remove(size_t index);
    void Clear();
    // Returns an old-index -> new-index map for callers which need to follow
    // item identity.  The native ListCtrl itself keeps selected/focused row
    // numbers across SortItems; only the persisted playing item is remapped.
    std::vector<size_t> Sort(
        SortKey key, bool ascending,
        std::span<const std::wstring> display_titles = {});
    void SortByDisplayTitle();
    void SortByFileName();
    std::vector<size_t> Shuffle();
    std::set<size_t> Reorder(const std::set<size_t>& selected, size_t insertion);
    bool SetDuration(size_t index, int duration_ms);
    bool SetMetadata(size_t index, std::string title, std::string artist,
                     std::string album);
    bool SetExtendedMetadata(
        size_t index,
        std::vector<std::pair<std::string, std::string>> metadata,
        std::string media_type, std::uint32_t bitrate_bps,
        std::uint32_t sample_rate_hz = 0);
    bool SetRating(size_t index, int rating);
    bool SetPath(size_t index, std::filesystem::path path);
    [[nodiscard]] const std::vector<Track>& Tracks() const noexcept { return tracks_; }
    [[nodiscard]] const std::wstring& Title() const noexcept { return title_; }
    void SetTitle(std::wstring title) { title_ = std::move(title); }
    [[nodiscard]] std::optional<size_t> CurrentRow() const noexcept {
        return current_row_;
    }
    bool SetCurrentRow(std::optional<size_t> row) noexcept;
    [[nodiscard]] std::optional<size_t> PlayingRow() const noexcept {
        return playing_row_;
    }
    bool SetPlayingRow(std::optional<size_t> row) noexcept;
    [[nodiscard]] std::optional<size_t> Next(size_t current, PlayMode mode);
    [[nodiscard]] std::optional<size_t> Previous(size_t current, PlayMode mode);
    void LoadM3u8(const std::filesystem::path& path,
                  const LoadOptions& options = {});
    void SaveM3u8(const std::filesystem::path& path) const;
    void LoadTtbl(const std::filesystem::path& path);
    void SaveTtbl(const std::filesystem::path& path) const;
    void LoadXml(const std::filesystem::path& path,
                 const LoadOptions& options = {});
    void SaveXml(const std::filesystem::path& path,
                 std::wstring_view tag_title_format = L"%A - %T",
                 std::wstring_view default_title_format = L"%F",
                 bool save_relative_path = true,
                 bool save_tags = false) const;
    void LoadFromFile(const std::filesystem::path& path,
                      const LoadOptions& options = {});
    void SaveToFile(const std::filesystem::path& path,
                    std::wstring_view tag_title_format = L"%A - %T",
                    std::wstring_view default_title_format = L"%F",
                    bool save_relative_path = true,
                    bool save_tags = false) const;
private:
    std::vector<Track> tracks_;
    std::wstring title_;
    // CPlayList +0x1c.  This is the last-playing item and the fourth DWORD in
    // TTBL v2+, not the ListCtrl caret stored at runtime in +0x20.
    std::optional<size_t> playing_row_;
    // CPlayList +0x20.  This is transient per-list UI focus/selection state.
    std::optional<size_t> current_row_;
};
}
