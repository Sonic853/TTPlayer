#include "ttplayer/audio/cue_sheet.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <windows.h>

#include "ttplayer/audio/archive_member.h"

namespace ttplayer::audio {
namespace {

std::wstring DecodeCueText(std::span<const unsigned char> bytes, int& encoding) {
    encoding = 0;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xfe) {
        encoding = 2;
        if (bytes.size() % 2) throw std::runtime_error("truncated UTF-16 CUE");
        const size_t units = (bytes.size() - 2) / 2;
        std::wstring value(units, L'\0');
        for (size_t index = 0; index < units; ++index) {
            value[index] = static_cast<wchar_t>(
                static_cast<unsigned char>(bytes[2 + index * 2]) |
                (static_cast<unsigned char>(bytes[3 + index * 2]) << 8));
        }
        return value;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xfe &&
        static_cast<unsigned char>(bytes[1]) == 0xff) {
        encoding = 3;
        if (bytes.size() % 2) throw std::runtime_error("truncated UTF-16 CUE");
        const size_t units = (bytes.size() - 2) / 2;
        std::wstring value(units, L'\0');
        for (size_t index = 0; index < units; ++index) {
            value[index] = static_cast<wchar_t>(
                (static_cast<unsigned char>(bytes[2 + index * 2]) << 8) |
                static_cast<unsigned char>(bytes[3 + index * 2]));
        }
        return value;
    }

    size_t offset{};
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf) { offset = 3; encoding = 1; }
    if (bytes.size() - offset > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("CUE file is too large");
    const int input_length = static_cast<int>(bytes.size() - offset);
    if (input_length == 0) return {};
    const char* input = reinterpret_cast<const char*>(bytes.data() + offset);
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input,
                                     input_length, nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length == 0) {
        if (offset) throw std::runtime_error("invalid UTF-8 CUE");
        encoding = 4;
        code_page = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(code_page, flags, input, input_length,
                                     nullptr, 0);
    }
    if (length <= 0) throw std::runtime_error("unsupported CUE text encoding");
    std::wstring value(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(code_page, flags, input, input_length, value.data(), length);
    return value;
}

std::filesystem::path ResolveArchiveCueFile(
    const std::filesystem::path& source_path,
    std::wstring_view filename) {
    ArchiveMemberPath source;
    if (!ParseArchiveMemberPath(source_path.native(), source)) {
        auto path = std::filesystem::path(filename);
        if (filename.find(L"://") != std::wstring_view::npos) return path;
        if (path.is_relative()) path = source_path.parent_path() / path;
        return path.lexically_normal();
    }

    // An explicit virtual member or an absolute filesystem FILE directive is
    // already complete.  004E323B tests those spellings before it joins a
    // relative FILE value to the CUE source.
    ArchiveMemberPath explicit_member;
    if (ParseArchiveMemberPath(filename, explicit_member))
        return std::filesystem::path(filename);
    std::filesystem::path referenced(filename);
    if (referenced.has_root_name() || filename.find(L"://") !=
            std::wstring_view::npos) {
        return referenced;
    }

    std::wstring member_text = referenced.native();
    std::replace(member_text.begin(), member_text.end(), L'/', L'\\');
    const bool archive_rooted = !member_text.empty() &&
        (member_text.front() == L'\\' || member_text.front() == L'/');
    while (!member_text.empty() &&
           (member_text.front() == L'\\' || member_text.front() == L'/')) {
        member_text.erase(member_text.begin());
    }
    auto member = archive_rooted
        ? std::filesystem::path(member_text)
        : std::filesystem::path(source.member).parent_path() / member_text;
    if (member.empty() || member.has_root_name() || member.has_root_directory())
        throw std::runtime_error("invalid archive CUE FILE path");

    // 004E323B builds `archive|cue-directory\\FILE` with string append
    // operations.  It deliberately does not canonicalize `.` or `..` before
    // 0047E177 performs its case-insensitive, exact archive-member lookup.
    // Preserve that spelling here: MakeArchiveMemberPath would fold the path
    // and make a member playable when the original fails to find it.
    std::wstring logical = source.archive.wstring();
    logical.push_back(L'|');
    logical.append(member.native());
    return std::filesystem::path(std::move(logical));
}

std::wstring_view Trim(std::wstring_view value) {
    while (!value.empty() && iswspace(value.front())) value.remove_prefix(1);
    while (!value.empty() && iswspace(value.back())) value.remove_suffix(1);
    return value;
}

std::pair<std::wstring_view, std::wstring_view> SplitCommand(
    std::wstring_view line) {
    line = Trim(line);
    const size_t end = line.find_first_of(L" \t");
    if (end == std::wstring_view::npos) return {line, {}};
    return {line.substr(0, end), Trim(line.substr(end + 1))};
}

bool EqualsAscii(std::wstring_view value, const wchar_t* expected) {
    return _wcsicmp(std::wstring(value).c_str(), expected) == 0;
}

std::wstring CueValue(std::wstring_view value) {
    value = Trim(value);
    if (value.size() >= 2 && value.front() == L'"') {
        const size_t close = value.find(L'"', 1);
        if (close != std::wstring_view::npos)
            return std::wstring(value.substr(1, close - 1));
    }
    return std::wstring(value.substr(0, value.find_first_of(L" \t")));
}

std::wstring CueFileValue(std::wstring_view value) {
    value = Trim(value);
    if (!value.empty() && value.front() == L'"') return CueValue(value);
    const size_t end = value.find_first_of(L" \t");
    return std::wstring(value.substr(0, end));
}

std::optional<std::uint64_t> ParseIndex(std::wstring_view value) {
    unsigned long minutes{}, seconds{}, frames{};
    wchar_t tail{};
    const std::wstring terminated(Trim(value));
    if (terminated.empty() || terminated.find(L'-') != std::wstring::npos ||
        swscanf_s(terminated.c_str(), L"%lu:%lu:%lu%c", &minutes, &seconds,
                  &frames, &tail, 1) != 3)
        return std::nullopt;
    const auto result = (static_cast<std::uint64_t>(minutes) * 60 + seconds) * 75 + frames;
    // Keep the original non-normalized time tolerance, without its signed
    // overflow. The native CUE reader stores frame offsets in a signed DWORD.
    return result <= INT_MAX ? std::optional(result) : std::nullopt;
}

void SetField(CueMetadata& fields, std::wstring name, std::wstring value) {
    const auto found = std::find_if(fields.begin(), fields.end(),
        [&](const auto& item) { return EqualsAscii(item.first, name.c_str()); });
    if (found != fields.end()) {
        if (value.empty()) fields.erase(found);
        else found->second = std::move(value);
    } else if (!value.empty()) fields.emplace_back(std::move(name), std::move(value));
}

std::wstring GetField(const CueMetadata& fields, const wchar_t* name) {
    for (const auto& item : fields) if (EqualsAscii(item.first, name)) return item.second;
    return {};
}

std::vector<unsigned char> ReadCueBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open CUE file");
    const auto size = input.tellg();
    if (size < 0 || size > 16 * 1024 * 1024) throw std::runtime_error("CUE file is too large");
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("cannot read CUE file");
    return bytes;
}

