#pragma once

#include "ttplayer/lyrics/lrc_parser.h"
#include "ttplayer/lyrics/local_search.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::ui {

// CSettings stores the local lyric search order as a list of strings.  A
// leading '*' enables recursion, not the directory itself (00401C0F).
// This helper lists exact-name candidates; the asynchronous local search also
// enumerates scored matches and subdirectories (00444F71).
inline std::vector<std::filesystem::path> BuildLocalLyricCandidates(
    const std::filesystem::path& media_path,
    std::wstring_view artist,
    std::wstring_view title,
    const std::filesystem::path& runtime_directory,
    const std::filesystem::path& download_folder,
    const std::vector<std::wstring>& configured_folders) {
    const auto roots = lyrics::LocalSearchRoots(media_path, runtime_directory,
        download_folder, configured_folders);
    const std::wstring media_text = media_path.wstring();
    const bool network = media_text.find(L"://") != std::wstring::npos;

    std::vector<std::wstring> names;
    if (!artist.empty() && !title.empty())
        names.push_back(std::wstring(artist) + L" - " + std::wstring(title));
    if (!network && !media_path.stem().empty())
        names.push_back(media_path.stem().wstring());

    std::vector<std::filesystem::path> result;
    std::set<std::wstring> candidate_keys;
    for (const auto& root : roots) {
        for (const auto& name : names) {
            for (const auto* extension : {L".lrc", L".txt"}) {
                const auto candidate = root.path / (name + extension);
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
    for (auto& line : lyrics.lines) line.TrimSpaces();
}

} // namespace ttplayer::ui
