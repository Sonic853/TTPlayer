#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::lyrics {
// Offsets refer to UTF-8 bytes in the display text, never to markup. A marker
// at text.size() is an explicit end time (and has no visible characters).
struct WordTime { std::chrono::milliseconds time; size_t text_offset{}; };
struct WordProgress { size_t begin{}, end{}; double fraction{}; };
struct LyricLine {
    std::chrono::milliseconds time;
    std::string text;
    std::vector<WordTime> words;
    void Shift(std::chrono::milliseconds delta) noexcept;
    void TrimSpaces();
    [[nodiscard]] WordProgress Progress(std::chrono::milliseconds position,
                                       std::chrono::milliseconds end) const;
};
struct Lyrics {
    std::string title, artist, album, author;
    std::chrono::milliseconds offset{};
    std::vector<LyricLine> lines;
    [[nodiscard]] std::optional<size_t> LineAt(std::chrono::milliseconds position) const;
    void ShiftLines(std::chrono::milliseconds delta) noexcept;
};
Lyrics ParseLrc(std::string_view content);
Lyrics LoadLrc(const std::filesystem::path& path);
std::optional<std::chrono::milliseconds> ParseLrcTimestamp(std::string_view value,
                                                        bool centisecond_rollover = false);
bool HasCentisecondRollover(std::string_view content);
std::string FormatLrcTimestamp(std::chrono::milliseconds time, bool word = false);
std::string TimedLyricText(const LyricLine& line, std::chrono::milliseconds offset = {});
}
