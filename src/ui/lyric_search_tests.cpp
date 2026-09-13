// Real shipped x86 DLL against a loopback-only server. No historical endpoint
// is contacted: provider names from our same-name .ini are asserted FIRST.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ttplayer/ui/player_window.h"
#include "ttplayer/core/text.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void Until(F&& ready) {
    const auto end = GetTickCount64() + 10000;
    while (!ready()) {
        Require(GetTickCount64() < end, "asynchronous lyric operation timed out");
        MSG m{};
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        Sleep(10);
    }
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary); file << bytes;
    Require(file.good(), "cannot write isolated test fixture");
}
class LoopbackServer {
public:
    std::atomic_bool slow{};
    std::atomic_bool single{};
    std::atomic_uint searches{}, downloads{}, refreshes{};
    std::string base, xml;
    std::mutex mutex;
    std::vector<std::string> requests;
    LoopbackServer() {
        WSADATA data{}; Require(WSAStartup(MAKEWORD(2,2), &data) == 0, "WSAStartup");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "loopback bind");
        int size = sizeof(address); getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        Require(listen(listener_, 4) == 0, "loopback listen");
        base = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/lyrics";
        xml = "<ttp_lrcsvr><server name=\"Fixture one\" url=\"" + base +
            "\"/><server name=\"Fixture two\" url=\"" + base +
            "\"/><extra title=\"Fixture help\" url=\"" + base + "\"/></ttp_lrcsvr>";
        thread_ = std::thread([this] { Run(); });
    }
    ~LoopbackServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        closesocket(listener_); WSACleanup();
    }
