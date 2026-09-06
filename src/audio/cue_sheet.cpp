#include "ttplayer/audio/cue_sheet.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <windows.h>

#include "ttplayer/audio/archive_member.h"

namespace ttplayer::audio {
namespace {

std::wstring DecodeCueText(std::span<const unsigned char> bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xfe) {
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
        static_cast<unsigned char>(bytes[2]) == 0xbf) offset = 3;
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
    return std::wstring(value);
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
    if (swscanf_s(terminated.c_str(), L"%lu:%lu:%lu%c", &minutes, &seconds,
                  &frames, &tail, 1) != 3 || seconds >= 60 || frames >= 75)
        return std::nullopt;
    constexpr std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max() / 75;
    if (minutes > maximum / 60) return std::nullopt;
    return (static_cast<std::uint64_t>(minutes) * 60 + seconds) * 75 + frames;
}

} // namespace

std::int64_t CueTrack::DurationMilliseconds() const noexcept {
    if (!end_frame || *end_frame < start_frame) return -1;
    return static_cast<std::int64_t>((*end_frame - start_frame) * 1000 / 75);
}

CueSheet CueSheet::Parse(std::span<const unsigned char> bytes,
                         const std::filesystem::path& source_path) {
    const std::wstring text = DecodeCueText(bytes);

    CueSheet sheet;
    std::filesystem::path current_file;
    CueTrack* current{};
    size_t begin{};
    while (begin <= text.size()) {
        const size_t end = text.find_first_of(L"\r\n", begin);
        auto line = std::wstring_view(text).substr(
            begin, end == std::wstring::npos ? text.size() - begin : end - begin);
        auto [command, argument] = SplitCommand(line);
        if (EqualsAscii(command, L"FILE")) {
            const auto filename = CueFileValue(argument);
            current_file = ResolveArchiveCueFile(source_path, filename);
            current = nullptr;
        } else if (EqualsAscii(command, L"TRACK")) {
            int number{};
            wchar_t type[32]{};
            const std::wstring terminated(argument);
            if (swscanf_s(terminated.c_str(), L"%d %31ls", &number, type,
                          static_cast<unsigned>(_countof(type))) == 2 &&
                number > 0 && _wcsicmp(type, L"AUDIO") == 0 &&
                !current_file.empty()) {
                sheet.tracks_.push_back({number, current_file, {},
                                         sheet.performer_, 0, std::nullopt});
                current = &sheet.tracks_.back();
            } else {
                current = nullptr;
            }
        } else if (EqualsAscii(command, L"TITLE") && current) {
            current->title = CueValue(argument);
        } else if (EqualsAscii(command, L"TITLE")) {
            sheet.title_ = CueValue(argument);
        } else if (EqualsAscii(command, L"PERFORMER")) {
            if (current) current->performer = CueValue(argument);
            else sheet.performer_ = CueValue(argument);
        } else if (EqualsAscii(command, L"INDEX") && current) {
            int index_number{};
            wchar_t time[64]{};
            const std::wstring terminated(argument);
            if (swscanf_s(terminated.c_str(), L"%d %63ls", &index_number,
                          time, static_cast<unsigned>(_countof(time))) == 2 &&
                index_number == 1) {
                if (const auto frame = ParseIndex(time)) current->start_frame = *frame;
            }
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
        if (begin < text.size() && text[end] == L'\r' && text[begin] == L'\n')
            ++begin;
    }

    for (size_t index = 0; index + 1 < sheet.tracks_.size(); ++index) {
        if (sheet.tracks_[index].audio_path == sheet.tracks_[index + 1].audio_path &&
            sheet.tracks_[index + 1].start_frame >= sheet.tracks_[index].start_frame)
            sheet.tracks_[index].end_frame = sheet.tracks_[index + 1].start_frame;
    }
    if (sheet.tracks_.empty()) throw std::runtime_error("CUE contains no AUDIO tracks");
    return sheet;
}

CueSheet CueSheet::Load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open CUE file");
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    return Parse(bytes, path);
}

CueSheet CueSheet::LoadFromMemory(
    std::span<const unsigned char> bytes,
    const std::filesystem::path& source_path) {
    if (source_path.empty())
        throw std::runtime_error("CUE source path is empty");
    return Parse(bytes, source_path);
}

const CueTrack* CueSheet::FindTrack(int one_based_number) const noexcept {
    const auto iterator = std::find_if(tracks_.begin(), tracks_.end(),
        [one_based_number](const CueTrack& track) {
            return track.number == one_based_number;
        });
    return iterator == tracks_.end() ? nullptr : &*iterator;
}

} // namespace ttplayer::audio
