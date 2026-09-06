#include "ttplayer/lyrics/lrc_parser.h"
#include "ttplayer/core/text.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <windows.h>

namespace ttplayer::lyrics {
namespace {
bool Timestamp(std::string_view value, std::chrono::milliseconds& result) {
    const auto colon = value.find(':'); const auto dot = value.find_first_of(".", colon);
    if (colon == value.npos) return false;
    int minutes = 0, seconds = 0, fraction = 0;
    if (std::from_chars(value.data(), value.data() + colon, minutes).ec != std::errc{}) return false;
    const auto second_end = dot == value.npos ? value.size() : dot;
    if (std::from_chars(value.data() + colon + 1, value.data() + second_end, seconds).ec != std::errc{}) return false;
    if (dot != value.npos) {
        auto digits = value.substr(dot + 1, 3);
        std::from_chars(digits.data(), digits.data() + digits.size(), fraction);
        if (digits.size() == 1) fraction *= 100; else if (digits.size() == 2) fraction *= 10;
    }
    result = std::chrono::minutes(minutes) + std::chrono::seconds(seconds) + std::chrono::milliseconds(fraction);
    return seconds < 60 && minutes >= 0;
}
}

Lyrics ParseLrc(std::string_view content) {
    Lyrics result; std::istringstream input{std::string(content)}; std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t cursor = 0; std::vector<std::chrono::milliseconds> times;
        while (cursor < line.size() && line[cursor] == '[') {
            const auto end = line.find(']', cursor + 1); if (end == line.npos) break;
            const std::string_view tag(line.data() + cursor + 1, end - cursor - 1);
            std::chrono::milliseconds time;
            if (Timestamp(tag, time)) times.push_back(time);
            else if (tag.starts_with("ti:")) result.title = std::string(tag.substr(3));
            else if (tag.starts_with("ar:")) result.artist = std::string(tag.substr(3));
            else if (tag.starts_with("al:")) result.album = std::string(tag.substr(3));
            else if (tag.starts_with("by:")) result.author = std::string(tag.substr(3));
            else if (tag.starts_with("offset:")) { int value{}; std::from_chars(tag.data()+7, tag.data()+tag.size(), value); result.offset = std::chrono::milliseconds(value); }
            cursor = end + 1;
        }
        const std::string text = line.substr(cursor);
        for (const auto time : times) result.lines.push_back({time, text});
    }
    std::stable_sort(result.lines.begin(), result.lines.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    return result;
}

Lyrics LoadLrc(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open lyric file");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.starts_with("\xEF\xBB\xBF"))
        return ParseLrc(std::string_view(bytes).substr(3));

    if (bytes.size() >= 2 &&
        ((static_cast<unsigned char>(bytes[0]) == 0xff &&
          static_cast<unsigned char>(bytes[1]) == 0xfe) ||
         (static_cast<unsigned char>(bytes[0]) == 0xfe &&
          static_cast<unsigned char>(bytes[1]) == 0xff))) {
        const bool big_endian = static_cast<unsigned char>(bytes[0]) == 0xfe;
        std::wstring wide;
        wide.reserve((bytes.size() - 2) / 2);
        for (size_t offset = 2; offset + 1 < bytes.size(); offset += 2) {
            const unsigned char first = static_cast<unsigned char>(bytes[offset]);
            const unsigned char second = static_cast<unsigned char>(bytes[offset + 1]);
            wide.push_back(static_cast<wchar_t>(big_endian
                ? (first << 8) | second : first | (second << 8)));
        }
        return ParseLrc(core::WideToUtf8(wide));
    }

    const int utf8_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (utf8_count > 0 || bytes.empty()) return ParseLrc(bytes);

    // The 5.7-era local lyric collection commonly contains ANSI/GBK files.
    // The original reader falls back to the process ANSI code page when no
    // BOM/valid UTF-8 sequence is present.
    const int wide_count = MultiByteToWideChar(CP_ACP, 0, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_count <= 0) throw std::runtime_error("unsupported lyric encoding");
    std::wstring wide(static_cast<size_t>(wide_count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()),
                        wide.data(), wide_count);
    return ParseLrc(core::WideToUtf8(wide));
}
std::optional<size_t> Lyrics::LineAt(std::chrono::milliseconds position) const {
    position -= offset;
    const auto it = std::upper_bound(lines.begin(), lines.end(), position,
        [](auto value, const LyricLine& line) { return value < line.time; });
    if (it == lines.begin()) return std::nullopt;
    return static_cast<size_t>(std::distance(lines.begin(), it) - 1);
}

void Lyrics::ShiftLines(std::chrono::milliseconds delta) noexcept {
    // CLyric::AdjustTime (0043D7D0), reached from CLyricCtrl's wheel handler
    // at 00442A97, adds the same signed value to every timestamp.  It does not
    // seek the audio engine and it deliberately leaves the parsed [offset:]
    // metadata untouched.
    for (auto& line : lines) line.time += delta;
}
}
