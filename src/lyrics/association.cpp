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
    return !value.empty() && value.find_first_of(L"\r\n") == value.npos &&
        value.find(L'\0') == value.npos;
}
constexpr std::wstring_view kEscaped = L"@TTPlayerLyricAssociation/2|";
std::wstring Escape(std::wstring_view value) {
    std::wstring out;
    for (const auto c : value) {
        if (c == L'%') out += L"%25";
        else if (c == L'|') out += L"%7C";
        else out += c;
    }
    return out;
}
bool Unescape(std::wstring& value) {
    std::wstring out;
    for (size_t i=0; i<value.size(); ++i) {
        if (value[i] != L'%') out += value[i];
        else if (value.compare(i,3,L"%25") == 0) { out += L'%'; i+=2; }
        else if (value.compare(i,3,L"%7C") == 0) { out += L'|'; i+=2; }
        else return false;
    }
    value = std::move(out); return true;
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
            const bool escaped = line.starts_with(kEscaped);
            if (escaped) line.erase(0,kEscaped.size());
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
            auto value_text = line.substr((second == line.npos ? first : second) + 1);
            if (value_text.find(L'|') != value_text.npos) continue;
            if (escaped) {
                auto key_text = key.path.native();
                if (second == line.npos || !Unescape(key_text) || !Unescape(value_text)) continue;
                key.path = key_text;
            }
            const std::filesystem::path value = value_text;
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
bool AssociationStore::RetargetSource(const std::filesystem::path& before,
                                      const std::filesystem::path& after) {
    if (!Field(before) || !Field(after) || !_wcsicmp(before.c_str(), after.c_str())) return false;
    Entries moved = entries_;
    bool changed{};
    for (const auto& [song, lyric] : entries_) {
        if (_wcsicmp(song.path.c_str(), before.c_str())) continue;
        moved.erase(song);
        moved.insert_or_assign(SongKey{after, song.subtrack}, lyric);
        changed = true;
    }
    if (changed) { entries_.swap(moved); dirty_ = true; }
    return changed;
}
bool AssociationStore::Save() {
    return static_cast<bool>(SaveChecked());
}
SaveResult AssociationStore::SaveChecked() {
    if (!dirty_) return {};
    if (file_.empty()) return SaveResult::Win32Failure(file_, ERROR_INVALID_NAME);
    if (entries_.empty()) {
        std::error_code error;
        std::filesystem::remove(file_, error);
        if (error) return SaveResult::Win32Failure(file_, static_cast<DWORD>(error.value()));
        dirty_ = false; return {};
    }
    // Same-directory temporary file preserves the old table on a failed write.
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(file_.parent_path().c_str(), L"rll", 0, temporary))
        return SaveResult::Win32Failure(file_);
    bool okay = false;
    DWORD failure = ERROR_WRITE_FAULT;
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "\xef\xbb\xbf";
        for (const auto& [key, value] : entries_) {
            const bool escaped = key.path.native().find(L'|') != std::wstring::npos ||
                value.native().find(L'|') != std::wstring::npos || key.path.native().starts_with(kEscaped);
            if (escaped) output << core::WideToUtf8(kEscaped);
            output << core::WideToUtf8(escaped ? Escape(key.path.native()) : key.path.native()) << '|' << key.subtrack << '|'
                   << core::WideToUtf8(escaped ? Escape(value.native()) : value.native()) << "\r\n";
        }
        output.flush(); okay = output.good(); output.close(); okay &= !output.fail();
        if (okay) {
            okay = MoveFileExW(temporary, file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            if (!okay) failure = GetLastError();
        }
    } catch (...) { okay = false; }
    if (!okay) DeleteFileW(temporary);
    else dirty_ = false;
    return okay ? SaveResult{} : SaveResult::Win32Failure(file_, failure);
}
} // namespace ttplayer::lyrics