private:
    void Run() {
        while (!stop_) {
            fd_set readable; FD_ZERO(&readable); FD_SET(listener_, &readable);
            timeval timeout{0,100000};
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            DWORD timeout_ms = 1000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout_ms), sizeof(timeout_ms));
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == request.npos && request.size() < 32000) {
                const int count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                request.append(buffer, count);
            }
            { std::lock_guard lock(mutex); requests.push_back(request); }
            std::string body;
            if (request.find("?svrlst") != request.npos) { ++refreshes; body = xml; }
            else if (request.find("?sh?") != request.npos) {
                ++searches;
                if (slow) for (int i = 0; i < 150 && !stop_; ++i) Sleep(10);
                if (request.find("6500720072006F007200") != request.npos) body = "<result errmsg=\"Fixture error\" errcode=\"32010\"/>";
                else if (single) body = "<result><lrc id=\"101\" artist=\"陈慧娴\" title=\"千千阙歌\"/></result>";
                else body = "<result><lrc id=\"100\" artist=\"Someone\" title=\"Other\"/>"
                            "<lrc id=\"101\" artist=\"陈慧娴\" title=\"千千阙歌\"/></result>";
            } else if (request.find("?dl?") != request.npos) {
                ++downloads; body = "[ar:陈慧娴]\r\n[ti:千千阙歌]\r\n[00:00.00]fixture lyric\r\n[00:01.00]歌词测试\r\n";
            }
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            for (size_t sent = 0; sent < response.size();) {
                const int count = send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0);
                if (count <= 0) break;
                sent += count;
            }
            closesocket(client);
        }
    }
    SOCKET listener_{INVALID_SOCKET};
    std::thread thread_;
    std::atomic_bool stop_{};
};
}

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void Ui(HMODULE resources, plugins::PluginManager& library, const fs::path& directory,
                   LoopbackServer& server) {
        settings::Settings settings;
        settings.source_path = directory / L"test-only.xml";
        settings.general.tray_icon = false; settings.general.send_title_to_msn = false;
        settings.general.fade_windows = false; settings.lyric.auto_download = false;
        settings.hotkey.global = false; settings.network.proxy_type = 0;
        settings.lyric.download_folder = directory / L"downloads";
        ui::PlayerWindow player(settings);
        player.instance_ = GetModuleHandleW(nullptr);
        player.SetSkinResourceModule(resources);
        player.SetSoundLibrary(&library);
        player.ShowOptions(8);
        const HWND combo = GetDlgItem(player.options_pages_[8], 2090);
        Require(SendMessageW(combo, CB_GETCOUNT, 0, 0) == 2, "options not using DLL registry");
        wchar_t name[256]{}; SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(name));
        Require(std::wstring(name) == L"Fixture one", "options retained hard-coded servers");
        player.CloseOptions();
        player.ShowOnlineLyricSearch();
        HWND dialog = player.lyric_search_dialog_;
        Require(dialog && GetDlgItem(dialog, 1046), "original dialog 209 missing");
        SetWindowPos(dialog, nullptr, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetDlgItemTextW(dialog, 1021, L"陈慧娴"); SetDlgItemTextW(dialog, 1009, L"千千阙歌");
        SendMessageW(dialog, WM_COMMAND, 1046, 0);
        Until([&] { player.PollOnlineLyricSearch(); return player.lyric_search_results_shown_; });
        HWND list = GetDlgItem(dialog, 1064);
        Require(ListView_GetItemCount(list) == 2, "results not populated");
        Require(ListView_GetNextItem(list, -1, LVNI_SELECTED) == 1, "best match selection");
        wchar_t heading[256]{}; LVCOLUMNW column{}; column.mask = LVCF_TEXT;
        column.pszText = heading; column.cchTextMax = 256;
        Require(ListView_GetColumn(list, 0, &column) && *heading, "resource column caption missing");
        SetDlgItemTextW(dialog, 2001, L"fixture-ui.lrc");
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        const auto saved = directory / L"downloads/fixture-ui.lrc";
        Until([&] { player.PollOnlineLyricSearch(); return fs::exists(saved); });
        Require(!player.lyrics_.lines.empty() && player.lyric_path_ == saved, "download not applied to lyrics");
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SetDlgItemTextW(dialog, 2001, L"fixture-second.lrc");
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        Until([&] { player.PollOnlineLyricSearch(); return fs::exists(directory / L"downloads/fixture-second.lrc"); });
        Require(IsWindowEnabled(GetDlgItem(dialog, IDOK)), "cannot download another candidate after success");
        SendMessageW(dialog, WM_CLOSE, 0, 0);
        Require(!IsWindow(dialog) && !player.lyric_search_, "close leaked UI/session");

        // Auto-search policy, silent best match, and stale-track cancellation.
        player.settings_.lyric.auto_download = true;
        player.settings_.lyric.auto_select_download = true;
        playlist::Track track; track.path = directory / L"media.flac";
        track.title = "千千阙歌"; track.artist = "陈慧娴";
        player.opened_track_ = track;
        player.ClearLyrics();
        player.StartOnlineLyricSearch(true);
        Until([&] { player.PollOnlineLyricSearch(); return !player.lyric_path_.empty(); });
        Require(!player.lyric_search_dialog_, "auto-select unexpectedly displayed modal results");
        const auto searches = server.searches.load();
        player.StartOnlineLyricSearch(true);
        Require(server.searches == searches && !player.lyric_search_, "automatic retry storm");

        player.settings_.lyric.auto_select_download = false;
        player.ClearLyrics(); player.lyric_auto_search_key_.clear();
        player.StartOnlineLyricSearch(true);
        Until([&] { player.PollOnlineLyricSearch(); return player.lyric_search_dialog_ != nullptr; });
        dialog = player.lyric_search_dialog_;
        Require(GetDlgItem(dialog, 2067) && !GetDlgItem(dialog, 1046), "automatic dialog 208 missing");
        wchar_t countdown[256]{}; GetDlgItemTextW(dialog, 1052, countdown, 256);
        Require(std::wstring(countdown).find(L"15") != std::wstring::npos, "countdown did not reuse original resource text");
        SendMessageW(dialog, WM_LBUTTONDOWN, 0, 0);
        Require(player.lyric_download_deadline_ == 0, "user input failed to cancel countdown");
        player.opened_track_->path = directory / L"next.flac";
        player.PollOnlineLyricSearch();
        Require(!player.lyric_search_ && !IsWindow(dialog), "old-track response/dialog survived song change");

        // Single result follows 0044BEED's automatic branch even with the
        // AutoSelectDownload checkbox off. Existing downloads are not replaced.
        server.single = true;
        player.ClearLyrics(); player.lyric_auto_search_key_.clear();
        player.StartOnlineLyricSearch(true);
        Until([&] { player.PollOnlineLyricSearch(); return !player.lyric_path_.empty(); });
        Require(!player.lyric_search_dialog_ && !player.lyric_search_, "single-result choice was not automatic");
        server.single = false;
        player.ClearLyrics(); player.lyric_auto_search_key_.clear();
        player.settings_.lyric.download_when_full_info = true;
        player.opened_track_->artist.clear();
        player.StartOnlineLyricSearch(true);
        Require(!player.lyric_search_, "full-info gate ignored missing artist");

        // No DLLs -> no pretend server names and no enabled search button.
        player.SetSoundLibrary(nullptr); player.ShowOptions(8);
        Require(SendDlgItemMessageW(player.options_pages_[8], 2090, CB_GETCOUNT, 0, 0) == 0, "empty registry fabricated servers");
        player.CloseOptions(); player.ShowOnlineLyricSearch();
        Require(!IsWindowEnabled(GetDlgItem(player.lyric_search_dialog_, 1046)), "search enabled without plugins");
        player.CloseOnlineLyricSearch();
        Require(!fs::exists(settings.source_path), "test saved user settings");
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2, "expected repository root");
        Require(SUCCEEDED(OleInitialize(nullptr)), "OleInitialize");
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        LoopbackServer server;
        const auto directory = fs::temp_directory_path() /
            (L"TTPlayer-lyric-search-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(directory / L"AddIn");
        fs::copy_file(fs::path(argv[1]) / L"AddIn/ttp_lrcsh.dll", directory / L"AddIn/ttp_lrcsh.dll");
        Write(directory / L"AddIn/ttp_lrcsh.ini", server.xml);
        plugins::PluginManager library;
        Require(SUCCEEDED(library.Load(directory / L"AddIn")), "load fixture DLL");
        const auto& providers = library.LyricSearchProviders();
        Require(providers.size() == 2 && providers[0].name == L"Fixture one" &&
            providers[1].name == L"Fixture two", "STOP: DLL did not accept loopback-only external .ini");
        settings::NetworkSettings network; network.proxy_type = 0;
        {
            lyrics::OnlineSearch search(library, 1, network, L"陈慧娴", L"千千阙歌");
            Until([&] { const auto s = search.Snapshot();
                if (s.phase == lyrics::SearchPhase::failed) std::wcerr << L"DLL error: " << s.error << L'\n';
                Require(s.phase != lyrics::SearchPhase::failed, "search callback failed");
                return s.phase == lyrics::SearchPhase::results; });
            auto s = search.Snapshot();
            Require(s.provider == 1 && s.results.size() == 2 && s.results[1].artist == L"陈慧娴", "callback ABI data corruption");
            Require(!search.Download(2) && !search.Download(-1), "invalid result index accepted");
            Require(search.Download(1), "cannot submit download");
            Until([&] { return search.Snapshot().phase == lyrics::SearchPhase::downloaded; });
            s = search.Snapshot();
            Require(s.text.find(L"歌词测试") != s.text.npos, "UTF-8 download conversion");
            const auto path = directory / L"downloaded.lrc";
            Require(lyrics::SaveDownloadedLyric(path, s.text, false), "download save failed");
            const auto before = Read(path);
            Require(!lyrics::SaveDownloadedLyric(path, L"overwrite", false) && Read(path) == before, "overwrite protection");
            Require(lyrics::LoadLrc(path).lines.size() == 2, "saved lyric cannot be loaded");
        }
        Require(server.refreshes == 1 && server.searches == 1 && server.downloads == 1, "native server refresh/search/download chain");
        const auto cache = Read(directory / L"AddIn/ttp_lrcsh.ini");
        Require(cache.find("Fixture one") != cache.npos && cache.find("<extra") == cache.npos, "DLL did not persist sanitized server cache");
        {
            std::lock_guard lock(server.mutex);
            Require(server.requests[1].find("Artist=") != std::string::npos &&
                server.requests[1].find("Title=") != std::string::npos &&
                server.requests[2].find("Id=101&Code=") != std::string::npos, "native protocol/index mapping");
        }
        const auto resources = LoadLibraryExW((fs::path(argv[1]) / L"ttpres.dll").c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Require(resources != nullptr, "5.7.9 resources");
        testing::SkinRebindAccess::Ui(resources, library, directory, server);
        FreeLibrary(resources);
        {
            lyrics::OnlineSearch search(library, 99, network, L"", L"error");
            Until([&] { return search.Snapshot().phase == lyrics::SearchPhase::failed; });
            Require(search.Snapshot().provider == 0, "invalid provider did not fall back to zero");
        }
        server.slow = true;
        const auto before = server.searches.load();
        auto slow = std::make_unique<lyrics::OnlineSearch>(library, 0, network, L"", L"slow");
        Until([&] { return server.searches > before; });
        const auto start = GetTickCount64();
        slow.reset();
        Require(GetTickCount64() - start < 100, "cancel waited for network/DLL release");
        library.Shutdown(); // worker must retain its own DLL and creator references
        // Let the private DLL's destructor finish off-thread before test teardown.
        Sleep(1800);
        Require(server.refreshes == 1, "server list refreshed repeatedly in same module lifetime");
        Require(lyrics::LyricFileName(L"../CON:x") == L".._CON_x.lrc", "unsafe filename handling");
        Require(lyrics::LyricFileName(L"CON") == L"_CON.lrc", "DOS device handling");
        Require(lyrics::BestSearchResult({{L"陳慧嫻",L"千千闕歌"}, {L"",L"other"}},
            L"陈慧娴",L"千千阙歌") == 0, "simplified/traditional matching");
        Require(lyrics::BestSearchResult({{L"A",L"小歌谣"},{L"A",L"歌 (现场)"}},
            L"A",L"歌") == 1, "matching ignored substring word boundaries");
        Require(lyrics::BestSearchResult({{L"A",L"Other"},{L"A",L"01. Song【Live】"}},
            L"A",L"Song") == 1, "bracket/track-number normalization");
        OleUninitialize();
        std::wcout << L"PASS: actual DLL .ini enumeration/cache, search/download ABI, UI 208/209, settings registry,\n"
                      L"auto selection, stale-track cancel, error/fallback, nonblocking close, safe save.\nFixtures: " << directory << L'\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "lyric search test: " << error.what() << '\n'; return 1;
    }
}
