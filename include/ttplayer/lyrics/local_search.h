#pragma once
#include "ttplayer/lyrics/lrc_parser.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ttplayer::lyrics {
struct LocalSearchRoot { std::filesystem::path path; bool recursive{}; };
std::vector<LocalSearchRoot> LocalSearchRoots(const std::filesystem::path& media,
    const std::filesystem::path& runtime, const std::filesystem::path& download,
    const std::vector<std::wstring>& configured);
struct LocalSearchRequest {
    std::filesystem::path media;
    std::wstring artist, title;
    std::vector<LocalSearchRoot> roots;
    bool partial{}, all_matches{};
    std::filesystem::path associated;
};
struct LocalSearchResult {
    std::vector<std::filesystem::path> matches;
    std::filesystem::path loaded_path;
    Lyrics lyric;
};
// 00444CCD (all matches), 00444F71/0043BC7C (best match). Workers never
// retain a window, player, plugin, or caller-owned reference.
struct LocalSearchJob {
    std::atomic_bool canceled{};
    std::mutex mutex;
    std::optional<LocalSearchResult> result;
};
std::shared_ptr<LocalSearchJob> SearchLocalLyricsAsync(LocalSearchRequest request);
LocalSearchResult SearchLocalLyrics(const LocalSearchRequest& request, const std::atomic_bool& canceled);
} // namespace ttplayer::lyrics