std::vector<unsigned char> EncodeCueText(const std::wstring& text, int encoding) {
    std::vector<unsigned char> bytes;
    if (encoding == 2 || encoding == 3) {
        bytes = encoding == 2 ? std::vector<unsigned char>{0xff, 0xfe}
                              : std::vector<unsigned char>{0xfe, 0xff};
        for (const auto c : text) {
            bytes.push_back(static_cast<unsigned char>(encoding == 2 ? c : c >> 8));
            bytes.push_back(static_cast<unsigned char>(encoding == 2 ? c >> 8 : c));
        }
        return bytes;
    }
    BOOL replaced{};
    const bool ansi = encoding == 4 && GetACP() != CP_UTF8;
    const auto code_page = ansi ? CP_ACP : CP_UTF8;
    const auto flags = ansi ? WC_NO_BEST_FIT_CHARS : 0U;
    const int length = WideCharToMultiByte(code_page, flags, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, ansi ? &replaced : nullptr);
    if (!length && !text.empty()) throw std::runtime_error("cannot encode CUE");
    if (replaced) return EncodeCueText(text, 1); // Never silently lose edited Unicode.
    if (encoding == 1) bytes = {0xef, 0xbb, 0xbf};
    const auto offset = bytes.size();
    bytes.resize(offset + length);
    if (length && WideCharToMultiByte(code_page, flags, text.data(),
            static_cast<int>(text.size()), reinterpret_cast<char*>(bytes.data() + offset),
            length, nullptr, ansi ? &replaced : nullptr) != length)
        throw std::runtime_error("cannot encode CUE");
    return bytes;
}

