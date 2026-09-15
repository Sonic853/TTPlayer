// Test the actual private completion/queue code as well as the real child.
#include "player_window_playlist_info.cpp"
#include "ttplayer/app/file_info_worker.h"
#include "ttplayer/app/worker_process.h"
#include "playlist_info_session_protocol.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
using namespace ttplayer::ui::detail;
namespace {
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void Wave(const fs::path& path, unsigned seconds = 1) {
    std::ofstream file(path, std::ios::binary);
    auto put = [&](auto value) { file.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
    const uint32_t bytes = seconds * 88200;
    file.write("RIFF", 4); put(uint32_t{36 + bytes}); file.write("WAVEfmt ", 8);
    put(uint32_t{16}); put(uint16_t{1}); put(uint16_t{1}); put(uint32_t{44100});
    put(uint32_t{88200}); put(uint16_t{2}); put(uint16_t{16});
    file.write("data", 4); put(bytes);
    const std::string silence(bytes, '\0'); file.write(silence.data(), silence.size());
}
void CheckSession(const fs::path& root) {
    const auto addin = root / L"AddIn", song = root / L"song.wav";
    fs::create_directories(addin); Wave(song);
    PlaylistInfoProbeSession session;
    FileInfoProbeProcessState state{};
    const auto read = [&](const fs::path& path, DWORD timeout = 5000,
                          const FileInfoProbeMp3Policy& policy = {}) {
        return session.Read({}, {}, addin, path, {}, 0, timeout, &state, policy);
    };
    auto result = read(song);
    Require(result && SUCCEEDED(result->status) && result->duration_ms == 1000, "WAV session read");
    const auto one = session.Stats();
    for (int i = 0; i < 5000; ++i) Require(read(song).has_value(), "cache repeat");
    Require(session.Stats().reads == one.reads && session.Stats().cache_hits == 5000, "cache reopened decoder");
    Wave(song, 2); result = read(song);
    Require(result && result->duration_ms == 2000 && session.Stats().reads == one.reads + 1, "file size cache invalidation");
    HANDLE file = CreateFileW(song.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "open stamp");
    FILETIME time{}; GetSystemTimeAsFileTime(&time); time.dwHighDateTime += 1;
    Require(SetFileTime(file, nullptr, nullptr, &time), "set stamp"); CloseHandle(file);
    Require(read(song).has_value() && session.Stats().reads == one.reads + 2, "same size modified stamp");
    FileInfoProbeMp3Policy policy; policy.read_priority ^= 0x40000;
    Require(read(song, 5000, policy).has_value() && session.Stats().reads == one.reads + 3, "policy cache key");
    Require(session.Stats().launches == 1, "per-song process launch regression");

    const auto next = root / L"next.wav"; Wave(next);
    for (const auto fault : {L"once-crash", L"once-stall", L"once-malformed", L"once-stale"}) {
        // Change configuration to retire the healthy child before injecting a
        // startup fault in the new one. The following read uses the same config.
        const auto fault_addin = root / fault; fs::create_directories(fault_addin);
        std::ofstream(fault_addin / fault) << "fault";
        auto bad = session.Read({}, {}, fault_addin, next, {}, 0, 1000, &state);
        Require(!bad, "fault must fail");
        Require(state == (std::wstring_view(fault) == L"once-stall" ?
            FileInfoProbeProcessState::timed_out : FileInfoProbeProcessState::process_failed), "fault classification");
        const auto launches = session.Stats().launches;
        result = session.Read({}, {}, fault_addin, next, {}, 0, 5000, &state);
        Require(result && SUCCEEDED(result->status) && session.Stats().launches == launches + 1, "failed worker not restarted");
    }
    const auto stall = root / L"cancel"; fs::create_directories(stall);
    std::ofstream(stall / L"once-stall") << "fault";
    std::stop_source cancel;
    std::jthread timer([&] { Sleep(100); cancel.request_stop(); });
    const auto start = GetTickCount64();
    Require(!session.Read(cancel.get_token(), {}, stall, next, {}, 0, 15000, &state) &&
        state == FileInfoProbeProcessState::cancelled && GetTickCount64() - start < 2500, "bounded cancellation");
    timer.join();
    result = session.Read({}, {}, stall, next, {}, 0, 5000, &state);
    Require(result && SUCCEEDED(result->status), "restart after cancellation");

    const auto cue = root / L"album.cue";
    std::ofstream(cue) << "TITLE \"Album\"\nFILE \"song.wav\" WAVE\n TRACK 01 AUDIO\n  TITLE \"First\"\n  INDEX 01 00:00:00\n TRACK 02 AUDIO\n  TITLE \"Second\"\n  INDEX 01 00:01:00\n";
    const auto first = session.Read({}, {}, addin, cue, {}, 1, 5000);
    const auto second = session.Read({}, {}, addin, cue, {}, 2, 5000);
    Require(first && second && SUCCEEDED(first->status) && SUCCEEDED(second->status) &&
        first->duration_ms == 1000 && second->duration_ms == 1000, "CUE subtrack duration");

    // A reproducible local comparison, without making timing a pass condition.
    std::vector<fs::path> files;
    for (int i = 0; i < 24; ++i) { files.push_back(root / (L"bench" + std::to_wstring(i) + L".wav")); Wave(files.back()); }
    auto begin = GetTickCount64();
    for (const auto& path : files) Require(RunPlaylistInfoReadProbe({}, {}, addin, path, {}, 0).has_value(), "one-shot baseline");
    const auto baseline = GetTickCount64() - begin;
    PlaylistInfoProbeSession fast; begin = GetTickCount64();
    for (const auto& path : files) Require(fast.Read({}, {}, addin, path, {}, 0).has_value(), "session benchmark");
    std::cout << "24 WAV files: one-shot=" << baseline << "ms, session=" << GetTickCount64() - begin
              << "ms; launches=24->" << fast.Stats().launches << "; 5000 cache hits avoid decoding\n";
}
}

namespace ttplayer::testing {
struct ProgressSeekAccess {
    static void Coordinator(const fs::path& root) {
        settings::Settings settings;
        settings.general.tray_icon = settings.general.send_title_to_msn = false;
        settings.lyric.auto_download = settings.lyric.auto_load_lyric = false;
        settings.playlist.read_info_mode = 1;
        plugins::PluginManager manager;
        ui::PlayerWindow p(settings);
        p.sound_library_ = &manager;
        p.window_ = CreateWindowExW(0, L"STATIC", L"info fixture", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, nullptr, nullptr);
        Require(p.window_ != nullptr, "coordinator receiver");
        p.playlists_.NewList(L"coordinator");
        for (const auto name : {L"next.wav", L"song.wav", L"next.wav"}) {
            playlist::Track track; track.path = root / name; p.ActivePlaylist().Add(std::move(track));
        }
        p.QueuePlaylistInfoRange(p.playlists_.ActiveIndex(), 0, 3);
        p.ActivePlaylist().SortByFileName();
        const auto deadline = GetTickCount64() + 10000;
        while (p.playlist_info_working_ || !p.playlist_info_pending_.empty()) {
            MSG message{};
            while (PeekMessageW(&message, p.window_, kMsgPlaylistInfoReady, kMsgPlaylistInfoReady, PM_REMOVE))
                if (message.message == kMsgPlaylistInfoReady) p.ApplyPlaylistInfoResult(message.lParam);
            Require(GetTickCount64() < deadline, "coordinator stalled");
            Sleep(5);
        }
        for (const auto& track : p.ActivePlaylist().Tracks()) Require(track.duration_ms > 0, "coordinator result missing");
        p.ShutdownPlaylistInfoLoading();
        Require(!p.playlist_info_receiver_ && !p.playlist_info_active_, "coordinator shutdown");
        p.sound_library_ = nullptr;
        DestroyWindow(p.window_); p.window_ = nullptr;
    }
    static void Run() {
        settings::Settings settings;
        settings.general.tray_icon = settings.general.send_title_to_msn = false;
        settings.lyric.auto_download = settings.lyric.auto_load_lyric = false;
        settings.playlist.read_info_mode = 0;
        ui::PlayerWindow p(settings);
        p.playlists_.NewList(L"fixture");
        auto& list = p.ActivePlaylist();
        for (int i = 0; i < 5000; ++i) {
            playlist::Track track; track.path = L"song" + std::to_wstring(i) + L".wav";
            list.Add(std::move(track));
        }
        const auto duplicate = list.Tracks()[100]; list.Add(duplicate);
        const auto index = p.playlists_.ActiveIndex();
        const auto key = ui::SourceKey(duplicate.path, 0);
        const auto& rows = p.PlaylistInfoSourceRows(index, key);
        Require(rows == std::vector<size_t>{100, 5000}, "duplicate source index");
        const auto revision = list.OrderRevision();
        auto result = std::make_unique<ui::PlaylistInfoResult>();
        result->playlist_slot = p.playlists_.Entries()[index].slot;
        result->path = duplicate.path; result->info_revision = list.InfoRevision();
        result->state = ui::PlaylistInfoState::success; result->duration_ms = 1234;
        list.SortByFileName();
        p.ApplyPlaylistInfoResult(reinterpret_cast<LPARAM>(result.release()));
        const auto sorted = p.PlaylistInfoSourceRows(index, key);
        Require(list.OrderRevision() != revision && sorted.size() == 2, "sort index rebuild");
        for (const auto row : sorted) Require(list.Tracks()[row].duration_ms == 1234, "completion after sort");
        const auto indexed_revision = p.playlist_info_index_->revision;
        list.SetRating(0, 2);
        p.PlaylistInfoSourceRows(index, key);
        Require(p.playlist_info_index_->revision == indexed_revision, "metadata rebuilt row index");

        auto pending = std::make_unique<ui::PlaylistInfoResult>();
        pending->playlist_slot = p.playlists_.Entries()[index].slot;
        pending->path = list.Tracks()[1].path; pending->info_revision = list.InfoRevision();
        pending->state = ui::PlaylistInfoState::success; pending->title = "stale";
        list.SetMetadata(1, "edited", {}, {});
        p.ApplyPlaylistInfoResult(reinterpret_cast<LPARAM>(pending.release()));
        Require(list.Tracks()[1].title == "edited" && list.Tracks()[1].duration_ms == -2, "stale result overwrote edit");
        auto removed = std::make_unique<ui::PlaylistInfoResult>();
        removed->playlist_slot = p.playlists_.Entries()[index].slot;
        removed->path = list.Tracks()[2].path; removed->info_revision = list.InfoRevision();
        removed->state = ui::PlaylistInfoState::success; removed->title = "removed";
        list.Remove(2); p.ApplyPlaylistInfoResult(reinterpret_cast<LPARAM>(removed.release()));
        Require(list.Tracks()[2].title != "removed", "removed row wrote into successor");

        plugins::PluginManager manager;
        p.sound_library_ = &manager; p.playlist_info_working_ = true;
        p.playlist_info_active_ = ui::PlayerWindow::PlaylistInfoRequest{
            p.playlists_.Entries()[index].slot, 10, list.Tracks()[10].path, 0, true};
        p.RequestPlaylistTrackInfo(index, 10, false, false);
        Require(!p.playlist_info_active_->visible_only, "active promotion lost bulk intent");
        p.playlist_info_active_.reset();
        p.RequestPlaylistTrackInfo(index, 10, false, false);
        p.RequestPlaylistTrackInfo(index, 11, false, false);
        p.RequestPlaylistTrackInfo(index, 11, true, true);
        Require(p.playlist_info_pending_.size() == 2 && p.playlist_info_pending_.front().row_hint == 11 &&
            !p.playlist_info_pending_.front().visible_only, "bulk promotion lost intent/deduplication");
        p.ShutdownPlaylistInfoLoading();
        p.playlist_info_working_ = true;
        p.RequestPlaylistTrackInfo(index, 10, true, true);
        p.playlist_info_working_ = false; p.StartNextPlaylistInfoRead();
        Require(p.playlist_info_pending_.empty() && !p.playlist_info_working_, "offscreen work not discarded");
        p.playlist_info_working_ = true; p.RequestPlaylistTrackInfo(index, 10);
        p.settings_.playlist.read_info_mode = 2;
        p.ShutdownPlaylistInfoLoading(); p.PollPlaylistInfo();
        Require(!p.playlist_info_working_ && p.playlist_info_pending_.empty(), "mode two cancellation");
        p.sound_library_ = nullptr;

        p.window_ = CreateWindowExW(0, L"STATIC", L"fallback fixture", 0, 0, 0, 200, 100,
            HWND_MESSAGE, nullptr, nullptr, nullptr);
        p.playlist_view_ = CreateWindowExW(0, L"LISTBOX", nullptr, WS_CHILD | LBS_NOINTEGRALHEIGHT,
            0, 0, 200, 80, p.window_, nullptr, nullptr, nullptr);
        Require(p.playlist_view_ != nullptr, "fallback list creation");
        for (size_t row = 0; row < 20; ++row)
            SendMessageW(p.playlist_view_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"old"));
        SendMessageW(p.playlist_view_, LB_SETCURSEL, 12, 0);
        SendMessageW(p.playlist_view_, LB_SETTOPINDEX, 8, 0);
        const auto top = SendMessageW(p.playlist_view_, LB_GETTOPINDEX, 0, 0);
        list.SetMetadata(10, "Updated", "Artist", {});
        p.InvalidatePlaylistInfoRows({10});
        wchar_t text[64]{}; SendMessageW(p.playlist_view_, LB_GETTEXT, 10, reinterpret_cast<LPARAM>(text));
        Require(std::wstring(text) == DisplayName(list.Tracks()[10]) &&
            SendMessageW(p.playlist_view_, LB_GETCURSEL, 0, 0) == 12 &&
            SendMessageW(p.playlist_view_, LB_GETTOPINDEX, 0, 0) == top, "fallback text/selection/scroll update");
        DestroyWindow(p.window_); p.window_ = nullptr; p.playlist_view_ = nullptr;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring_view(argv[1]) == app::kFileInfoWorkerSwitch) {
        if (argc == 9 && std::wstring_view(argv[2]) == L"playlist-session") {
            const fs::path addin(argv[3]);
            for (const auto name : {L"once-crash", L"once-stall", L"once-malformed", L"once-stale"}) {
                if (!fs::exists(addin / name)) continue;
                fs::remove(addin / name);
                if (std::wstring_view(name) == L"once-crash") return 17;
                if (std::wstring_view(name) == L"once-stall") Sleep(30000);
                else {
                    HANDLE incoming = OpenEventW(SYNCHRONIZE, FALSE, argv[7]);
                    HANDLE outgoing = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[8]);
                    if (!incoming || !outgoing || WaitForSingleObject(incoming, 5000) != WAIT_OBJECT_0) return 18;
                    if (std::wstring_view(name) == L"once-stale") {
                        PlaylistInfoSessionRequest request;
                        if (!ReadPlaylistInfoSessionRequest(argv[5], request)) return 19;
                        FileInfoProbeReadResult result; result.status = S_OK;
                        WritePlaylistInfoSessionResult(argv[6], request.id + 1, result);
                    } else { std::ofstream output(fs::path(argv[6]), std::ios::binary); output << "broken"; }
                    SetEvent(outgoing); Sleep(30000);
                }
            }
        }
        return app::RunFileInfoWorker(argc - 1, argv + 1);
    }
    try {
        const auto root = fs::temp_directory_path() / (L"TTPlayerInfo-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(root);
        EnableEmbeddedFileInfoProbe();
        CheckSession(root); testing::ProgressSeekAccess::Run(); testing::ProgressSeekAccess::Coordinator(root);
        std::cout << "Session/cache/fault/cancel/CUE and playlist queue/index/edit-race tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
