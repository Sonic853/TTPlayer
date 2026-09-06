#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ttplayer::lyrics {
struct LyricLine { std::chrono::milliseconds time; std::string text; };
struct Lyrics {
    std::string title, artist, album, author;
    std::chrono::milliseconds offset{};
    std::vector<LyricLine> lines;
    [[nodiscard]] std::optional<size_t> LineAt(std::chrono::milliseconds position) const;
    void ShiftLines(std::chrono::milliseconds delta) noexcept;
};
Lyrics ParseLrc(std::string_view content);
Lyrics LoadLrc(const std::filesystem::path& path);
}
