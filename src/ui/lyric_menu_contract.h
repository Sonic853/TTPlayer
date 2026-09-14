#pragma once
#include "ttplayer/lyrics/lrc_parser.h"
#include "ttplayer/core/text.h"
#include <array>
#include <map>
#include <windows.h>

namespace ttplayer::ui {
// 0053B9A0: entry zero overlaps the adjustment-dialog default (1000),
// and 0044D601 explicitly substitutes charset 0 for that entry.
inline constexpr std::array<BYTE, 15> kLyricCharsets{
    0, 134, 136, 130, 128, 178, 186, 238, 161, 129, 177, 204, 222, 162, 163};
// These names are literals in the 5.7.9 EXE's table, not resource strings.
inline constexpr std::array<const wchar_t*, 14> kLyricCharsetNames{
    L"Chinese(GB2312)", L"Chinese(BIG5)", L"Korean", L"Japanese(Shift-JIS)",
    L"Arabic", L"Baltic", L"East Europe", L"Greek", L"Hangul", L"Hebrew",
    L"Russian", L"Thai", L"Turkish", L"Vietnamese"};

inline bool AdjustLyricDocument(lyrics::Lyrics& document, UINT command,
                                std::chrono::milliseconds position) {
    if (document.lines.empty() || command < 0x8025 || command > 0x802a) return false;
    const auto current = document.LineAt(position);
    size_t begin = 0, end = document.lines.size();
    if (command <= 0x8026) {
        if (!current) return false; // 0043D771 accepts no current line (-1).
        begin = *current; end = begin + 1;
    } else if (command <= 0x8028) {
        begin = current ? *current + 1 : 0; // 0043D795: AFTER, not including current.
    }
    const auto delta = std::chrono::milliseconds(command & 1 ? -500 : 500);
    for (size_t i = begin; i < end; ++i) document.lines[i].time += delta;
    // Original signed addition does not clamp to zero or sort the lines.
    return begin < end;
}

inline bool ConvertLyricDocument(lyrics::Lyrics& document, DWORD mapping) {
    if (document.lines.empty()) return false;
    auto converted = document.lines; // Commit only a complete conversion.
    for (auto& line : converted) {
        const auto source = core::Utf8ToWide(line.text);
        if (source.empty()) continue;
        const LCID locale = GetThreadLocale(); // 004C1C5E
        const int count = LCMapStringW(locale, mapping,
            source.data(), static_cast<int>(source.size()), nullptr, 0);
        if (count <= 0) return false;
        std::wstring result(static_cast<size_t>(count), L'\0');
        if (!LCMapStringW(locale, mapping, source.data(),
                static_cast<int>(source.size()), result.data(), count)) return false;
        line.text = core::WideToUtf8(result);
    }
    document.lines = std::move(converted); // 0043D803: text only, not metadata/timestamps.
    return true;
}

// 0043DEC8's second argument means COMPACT, not "apply offset". This
// model stores offset separately; flatten its effective display timestamps
// so a subsequent read (without an offset tag) preserves synchronization.
inline std::wstring SerializeLyricDocument(const lyrics::Lyrics& document, bool compact) {
    if (document.lines.empty()) return {};
    std::wstring result;
    for (const auto& [name, value] : std::array<std::pair<const wchar_t*, const std::string*>, 4>{{
            {L"ti", &document.title}, {L"ar", &document.artist},
            {L"al", &document.album}, {L"by", &document.author}}})
        result += L"[" + std::wstring(name) + L":" + core::Utf8ToWide(*value) + L"]\r\n";
    result += L"\r\n";
    std::map<std::string, size_t> positions;
    std::vector<std::pair<std::wstring, std::wstring>> rows;
    for (const auto& line : document.lines) {
        // 0043DEC8 omits bracket-only display rows when serializing LRC.
        if (line.text.size() >= 2 && line.text.front() == '[' && line.text.back() == ']') continue;
        const auto ms = (line.time + document.offset).count();
        wchar_t stamp[80]{};
        swprintf_s(stamp, L"[%02lld:%02lld.%02lld]", ms / 60000, (ms % 60000) / 1000, (ms % 1000) / 10);
        if (compact) {
            const auto [it, inserted] = positions.emplace(line.text, rows.size());
            if (!inserted) { rows[it->second].first.insert(0, stamp); continue; }
        }
        rows.emplace_back(stamp, core::Utf8ToWide(line.text));
    }
    for (const auto& [stamps, text] : rows) result += stamps + text + L"\r\n";
    result += L"\r\n";
    return result;
}
}
