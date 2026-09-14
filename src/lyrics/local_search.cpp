#include "ttplayer/lyrics/local_search.h"
#include "ttplayer/lyrics/association.h"
#include "ttplayer/lyrics/online_search.h"
#include <algorithm>
#include <functional>
#include <set>
#include <thread>
#include <windows.h>

namespace ttplayer::lyrics {
namespace {
struct PathLess {
    bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
bool IsFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}
SearchResult SplitName(const std::wstring& name) {
    const auto dash = name.find(L'-'); // 004444E3: first hyphen, not only " - ".
    return dash == name.npos ? SearchResult{L"", name} : SearchResult{name.substr(0, dash), name.substr(dash + 1)};
}
}

std::vector<LocalSearchRoot> LocalSearchRoots(const std::filesystem::path& media,
    const std::filesystem::path& runtime, const std::filesystem::path& download,
    const std::vector<std::wstring>& configured) {
    std::vector<LocalSearchRoot> result;
    for (auto value : configured) {
        if (value.empty()) continue;
        const bool recursive = value.front() == L'*';
        if (recursive) value.erase(value.begin());
        std::filesystem::path path;
        if (value == L"<Sound Folder>") {
            if (media.native().find(L"://") == std::wstring::npos) path = media.parent_path();
        } else if (value == L"<Lyrics Download Folder>") path = download;
        else path = value;
        if (!path.empty()) result.push_back({(path.is_absolute() ? path : runtime / path).lexically_normal(), recursive});
    }
    return result;
}

LocalSearchResult SearchLocalLyrics(const LocalSearchRequest& request, const std::atomic_bool& canceled) {
    LocalSearchResult result;
    auto load = [&](const std::filesystem::path& path) {
        if (canceled || path.empty()) return false;
        try {
            auto lyric = LoadLrc(path);
            if (lyric.lines.empty()) return false;
            result.loaded_path = path; result.lyric = std::move(lyric); return true;
        } catch (...) { return false; }
    };
    if (!request.all_matches && load(request.associated)) return result;
    if (!request.all_matches && request.associated == kNoLyric) return result;
    std::set<std::filesystem::path, PathLess> seen;
    int best{};
    std::filesystem::path best_path;
    const auto file_title = request.media.stem().wstring();
    const auto exact_name = request.artist.empty() ? file_title : request.artist + L" - " + request.title;
    const auto accept = [&](const std::filesystem::path& path, int score) {
        if (score < (request.partial ? 1 : 4)) return;
        if (request.all_matches) {
            if (seen.insert(path.lexically_normal()).second) result.matches.push_back(path);
        } else if (score > best) { best = score; best_path = path; }
    };
    std::function<void(const std::filesystem::path&, unsigned, bool)> scan;
    scan = [&](const std::filesystem::path& root, unsigned depth, bool recursive) {
        if (canceled || (!request.all_matches && best == 9)) return;
        if (!request.all_matches) {
            for (const auto& [name, score] : {std::pair{exact_name, 9}, std::pair{file_title, 4}}) {
                // Filename metadata must never escape the configured root.
                if (name.empty() || std::filesystem::path(name).filename() != name || name.find_first_of(L"<>:\"/\\|?*") != name.npos) continue;
                for (const auto* extension : {L".lrc", L".txt"}) {
                    const auto file = root / (name + extension);
                    if (IsFile(file)) { accept(file, score); break; }
                }
                if (best == 9) return;
            }
        }
        std::vector<std::filesystem::path> subdirectories;
        std::error_code error;
        for (std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && it != end && !canceled; it.increment(error)) {
            const auto& entry = *it;
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) continue;
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (recursive && depth < 4 && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) subdirectories.push_back(entry.path());
                continue;
            }
            const auto extension = entry.path().extension().wstring();
            if (_wcsicmp(extension.c_str(), L".lrc") && _wcsicmp(extension.c_str(), L".txt")) continue;
            const auto stem = entry.path().stem().wstring();
            accept(entry.path(), ScoreLyricMatch(SplitName(stem), request.artist, request.title, file_title, request.partial));
            if (!request.all_matches && best == 9) return;
        }
        for (const auto& path : subdirectories) scan(path, depth + 1, recursive);
    };
    for (const auto& root : request.roots) scan(root.path, 1, root.recursive);
    if (!request.all_matches && best >= 4) load(best_path);
    return result;
}

std::shared_ptr<LocalSearchJob> SearchLocalLyricsAsync(LocalSearchRequest request) {
    auto job = std::make_shared<LocalSearchJob>();
    std::thread([job, request = std::move(request)] {
        LocalSearchResult result;
        try { result = SearchLocalLyrics(request, job->canceled); } catch (...) {}
        std::lock_guard lock(job->mutex);
        if (!job->canceled) job->result = std::move(result);
    }).detach();
    return job;
}
} // namespace ttplayer::lyrics
