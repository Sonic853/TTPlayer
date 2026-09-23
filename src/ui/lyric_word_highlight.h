#pragma once
#include "ttplayer/lyrics/lrc_parser.h"
#include "ttplayer/core/text.h"
#include <algorithm>
#include <cmath>
#include <windows.h>

namespace ttplayer::ui {
// Measure against the actual selected font, not a character count. Row begin
// uses UTF-16 indices so wrapped lines and surrogate pairs share one timeline.
inline int WordHighlightPixels(HDC dc, const lyrics::LyricLine& line,
                               std::chrono::milliseconds position,
                               std::chrono::milliseconds end,
                               std::wstring_view row, size_t row_begin = 0) {
    const auto progress = line.Progress(position, end);
    const auto full_text = core::Utf8ToWide(line.text);
    const auto width = [&](std::wstring_view text) {
        SIZE size{};
        if (!text.empty()) GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
        return static_cast<int>(size.cx);
    };
    const auto measure = [&](size_t byte_offset) {
        const size_t characters = core::Utf8ToWide(
            std::string_view(line.text).substr(0, byte_offset)).size();
        return width(std::wstring_view(full_text).substr(0, characters));
    };
    const auto first = measure(progress.begin), last = measure(progress.end);
    // Interpolate BEFORE clipping to a display row. A timed word/phrase can
    // itself wrap; interpolating each row separately would light them together.
    const auto row_start = width(std::wstring_view(full_text).substr(0, row_begin));
    return std::clamp(static_cast<int>(std::lround(first + (last - first) * progress.fraction)) - row_start,
                      0, static_cast<int>(width(row)));
}
}
