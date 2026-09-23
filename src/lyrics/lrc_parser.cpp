#include "ttplayer/lyrics/lrc_parser.h"
#include "ttplayer/core/text.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <windows.h>

namespace ttplayer::lyrics {
std::optional<std::chrono::milliseconds> ParseLrcTimestamp(std::string_view value,
                                                        bool centisecond_rollover) {
    const auto colon = value.find(':'); const auto dot = value.find_first_of(".", colon);
    if (colon == value.npos) return std::nullopt;
    const auto number = [](std::string_view text, int& value) {
        if (text.empty() || text.find_first_not_of("0123456789") != text.npos) return false;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    };
    int minutes = 0, seconds = 0, fraction = 0;
    if (!number(value.substr(0, colon), minutes)) return std::nullopt;
    const auto second_end = dot == value.npos ? value.size() : dot;
    if (!number(value.substr(colon + 1, second_end - colon - 1), seconds) || seconds >= 60)
        return std::nullopt;
    if (dot != value.npos) {
        auto digits = value.substr(dot + 1);
        if (digits.size() > 3 || !number(digits, fraction)) return std::nullopt;
        if (digits.size() == 1) fraction *= 100; else if (digits.size() == 2) fraction *= 10;
        else if (centisecond_rollover && digits == "100") fraction = 1000;
    }
    return std::chrono::minutes(minutes) + std::chrono::seconds(seconds) + std::chrono::milliseconds(fraction);
}

bool HasCentisecondRollover(std::string_view content) {
    // Some centisecond exporters emit .100 instead of carrying into the next
    // second. Only opt in when an inline .100 moves BACKWARDS, two-digit tags
    // exist, and no other three-digit fraction establishes millisecond input.
    bool centiseconds = false, backwards = false;
    std::optional<std::chrono::milliseconds> previous;
    size_t cursor = 0;
    while (cursor < content.size()) {
        const auto open = content.find_first_of("[<\r\n", cursor);
        if (open == content.npos) break;
        cursor = open + 1;
        if (content[open] == '\r' || content[open] == '\n') { previous.reset(); continue; }
        const auto close = content.find(content[open] == '<' ? '>' : ']', cursor);
        if (close == content.npos) return false;
        const auto tag = content.substr(cursor, close - cursor);
        const auto time = ParseLrcTimestamp(tag);
        if (!time) continue;
        const auto dot = tag.find('.');
        if (dot != tag.npos) {
            const auto fraction = tag.substr(dot + 1);
            if (fraction.size() == 3 && fraction != "100") return false;
            centiseconds |= fraction.size() == 2;
            if (content[open] == '<' && fraction == "100" && previous && *time < *previous &&
                *time + std::chrono::milliseconds(900) >= *previous) backwards = true;
        }
        if (content[open] == '<') previous = *time;
        cursor = close + 1;
    }
    return centiseconds && backwards;
}

Lyrics ParseLrc(std::string_view content) {
    if (content.starts_with("\xEF\xBB\xBF")) content.remove_prefix(3);
    const bool rollover = HasCentisecondRollover(content);
    Lyrics result; std::istringstream input{std::string(content)}; std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t cursor = 0; std::vector<std::chrono::milliseconds> times;
        while (cursor < line.size() && line[cursor] == '[') {
            const auto end = line.find(']', cursor + 1); if (end == line.npos) break;
            const std::string_view tag(line.data() + cursor + 1, end - cursor - 1);
            if (const auto time = ParseLrcTimestamp(tag, rollover)) times.push_back(*time);
            else if (tag.starts_with("ti:")) result.title = std::string(tag.substr(3));
            else if (tag.starts_with("ar:")) result.artist = std::string(tag.substr(3));
            else if (tag.starts_with("al:")) result.album = std::string(tag.substr(3));
            else if (tag.starts_with("by:")) result.author = std::string(tag.substr(3));
            else if (tag.starts_with("offset:")) { int value{}; std::from_chars(tag.data()+7, tag.data()+tag.size(), value); result.offset = std::chrono::milliseconds(value); }
            cursor = end + 1;
        }
        if (times.empty()) continue;
        LyricLine parsed{*std::min_element(times.begin(), times.end()), {}};
        while (cursor < line.size()) {
            if (line[cursor] == '<') {
                const auto close = line.find('>', cursor + 1);
                if (close == line.npos) {
                    parsed.text.append(line, cursor, line.size() - cursor);
                    break;
                }
                if (close != line.npos) {
                    if (const auto time = ParseLrcTimestamp(
                            std::string_view(line).substr(cursor + 1, close - cursor - 1), rollover)) {
                        // Keep text order and prevent malformed timing from
                        // making the highlight retreat within a line.
                        const auto lower = parsed.words.empty() ? parsed.time : parsed.words.back().time;
                        parsed.words.push_back({std::max(*time, lower), parsed.text.size()});
                        cursor = close + 1;
                        continue;
                    }
                }
            }
            parsed.text += line[cursor++];
        }
        for (const auto time : times) {
            auto repeated = parsed;
            repeated.Shift(time - parsed.time);
            result.lines.push_back(std::move(repeated));
        }
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

    // 0043E237: after BOM/strict UTF-8 checks, use Big5 on a Big5 system,
    // GBK otherwise (including English and UTF-8 system locales).
    const UINT fallback = GetACP() == 950 ? 950 : 936;
    const int wide_count = MultiByteToWideChar(fallback, 0, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_count <= 0) throw std::runtime_error("unsupported lyric encoding");
    std::wstring wide(static_cast<size_t>(wide_count), L'\0');
    MultiByteToWideChar(fallback, 0, bytes.data(), static_cast<int>(bytes.size()),
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
    for (auto& line : lines) line.Shift(delta);
}

void LyricLine::Shift(std::chrono::milliseconds delta) noexcept {
    time += delta;
    for (auto& word : words) word.time += delta;
}

void LyricLine::TrimSpaces() {
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto begin = first == text.npos ? text.size() : first;
    const auto length = first == text.npos ? 0 : text.find_last_not_of(" \t\r\n") - begin + 1;
    for (auto& word : words)
        word.text_offset = std::min(length, word.text_offset > begin ? word.text_offset - begin : 0);
    text = text.substr(begin, length);
}

WordProgress LyricLine::Progress(std::chrono::milliseconds position,
                                std::chrono::milliseconds line_end) const {
    if (position < time) return {};
    size_t begin = 0, end = text.size();
    auto start = time, finish = line_end;
    for (const auto& word : words) {
        if (position < word.time) { end = word.text_offset; finish = std::min(finish, word.time); break; }
        begin = word.text_offset;
        start = word.time;
    }
    const auto duration = (finish - start).count();
    return {std::min(begin, text.size()), std::min(end, text.size()),
        duration <= 0 ? 1.0 : std::clamp(static_cast<double>((position - start).count()) /
                                       static_cast<double>(duration), 0.0, 1.0)};
}

std::string FormatLrcTimestamp(std::chrono::milliseconds time, bool word) {
    const auto ms = std::max<std::int64_t>(0, time.count());
    char text[64]{};
    std::snprintf(text, sizeof(text), "%c%02lld:%02lld.%03lld%c", word ? '<' : '[',
        ms / 60000, (ms % 60000) / 1000, ms % 1000, word ? '>' : ']');
    return text;
}

std::string TimedLyricText(const LyricLine& line, std::chrono::milliseconds offset) {
    std::string result;
    size_t cursor = 0;
    for (const auto& word : line.words) {
        const auto end = std::clamp(word.text_offset, cursor, line.text.size());
        result.append(line.text, cursor, end - cursor);
        result += FormatLrcTimestamp(word.time + offset, true);
        cursor = end;
    }
    result.append(line.text, cursor, line.text.size() - cursor);
    return result;
}
}