[[noreturn]] void CueIoError(DWORD error) {
    throw std::system_error(static_cast<int>(error), std::system_category(), "CUE write");
}

} // namespace

std::int64_t CueTrack::DurationMilliseconds() const noexcept {
    if (!has_index01 || !end_frame || *end_frame < start_frame) return -1;
    return static_cast<std::int64_t>((*end_frame - start_frame) * 1000 / 75);
}

CueSheet CueSheet::Parse(std::span<const unsigned char> bytes,
                         const std::filesystem::path& source_path) {
    CueSheet sheet;
    if (bytes.size() > 16 * 1024 * 1024) throw std::runtime_error("CUE file is too large");
    const std::wstring text = DecodeCueText(bytes, sheet.encoding_);
    sheet.original_bytes_.assign(bytes.begin(), bytes.end());
    sheet.source_path_ = source_path;
    std::wstring current_file;
    std::optional<CueTrack> current;
    bool in_track{};
    const auto finish = [&] {
        if (current && !current->file_reference.empty() &&
            (current->has_index01 || current->index00)) {
            current->last_line = sheet.lines_.size();
            current->number = static_cast<int>(sheet.tracks_.size() + 1);
            sheet.tracks_.push_back(std::move(*current));
        }
        current.reset();
    };
    size_t begin{};
    while (begin < text.size()) {
        const size_t end = text.find_first_of(L"\r\n", begin);
        auto line = std::wstring_view(text).substr(
            begin, end == std::wstring::npos ? text.size() - begin : end - begin);
        auto [command, argument] = SplitCommand(line);
        if (EqualsAscii(command, L"FILE")) {
            finish();
            const auto filename = CueFileValue(argument);
            current_file = _wcsicmp(std::filesystem::path(filename).extension().c_str(), L".cue") == 0
                ? std::wstring{} : filename;
        } else if (EqualsAscii(command, L"TRACK")) {
            finish();
            in_track = true;
            int number{};
            wchar_t type[32]{};
            const std::wstring terminated(argument);
            if (swscanf_s(terminated.c_str(), L"%d %31ls", &number, type,
                          static_cast<unsigned>(_countof(type))) == 2 &&
                number > 0 && _wcsicmp(type, L"AUDIO") == 0 &&
                !current_file.empty()) {
                current.emplace();
                current->source_number = number;
                current->file_reference = current_file;
                current->audio_path = ResolveArchiveCueFile(source_path, current_file);
                current->first_line = sheet.lines_.size();
            }
        } else if (EqualsAscii(command, L"TITLE") && current) {
            SetField(current->metadata, L"Title", CueValue(argument));
        } else if (EqualsAscii(command, L"TITLE") && !in_track) {
            sheet.title_ = CueValue(argument);
        } else if (EqualsAscii(command, L"PERFORMER")) {
            if (current) SetField(current->metadata, L"Artist", CueValue(argument));
            else if (!in_track) sheet.performer_ = CueValue(argument);
        } else if (EqualsAscii(command, L"REM") && current) {
            const auto [key, value] = SplitCommand(argument);
            if (!key.empty()) SetField(current->metadata, std::wstring(key), CueValue(value));
        } else if (EqualsAscii(command, L"INDEX") && current) {
            int index_number{};
            wchar_t time[64]{};
            const std::wstring terminated(argument);
            if (swscanf_s(terminated.c_str(), L"%d %63ls", &index_number,
                          time, static_cast<unsigned>(_countof(time))) == 2 &&
                (index_number == 0 || index_number == 1)) {
                if (const auto frame = ParseIndex(time)) {
                    if (index_number == 0) current->index00 = *frame;
                    else { current->start_frame = *frame; current->has_index01 = true; }
                }
            }
        }
        size_t next = end == std::wstring::npos ? text.size() : end + 1;
        if (next < text.size() && text[end] == L'\r' && text[next] == L'\n') ++next;
        sheet.lines_.push_back({std::wstring(line), end == std::wstring::npos
            ? std::wstring{} : text.substr(end, next - end)});
        if (end == std::wstring::npos) break;
        begin = next;
    }
    finish();
    for (size_t index = 0; index < sheet.tracks_.size(); ++index) {
        auto& track = sheet.tracks_[index];
        track.title = GetField(track.metadata, L"Title");
        track.performer = GetField(track.metadata, L"Artist");
        if (track.performer.empty()) track.performer = sheet.performer_;
        SetField(track.metadata, L"Artist", track.performer);
        SetField(track.metadata, L"Album", sheet.title_);
        SetField(track.metadata, L"Tracknumber", std::to_wstring(track.number));
        if (index + 1 < sheet.tracks_.size()) {
            const auto& next = sheet.tracks_[index + 1];
            if (track.has_index01 && next.has_index01 &&
                EqualsAscii(track.file_reference, next.file_reference.c_str()) &&
                next.start_frame > track.start_frame)
                track.end_frame = next.start_frame;
        }
    }
    if (sheet.tracks_.empty()) throw std::runtime_error("CUE contains no AUDIO tracks");
    return sheet;
}

