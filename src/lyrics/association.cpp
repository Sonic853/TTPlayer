#include "ttplayer/lyrics/association.h"
#include "ttplayer/core/text.h"
#include <fstream>
#include <sstream>
#include <windows.h>

namespace ttplayer::lyrics {
bool SongKeyLess::operator()(const SongKey& a, const SongKey& b) const noexcept {
    const int comparison = _wcsicmp(a.path.c_str(), b.path.c_str());
    return comparison < 0 || (comparison == 0 && a.subtrack < b.subtrack);
}
bool SongKey::operator==(const SongKey& other) const noexcept {
    return subtrack == other.subtrack && !_wcsicmp(path.c_str(), other.path.c_str());
}
namespace {
bool Field(const std::filesystem::path& path) {
    const auto& value = path.native();
    return !value.empty() && value.find_first_of(L"|\r\n") == value.npos &&
        value.find(L'\0') == value.npos;
}
}
bool AssociationStore::Load(const std::filesystem::path& file) {
    entries_.clear(); dirty_ = false; file_.clear();
    std::error_code error;
    if (!std::filesystem::exists(file, error) && !error) { file_ = file; return true; }
    std::ifstream input(file, std::ios::binary);
    if (!input) return false; // Do not overwrite an unreadable association file.
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) return false;
    try {
        std::wstring text;
        if (bytes.starts_with("\xef\xbb\xbf")) text = core::Utf8ToWide(bytes.substr(3));
        else if (!bytes.empty()) {
            const int size = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (!size) return false;
            text.resize(size);
            MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), size);
        }
        std::wistringstream lines(text);
        for (std::wstring line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            const auto first = line.find(L'|');
            if (first == line.npos || !first) continue;
            const auto second = line.find(L'|', first + 1);
            SongKey key{line.substr(0, first), 0};
            if (second != line.npos) {
                const auto number = line.substr(first + 1, second - first - 1);
                size_t used{};
                try { key.subtrack = std::stoi(number, &used); }
                catch (...) { continue; }
                if (used != number.size() || key.subtrack < 0) continue;
            }
            const std::filesystem::path value = line.substr((second == line.npos ? first : second) + 1);
            if (Field(key.path) && Field(value)) entries_.insert_or_assign(std::move(key), value);
        }
        file_ = file;
        return true;
    } catch (...) { entries_.clear(); return false; }
}
std::optional<std::filesystem::path> AssociationStore::Find(const SongKey& song) const {
    const auto found = entries_.find(song);
    return found == entries_.end() ? std::nullopt : std::optional(found->second);
}
bool AssociationStore::Set(const SongKey& song, const std::filesystem::path& lyric) {
    if (!Field(song.path) || !Field(lyric) || song.subtrack < 0) return false;
    if (Find(song) == lyric) return false;
    entries_.insert_or_assign(song, lyric); dirty_ = true; return true;
}
bool AssociationStore::Erase(const SongKey& song) {
    if (!entries_.erase(song)) return false;
    dirty_ = true; return true;
}
bool AssociationStore::Save() {
    if (!dirty_) return true;
    if (file_.empty()) return false;
    if (entries_.empty()) {
        std::error_code error;
        std::filesystem::remove(file_, error);
        if (error) return false;
        dirty_ = false; return true;
    }
    // Same-directory temporary file preserves the old table on a failed write.
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(file_.parent_path().c_str(), L"rll", 0, temporary)) return false;
    bool okay = false;
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "\xef\xbb\xbf";
        for (const auto& [key, value] : entries_)
            output << core::WideToUtf8(key.path.native()) << '|' << key.subtrack << '|'
                   << core::WideToUtf8(value.native()) << "\r\n";
        output.flush(); okay = output.good(); output.close(); okay &= !output.fail();
        if (okay) okay = MoveFileExW(temporary, file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) { okay = false; }
    if (!okay) DeleteFileW(temporary);
    else dirty_ = false;
    return okay;
}
} // namespace ttplayer::lyrics
