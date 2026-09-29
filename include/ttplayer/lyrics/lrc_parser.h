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
struct LyricTimeRange { std::chrono::milliseconds start, end; };
struct LyricLine {
    std::chrono::milliseconds time;
    std::string text;
    std::vector<WordTime> words;
    // Explicit singing end; LineAt still retains the text until the next line.
    std::optional<std::chrono::milliseconds> end_time;
    void Shift(std::chrono::milliseconds delta) noexcept;
    void TrimSpaces();
    [[nodiscard]] std::chrono::milliseconds EndOr(std::chrono::milliseconds fallback) const noexcept;
    [[nodiscard]] WordProgress Progress(std::chrono::milliseconds position,
                                       std::chrono::milliseconds end) const;
};
struct Lyrics {
    std::string title, artist, album, author;
    std::chrono::milliseconds offset{};
    std::vector<LyricLine> lines;
    [[nodiscard]] std::optional<size_t> LineAt(std::chrono::milliseconds position) const;
    [[nodiscard]] std::chrono::milliseconds LineEnd(size_t index) const;
    void ShiftLines(std::chrono::milliseconds delta) noexcept;
};
Lyrics ParseLrc(std::string_view content);
Lyrics LoadLrc(const std::filesystem::path& path);
std::optional<std::chrono::milliseconds> ParseLrcTimestamp(std::string_view value,
                                                        bool centisecond_rollover = false);
// Contents of one [start,end] tag. Colon fractions require three digits;
// ordinary timestamps and repeated [t1][t2] tags keep their existing syntax.
std::optional<LyricTimeRange> ParseLrcTimeRange(std::string_view value);
bool HasLrcTimeRange(std::string_view line);
bool HasCentisecondRollover(std::string_view content);
std::string FormatLrcTimestamp(std::chrono::milliseconds time, bool word = false);
std::string FormatLrcTimeRange(LyricTimeRange range);
std::string TimedLyricText(const LyricLine& line, std::chrono::milliseconds offset = {});
}
