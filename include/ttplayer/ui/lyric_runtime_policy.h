#pragma once

#include "ttplayer/lyrics/lrc_parser.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::ui {

// CSettings stores the local lyric search order as a list of strings.  A
// leading '*' is the persisted check state; the two angle-bracket values are
// logical locations rather than literal directory names.
inline std::vector<std::filesystem::path> BuildLocalLyricCandidates(
    const std::filesystem::path& media_path,
    std::wstring_view artist,
    std::wstring_view title,
    const std::filesystem::path& runtime_directory,
    const std::filesystem::path& download_folder,
    const std::vector<std::wstring>& configured_folders) {
    std::vector<std::filesystem::path> roots;
    std::set<std::wstring> root_keys;
    const std::wstring media_text = media_path.wstring();
    const bool network = media_text.find(L"://") != std::wstring::npos;

    const auto normalize = [](const std::filesystem::path& value,
                              const std::filesystem::path& base) {
        if (value.empty()) return std::filesystem::path{};
        return (value.is_absolute() ? value : base / value).lexically_normal();
    };
    const auto append_root = [&](const std::filesystem::path& value) {
        if (value.empty()) return;
        const auto normalized = value.lexically_normal();
        auto key = normalized.wstring();
        std::ranges::transform(key, key.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        if (root_keys.insert(std::move(key)).second)
            roots.push_back(normalized);
    };

    for (auto configured : configured_folders) {
        if (configured.empty() || configured.front() != L'*') continue;
        configured.erase(configured.begin());
        if (configured == L"<Sound Folder>") {
            if (!network) append_root(media_path.parent_path());
        } else if (configured == L"<Lyrics Download Folder>") {
            append_root(download_folder.empty()
                ? runtime_directory / L"Lyrics"
                : normalize(download_folder, runtime_directory));
        } else if (!configured.empty()) {
            append_root(normalize(configured, runtime_directory));
        }
    }

    std::vector<std::wstring> names;
    if (!network && !media_path.stem().empty())
        names.push_back(media_path.stem().wstring());
    if (!artist.empty() && !title.empty())
        names.push_back(std::wstring(artist) + L" - " + std::wstring(title));

    std::vector<std::filesystem::path> result;
    std::set<std::wstring> candidate_keys;
    for (const auto& root : roots) {
        for (const auto& name : names) {
            for (const auto* extension : {L".lrc", L".txt"}) {
                const auto candidate = root / (name + extension);
                auto key = candidate.lexically_normal().wstring();
                std::ranges::transform(key, key.begin(), [](wchar_t character) {
                    return static_cast<wchar_t>(std::towlower(character));
                });
                if (candidate_keys.insert(std::move(key)).second)
                    result.push_back(candidate);
            }
        }
    }
    return result;
}

inline void ApplyLyricTrimSpaces(lyrics::Lyrics& lyrics, bool trim_spaces) {
    if (!trim_spaces) return;
    for (auto& line : lyrics.lines) {
        const auto begin = line.text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            line.text.clear();
            continue;
        }
        const auto end = line.text.find_last_not_of(" \t\r\n");
        line.text = line.text.substr(begin, end - begin + 1);
    }
}

} // namespace ttplayer::ui