CueSheet CueSheet::Load(const std::filesystem::path& path) {
    return Parse(ReadCueBytes(path), path);
}

CueSheet CueSheet::LoadFromMemory(
    std::span<const unsigned char> bytes,
    const std::filesystem::path& source_path) {
    if (source_path.empty())
        throw std::runtime_error("CUE source path is empty");
    auto sheet = Parse(bytes, source_path);
    sheet.from_memory_ = true;
    return sheet;
}

const CueTrack* CueSheet::FindTrack(int one_based_number) const noexcept {
    return one_based_number > 0 && static_cast<size_t>(one_based_number) <= tracks_.size()
        ? &tracks_[static_cast<size_t>(one_based_number) - 1] : nullptr;
}

const CueTrack* CueSheet::FindSourceTrack(int number) const noexcept {
    const CueTrack* found{};
    for (const auto& track : tracks_) if (track.source_number == number) {
        if (found) return nullptr; // Ambiguous legacy data must not select a different song.
        found = &track;
    }
    return found;
}

std::vector<std::filesystem::path> CueSheet::AudioCandidates(const CueTrack& track) const {
    std::vector<std::filesystem::path> result{track.audio_path};
    for (const auto& extension : {track.audio_path.extension().wstring(),
                                  std::wstring(L".ape"), std::wstring(L".tak")}) {
        auto candidate = source_path_;
        candidate.replace_extension(extension);
        if (_wcsicmp(candidate.extension().c_str(), L".cue") == 0) continue;
        if (std::none_of(result.begin(), result.end(), [&](const auto& path) {
                return _wcsicmp(path.c_str(), candidate.c_str()) == 0;
            })) result.push_back(std::move(candidate));
    }
    return result;
}

bool CueSheet::IsWritableField(std::wstring_view name) noexcept {
    if (name.empty() || name.size() > 255) return false;
    if ((name.size() == 11 && _wcsnicmp(name.data(), L"Tracknumber", 11) == 0) ||
        (name.size() == 6 && _wcsnicmp(name.data(), L"Lyrics", 6) == 0)) return false;
    return std::all_of(name.begin(), name.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
               (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
    });
}

