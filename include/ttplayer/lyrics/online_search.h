#pragma once

#include "ttplayer/plugins/plugin_manager.h"
#include "ttplayer/settings/settings.h"
#include "ttplayer/lyrics/service_catalog.h"

namespace ttplayer::lyrics {

struct SearchResult { std::wstring artist, title; };
enum class SearchPhase { searching, results, downloading, downloaded, failed, canceled };
struct SearchSnapshot {
    SearchPhase phase{SearchPhase::searching};
    size_t revision{};
    size_t provider{};
    std::vector<SearchResult> results;
    std::wstring text, error, server, extra_title, extra_url;
};

// All private DLL calls, including its blocking destructor, run off the UI
// thread. Callbacks own copied state, never a PlayerWindow or HWND.
class OnlineSearch {
public:
    struct State;
    OnlineSearch(const plugins::PluginManager& plugins, size_t provider,
                 settings::NetworkSettings network, std::wstring artist,
                 std::wstring title);
    OnlineSearch(LyricService service, size_t index, settings::NetworkSettings network,
                 std::wstring artist, std::wstring title);
    ~OnlineSearch();
    OnlineSearch(const OnlineSearch&) = delete;
    OnlineSearch& operator=(const OnlineSearch&) = delete;
    SearchSnapshot Snapshot() const;
    bool Download(int index);
    void Cancel() noexcept;
private:
    std::shared_ptr<State> state_;
};

std::wstring LyricFileName(std::wstring value);
int ScoreLyricMatch(const SearchResult& result, std::wstring_view artist,
                   std::wstring_view title, std::wstring_view filename = {}, bool partial = false);
size_t BestSearchResult(const std::vector<SearchResult>& results,
                       std::wstring_view artist, std::wstring_view title);
std::filesystem::path DownloadDirectory(const settings::LyricSettings& settings,
    const std::filesystem::path& media, const std::filesystem::path& runtime);
// CLyric::Save (0043E3CF): lossless ACP, otherwise UTF-8 with BOM.
bool SaveDownloadedLyric(const std::filesystem::path& path,
                         std::wstring_view text, bool overwrite);

} // namespace ttplayer::lyrics
