#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace ttplayer::lyrics {

// 00411C72/0041B553: a CPlayItem identity, not its title or server result ID.
struct SongKey {
    std::filesystem::path path;
    int subtrack{};
    bool operator==(const SongKey& other) const noexcept;
};
struct SongKeyLess {
    bool operator()(const SongKey& left, const SongKey& right) const noexcept;
};

// DAT_0053B9A4. Empty (no entry) and "\\" have different meanings.
inline constexpr wchar_t kNoLyric[] = L"\\";

class AssociationStore {
public:
    using Entries = std::map<SongKey, std::filesystem::path, SongKeyLess>;
    // 004BE9C8 accepts UTF-8 BOM / ACP, and both two- and three-field rows.
    bool Load(const std::filesystem::path& file);
    // 00453AF6: UTF-8 BOM, media|subtrack|lyric, CRLF; dirty-only writes.
    bool Save();
    std::optional<std::filesystem::path> Find(const SongKey& song) const;
    bool Set(const SongKey& song, const std::filesystem::path& lyric);
    bool Erase(const SongKey& song);
    const Entries& All() const noexcept { return entries_; }
    bool Loaded() const noexcept { return !file_.empty(); }
    bool Dirty() const noexcept { return dirty_; }
private:
    std::filesystem::path file_;
    Entries entries_;
    bool dirty_{};
};

} // namespace ttplayer::lyrics