void CueSheet::WriteTrackMetadata(int subtrack, const CueMetadata& fields) const {
    ArchiveMemberPath archive;
    if (from_memory_ || ParseArchiveMemberPath(source_path_.native(), archive))
        CueIoError(ERROR_ACCESS_DENIED);
    const auto* track = FindTrack(subtrack);
    if (!track) CueIoError(ERROR_INVALID_PARAMETER);
    // Serialize our property and background ReplayGain writers across
    // processes. A waiting writer still has to pass the snapshot comparison.
    auto lock_key = std::filesystem::absolute(source_path_).lexically_normal().wstring();
    CharLowerBuffW(lock_key.data(), static_cast<DWORD>(lock_key.size()));
    uint64_t hash = 14695981039346656037ULL;
    for (const auto ch : lock_key) { hash ^= static_cast<uint16_t>(ch); hash *= 1099511628211ULL; }
    const auto mutex_name = L"Local\\TTPlayer.CueWriter." + std::to_wstring(hash);
    struct Mutex {
        HANDLE handle{}; bool owned{};
        ~Mutex() { if (owned) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
    } mutex{CreateMutexW(nullptr, FALSE, mutex_name.c_str())};
    if (!mutex.handle) CueIoError(GetLastError());
    const DWORD wait = WaitForSingleObject(mutex.handle, 3000);
    mutex.owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (!mutex.owned) CueIoError(wait == WAIT_TIMEOUT ? ERROR_LOCK_VIOLATION : GetLastError());
    const auto attributes = GetFileAttributesW(source_path_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) CueIoError(GetLastError());
    if (attributes & FILE_ATTRIBUTE_READONLY) CueIoError(ERROR_ACCESS_DENIED);
    auto lines = lines_;
    CueMetadata changes;
    for (const auto& [key, value] : fields) {
        if (!IsWritableField(key) || value.find_first_of(L"\r\n\"") != std::wstring::npos ||
            value.find(L'\0') != std::wstring::npos) CueIoError(ERROR_INVALID_PARAMETER);
        // Keep empty values: they mean removal, not absence of an edit.
        auto found = std::find_if(changes.begin(), changes.end(), [&](const auto& field) {
            return EqualsAscii(field.first, key.c_str());
        });
        if (found == changes.end()) changes.emplace_back(key, value);
        else found->second = value;
    }
    const auto newline = [&]() -> std::wstring {
        for (const auto& line : lines) if (!line.ending.empty()) return line.ending;
        return L"\r\n";
    }();
    std::vector<bool> removed(lines.size());
    std::vector<std::wstring> before(lines.size() + 1);
    size_t global_end{};
    while (global_end < lines.size()) {
        const auto [cmd, arg] = SplitCommand(lines[global_end].text);
        if (EqualsAscii(cmd, L"FILE") || EqualsAscii(cmd, L"TRACK")) break;
        ++global_end;
    }
    for (const auto& [key, value] : changes) {
        const bool album = EqualsAscii(key, L"Album");
        const bool artist = EqualsAscii(key, L"Artist");
        const bool title = EqualsAscii(key, L"Title");
        const auto first = album ? 0 : track->first_line + 1;
        const auto last = album ? global_end : track->last_line;
        const std::wstring prefix = album ? L"TITLE " : artist ? L"    PERFORMER " :
            title ? L"    TITLE " : L"    REM " + key + L" ";
        const std::wstring replacement = value.empty() ? std::wstring{} : prefix + L"\"" + value + L"\"";
        bool written{};
        for (size_t i = first; i < last; ++i) {
            const auto [cmd, arg] = SplitCommand(lines[i].text);
            const auto [rem, rem_value] = SplitCommand(arg);
            const bool matches = (album || title) && EqualsAscii(cmd, L"TITLE") ||
                artist && EqualsAscii(cmd, L"PERFORMER") ||
                (!album && EqualsAscii(cmd, L"REM") && EqualsAscii(rem, key.c_str()));
            if (!matches) continue;
            if (written || replacement.empty()) removed[i] = true;
            else { lines[i].text = replacement; written = true; }
        }
        if (!written && !replacement.empty()) before[first] += replacement + newline;
        // Removing a per-track artist restores the global PERFORMER fallback.
    }
    std::wstring text;
    for (size_t i = 0; i <= lines.size(); ++i) {
        if (!before[i].empty() && !text.empty() && text.back() != L'\r' && text.back() != L'\n') text += newline;
        text += before[i];
        if (i < lines.size() && !removed[i]) text += lines[i].text + lines[i].ending;
    }
    const auto bytes = EncodeCueText(text, encoding_);
    const auto verify = Parse(bytes, source_path_);
    if (verify.Tracks().size() != tracks_.size()) CueIoError(ERROR_INVALID_DATA);
    if (ReadCueBytes(source_path_) != original_bytes_) CueIoError(ERROR_REVISION_MISMATCH);
    wchar_t temporary[MAX_PATH + 1]{};
    const auto directory = std::filesystem::absolute(source_path_).parent_path();
    if (!GetTempFileNameW(directory.c_str(), L"cue", 0, temporary)) CueIoError(GetLastError());
    struct Cleanup { const wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{temporary};
    const HANDLE output = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) CueIoError(GetLastError());
    DWORD count{};
    const bool okay = WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
        count == bytes.size() && FlushFileBuffers(output);
    const DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    CloseHandle(output);
    if (!okay) CueIoError(error ? error : ERROR_WRITE_FAULT);
    for (unsigned attempt = 0; ; ++attempt) {
        if (ReadCueBytes(source_path_) != original_bytes_) CueIoError(ERROR_REVISION_MISMATCH);
        if (ReplaceFileW(source_path_.c_str(), temporary, nullptr, 0, nullptr, nullptr)) break;
        const DWORD replaced = GetLastError();
        if (attempt >= 3 || (replaced != ERROR_SHARING_VIOLATION &&
            replaced != ERROR_LOCK_VIOLATION && replaced != ERROR_UNABLE_TO_REMOVE_REPLACED))
            CueIoError(replaced);
        Sleep(25 * (attempt + 1));
    }
}

} // namespace ttplayer::audio
