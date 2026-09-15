// Exercise the real private coordinator and its controlled completion order.
#include "player_window_library.cpp"
#include "ttplayer/app/file_info_worker.h"
#include "ttplayer/app/worker_process.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void Wave(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path,std::ios::binary);
    auto put=[&](auto value) { file.write(reinterpret_cast<const char*>(&value),sizeof(value)); };
    file.write("RIFF",4); put(uint32_t{88236}); file.write("WAVEfmt ",8);
    put(uint32_t{16}); put(uint16_t{1}); put(uint16_t{1}); put(uint32_t{44100});
    put(uint32_t{88200}); put(uint16_t{2}); put(uint16_t{16});
    file.write("data",4); put(uint32_t{88200});
    std::string silence(88200,'\0'); file.write(silence.data(),silence.size());
}
settings::Settings QuietSettings() {
    settings::Settings s;
    s.general.fade_windows=s.general.tray_icon=s.general.send_title_to_msn=false;
    s.lyric.auto_download=s.lyric.auto_load_lyric=false;
    s.playback.auto_play=false; s.player.mute=true;
    s.library.enabled=true; s.playlist.library_mode=true;
    return s;
}
struct Monitor {
    static constexpr UINT message=WM_APP+222;
    HWND receiver=CreateWindowExW(0,L"STATIC",L"library test",0,0,0,0,0,HWND_MESSAGE,nullptr,nullptr,nullptr);
    ui::detail::DirectoryChangeMonitor monitor;
    std::vector<ui::detail::DirectoryChangeNotification> events;
    void Pump(DWORD ms) {
        const auto end=GetTickCount64()+ms;
        do {
            MSG message_item{};
            while(PeekMessageW(&message_item,receiver,message,message,PM_REMOVE)) {
                // PeekMessage also returns WM_QUIT regardless of its filter.
                if(message_item.message!=message) continue;
                std::unique_ptr<ui::detail::DirectoryChangeNotification> item(
                    reinterpret_cast<ui::detail::DirectoryChangeNotification*>(message_item.lParam));
                Require(item!=nullptr,"empty directory notification");
                if(monitor.Accept(item->generation)) {
                    events.push_back(*item);
                    std::ofstream("monitor-events.txt",std::ios::app) << item->action << " " << item->path << '\n';
                }
            }
            Sleep(5);
        } while(GetTickCount64()<end);
    }
    bool Saw(const fs::path& path,DWORD action) const {
        return std::any_of(events.begin(),events.end(),[&](const auto& item) {
            return item.action==action && item.path==path;
        });
    }
    bool Wait(const fs::path& path,DWORD action,DWORD timeout=4000) {
        const auto end=GetTickCount64()+timeout;
        do { Pump(20); if(Saw(path,action)) return true; } while(GetTickCount64()<end);
        return false;
    }
    ~Monitor() { monitor.Stop(); Pump(5); DestroyWindow(receiver); }
};
void CheckMonitor(const fs::path& root) {
    const auto flat=root/L"flat", recursive=root/L"recursive";
    fs::create_directories(flat/L"sub"); fs::create_directories(recursive/L"sub");
    Monitor monitor;
    const std::vector<ui::detail::DirectoryWatchPath> paths{{flat,false},{recursive,true}};
    Require(monitor.monitor.Start(monitor.receiver,Monitor::message,paths),"monitor start");
    Require(monitor.Wait(flat,ui::detail::kDirectoryRescan) &&
            monitor.Wait(recursive,ui::detail::kDirectoryRescan),"initial reconciliation notification");
    Wave(flat/L"one.wav"); Wave(flat/L"sub"/L"hidden.wav"); Wave(recursive/L"sub"/L"two.wav");
    Require(monitor.Wait(flat/L"one.wav",FILE_ACTION_ADDED),"unchecked folder must still be monitored");
    Require(monitor.Wait(recursive/L"sub"/L"two.wav",FILE_ACTION_ADDED),"recursive event missing");
    monitor.Pump(100);
    Require(!monitor.Saw(flat/L"sub"/L"hidden.wav",FILE_ACTION_ADDED),"nonrecursive watch entered subtree");
    // Only fixture files are removed; the remaining watch must survive failure.
    fs::remove(flat/L"one.wav"); fs::remove(flat/L"sub"/L"hidden.wav");
    fs::remove(flat/L"sub"); fs::remove(flat);
    monitor.Pump(200);
    Wave(recursive/L"still-live.wav");
    Require(monitor.Wait(recursive/L"still-live.wav",FILE_ACTION_ADDED),"one failed root stopped another watch");
    monitor.events.clear(); fs::create_directories(flat);
    Require(monitor.Wait(flat,ui::detail::kDirectoryRescan),"failed root was not reopened");
    Wave(flat/L"back.wav");
    Require(monitor.Wait(flat/L"back.wav",FILE_ACTION_ADDED),"reopened root is not monitored");
}
void CheckTtbl(const fs::path& root) {
    playlist::Track track; track.path=root/L"record.wav"; track.title="title";
    track.duration_ms=1000; track.media_type="PCM"; track.sample_rate_hz=44100;
    track.channels=2; track.bits_per_sample=24; track.bitrate_bps=0x80012345U; track.rating=4;
    for(bool tags:{false,true}) {
        if(tags) { track.artist="Artist"; track.album="Album"; track.metadata={{"Genre","Jazz"}}; }
        playlist::Playlist list; list.Add(track); const auto file=root/(tags?L"tags.ttbl":L"plain.ttbl");
        list.SaveTtbl(file); playlist::Playlist loaded; loaded.LoadTtbl(file);
        const auto& copy=loaded.Tracks().at(0);
        Require(copy.media_type==track.media_type && copy.sample_rate_hz==44100 && copy.channels==2 &&
                copy.bits_per_sample==24 && copy.bitrate_bps==0x80012345U && copy.rating==4,
                "TTBL detailed audio payload did not round-trip");
        HANDLE lock=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        Require(lock!=INVALID_HANDLE_VALUE,"lock fixture");
        list.SetRating(0,1); bool failed{};
        try { list.SaveTtbl(file); } catch(const std::exception&) { failed=true; }
        CloseHandle(lock); Require(failed,"locked destination unexpectedly replaced");
        loaded.LoadTtbl(file); Require(loaded.Tracks()[0].rating==4,"failed save damaged previous file");
    }
}
}

namespace ttplayer::testing {
struct ProgressSeekAccess {
    using State=ui::PlayerWindow::MediaLibraryState;
    struct Fixture {
        ui::PlayerWindow p{QuietSettings()};
        Fixture() { p.playlists_.NewList(L"ordinary"); p.media_library_=std::make_shared<State>(); p.media_library_->persistence_loaded=true; }
        ~Fixture() { p.media_library_.reset(); } // No application persistence from unit fixtures.
    };
    static void Query(ui::PlayerWindow& p,State::NodeKind kind=State::NodeKind::root,std::string key={}) {
        State::Node node; node.kind=kind; node.key=std::move(key);
        NMTREEVIEWW event{}; event.hdr.hwndFrom=p.playlist_tree_control_; event.hdr.code=TVN_SELCHANGEDW;
        event.itemNew.lParam=reinterpret_cast<LPARAM>(&node);
        Require(p.HandleMediaLibraryTreeNotification(&event.hdr),"query notification rejected");
    }
    static std::unique_ptr<State::BuildResult> Build(State& state,std::vector<playlist::Track> tracks,
            std::vector<ui::detail::DirectoryWatchPath> directories={}) {
        State::BuildRequest request; request.instance=state.instance; request.generation=state.generation;
        request.revision=state.revision; request.runtime=fs::current_path(); request.directories=std::move(directories);
        request.extensions={L".wav"}; request.control=std::make_shared<State::WorkerControl>();
        for(auto& track:tracks) request.seeds.push_back({std::move(track),{}});
        auto control=request.control; State::RunBuild(std::move(request));
        Require(control->unposted_result!=nullptr,"build did not produce result");
        return std::move(control->unposted_result);
    }
    static void Apply(ui::PlayerWindow& p,std::unique_ptr<State::BuildResult> result) {
        p.ApplyMediaLibraryIndex(reinterpret_cast<LPARAM>(result.release())); Query(p);
    }
    static playlist::Track Song(std::wstring name) {
        playlist::Track track; track.path=L"https://fixture.invalid/"+name;
        track.title="Title"; track.artist="Artist"; track.rating=1; track.duration_ms=1000; track.media_type="test";
        return track;
    }
    static void Seed(Fixture& f,const playlist::Track& track) {
        f.p.media_library_->items.push_back({track,State::Identity(track),{}});
        f.p.media_library_->persisted_tracks.push_back(track); Query(f.p);
    }
    static void CheckTransactions(const fs::path& root) {
        Fixture fixture; auto& p=fixture.p; auto& state=*p.media_library_;
        auto track=Song(L"one"); Seed(fixture,track);
        p.media_library_playback_=ui::BuildMediaLibraryPlaybackSnapshot({&track,1},0);
        p.media_library_playback_active_=true; p.current_=0;
        auto result=Build(state,{track}); p.SetVisiblePlaylistRating(0,5); Apply(p,std::move(result));
        Require(state.items[0].track.rating==5 && state.persisted_tracks[0].rating==5 &&
                p.media_library_playback_.Tracks()[0].rating==5,"scan overwrote newer rating");
        result=Build(state,state.persisted_tracks);
        auto edited=state.items[0].track; edited.artist="Edited"; edited.album="New album";
        p.UpdateMediaLibraryTrackByIdentity(track,edited); Apply(p,std::move(result));
        Require(state.items[0].track.artist=="Edited" && state.persisted_tracks[0].artist=="Edited", "scan overwrote tag edit");
        result=Build(state,state.persisted_tracks); auto second=Song(L"second");
        Require(p.CommitMediaLibraryTracks({second},0,false),"commit fixture"); Apply(p,std::move(result));
        Require(state.result_tracks.size()==2,"scan lost a new committed item");
        result=Build(state,state.persisted_tracks);
        Require(p.UpdateMediaLibraryTrackPath(second,L"https://fixture.invalid/renamed"),"rename fixture"); Apply(p,std::move(result));
        Require(state.result_tracks.size()==2 && std::none_of(state.result_tracks.begin(),state.result_tracks.end(),
            [&](const auto& item) { return item.path==second.path; }),"scan revived old renamed identity");

        playlist::Track blank=Song(L"blank"); blank.artist.clear(); blank.rating=0;
        state.items.push_back({blank,State::Identity(blank),{}});
        Query(p,State::NodeKind::category,"Artist");
        Require(state.result_tracks.size()==1 && state.result_tracks[0].artist.empty(),"Artist parent must select missing artist");
        Query(p,State::NodeKind::category,"Rating"); Require(state.result_tracks.empty(),"Rating parent must be empty");

        Require(ui::IsSameOrBelowPath(L"C:\\song.wav",L"C:\\"),"drive root containment");
        Require(ui::IsSameOrBelowPath(L"\\\\server\\share\\song.wav",L"\\\\server\\share\\"),"UNC root containment");
        Require(!ui::IsSameOrBelowPath(L"C:\\musical\\song.wav",L"C:\\music"),"path prefix boundary");

        Fixture revive; auto& r=revive.p; auto& rs=*r.media_library_;
        playlist::Track local; local.path=root/L"revived.wav"; Wave(local.path); Seed(revive,local);
        const auto id=State::Identity(local);
        r.RemoveMediaLibraryTracksByIdentity({id}); Apply(r,Build(rs,rs.persisted_tracks));
        Require(rs.items.empty() && rs.excluded.contains(id),"remove fixture did not compact");
        Monitor receiver; std::vector<ui::detail::DirectoryWatchPath> paths{{root,false}};
        rs.monitor.Start(receiver.receiver,Monitor::message,paths);
        // Capture the actual generation without using the other monitor's filter.
        MSG event{}; const auto deadline=GetTickCount64()+3000;
        while(!PeekMessageW(&event,receiver.receiver,Monitor::message,Monitor::message,PM_REMOVE) && GetTickCount64()<deadline) Sleep(5);
        Require(event.lParam!=0,"revival monitor did not start");
        std::unique_ptr<ui::detail::DirectoryChangeNotification> notification(
            reinterpret_cast<ui::detail::DirectoryChangeNotification*>(event.lParam));
        const auto generation=notification->generation;
        notification->action=FILE_ACTION_ADDED; notification->path=local.path; rs.indexing=true;
        r.HandleMediaLibraryDirectoryChange(FILE_ACTION_ADDED,reinterpret_cast<LPARAM>(notification.release()));
        Apply(r,Build(rs,{local})); Require(rs.result_tracks.size()==1 && !rs.excluded.contains(id),"reappearing path stayed excluded");
        auto stale=Build(rs,{local},{{root,false}});
        // A deletion arrives after the worker observed the path, before delivery.
        auto* removed=new ui::detail::DirectoryChangeNotification{generation,FILE_ACTION_REMOVED,local.path};
        r.HandleMediaLibraryDirectoryChange(FILE_ACTION_REMOVED,reinterpret_cast<LPARAM>(removed));
        Apply(r,std::move(stale));
        Require(rs.excluded.contains(id) && std::none_of(rs.result_tracks.begin(),rs.result_tracks.end(),
            [&](const auto& track) { return track.path==local.path; }),"old scan revived a newer deletion");
        rs.monitor.Stop();
    }
    static void CheckReadAndScan(const fs::path& root) {
        const auto folder=root/L"scan"; Wave(folder/L"one.wav"); Wave(folder/L"sub"/L"two.wav");
        Fixture f; auto& state=*f.p.media_library_;
        auto flat=Build(state,{},{{folder,false}});
        Require(flat->items.size()==1,"nonrecursive initial scan descended into child folder");
        const auto& wave=flat->items[0].track;
        Require(wave.duration_ms==1000 && wave.sample_rate_hz==44100 && wave.channels==1 &&
                wave.bits_per_sample==16 && wave.bitrate_bps==705600 && !wave.media_type.empty(),"built-in WAV metadata missing");
        Require(Build(state,{},{{folder,true}})->items.size()==2,"recursive initial scan missing child");
        auto absent=wave; absent.path=folder/L"absent.wav";
        Require(Build(state,{absent},{{folder,false}})->missing.size()==1,"offline deletion was not reconciled");

        Fixture startup; Seed(startup,wave); startup.p.settings_.player.playing_file_name=wave.path.wstring();
        startup.p.media_library_->persistence_loaded=false; startup.p.media_library_->indexing=true;
        startup.p.RestoreStartupPlayback(); Require(startup.p.media_library_startup_pending_,"startup did not wait for library");
        auto ready=Build(*startup.p.media_library_,{wave}); ready->persistence_attempted=true;
        Apply(startup.p,std::move(ready));
        Require(startup.p.media_library_playback_active_ && startup.p.current_==size_t{0} &&
                startup.p.media_library_playback_.Tracks()[0].path==wave.path &&
                !startup.p.settings_.player.playing_file_name.empty(),"library-only playback was not restored");

        Fixture numbered; auto& p=numbered.p; p.settings_.library.enabled=false; p.settings_.playlist.library_mode=false;
        const auto active=p.playlists_.ActiveIndex(); const auto target=p.MediaLibraryMonitorPlaylist();
        auto monitored=Build(*p.media_library_,{},{{folder,false}});
        monitored->numbered_target=p.playlists_.Entries()[target].slot;
        p.ApplyMediaLibraryIndex(reinterpret_cast<LPARAM>(monitored.release()));
        Require(p.playlists_.ActiveIndex()==active && p.playlists_.At(target).Tracks().size()==1,
                "disabled-library monitor did not update default list independently");
    }
    template<class Predicate> static bool PumpUntil(Predicate done,DWORD timeout=12000) {
        const auto end=GetTickCount64()+timeout;
        do {
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message!=WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if(done()) return true;
            Sleep(5);
        } while(GetTickCount64()<end);
        return false;
    }
    static void CheckWindowsLifecycle(const fs::path& root) {
        const auto watched=root/L"startup-watch";
        Wave(watched/L"remembered.wav"); Wave(watched/L"sub"/L"outside.wav");
        auto settings=QuietSettings();
        settings.source_path=root/settings::kSettingsFileName;
        settings.library.monitor_directories=true; settings.library.directories={{watched,false}};
        settings.player.playing_file_name=(watched/L"remembered.wav").wstring();
        const auto resources=LoadLibraryExW(L"ttpres.dll",nullptr,LOAD_LIBRARY_AS_DATAFILE);
        Require(resources!=nullptr,"lifecycle resources");
        {
            ui::PlayerWindow p(settings); p.SetSkinResourceModule(resources);
            Require(p.LoadSkinResource(resources) && p.Create(GetModuleHandleW(nullptr),SW_HIDE),"lifecycle window");
            p.RestoreStartupPlayback();
            Require(PumpUntil([&] { return p.media_library_playback_active_ && p.media_library_ &&
                    !p.media_library_->indexing && p.media_library_->workers.empty(); }),"startup scan/restore did not finish");
            Query(p); Require(p.media_library_->result_tracks.size()==1,"initial unchecked directory scan scope");
            Require(p.media_library_playback_.Tracks().at(0).path==watched/L"remembered.wav", "startup remembered wrong song");
            p.SetVisiblePlaylistRating(0,5);
            Require(p.CommitMediaLibraryTracks({Song(L"just-added")},0,false),"close-time commit");
            // Close while the newly started worker is still outstanding.
            DestroyWindow(p.window_);
        }
        playlist::Playlist stored; stored.LoadTtbl(root/L"Music.library");
        Require(stored.Tracks().size()==2 && std::any_of(stored.Tracks().begin(),stored.Tracks().end(),
            [&](const auto& track) { return track.path==watched/L"remembered.wav" && track.rating==5; }),
            "close during indexing lost a rating or newly committed item");
        fs::rename(watched/L"remembered.wav",watched/L"renamed-offline.wav");
        settings.player.playing_file_name.clear();
        {
            ui::PlayerWindow p(settings); p.SetSkinResourceModule(resources);
            Require(p.LoadSkinResource(resources) && p.Create(GetModuleHandleW(nullptr),SW_HIDE),"reopen window");
            Require(PumpUntil([&] { return p.media_library_ && !p.media_library_->indexing &&
                    p.media_library_->workers.empty() && p.media_library_->persistence_loaded; }),"reopen reconciliation");
            Query(p);
            Require(p.media_library_->result_tracks.size()==2 && std::any_of(
                p.media_library_->result_tracks.begin(),p.media_library_->result_tracks.end(),
                [&](const auto& track) { return track.path==watched/L"renamed-offline.wav"; }),"offline rename did not reconcile");
            DestroyWindow(p.window_);
        }
        FreeLibrary(resources);
    }
    static void Run(const fs::path& root) {
        CheckTransactions(root); CheckReadAndScan(root);
        if(fs::exists(root/L"ttpres.dll")) {
            CheckWindowsLifecycle(root);
            std::ofstream("validation.txt") << "Core and original-resource window lifecycle tests passed.\n";
        } else std::ofstream("validation.txt") <<
            "Core tests passed; original-resource window lifecycle skipped (private ttpres.dll unavailable).\n";
    }
};
}

int wmain(int argc,wchar_t** argv) {
    if(argc>1 && std::wstring_view(argv[1])==app::kFileInfoWorkerSwitch)
        return app::RunFileInfoWorker(argc-1,argv+1);
    try {
        Require(argc>=2,"expected resource directory or fixture switch");
        if(std::wstring_view(argv[1])!=L"--fixture") {
            const auto runtime=fs::temp_directory_path()/(L"TTPlayerLibrary-"+
                std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
            fs::create_directories(runtime);
            const auto child=runtime/L"media_library_tests.exe";
            fs::copy_file(app::CurrentExecutablePath(),child);
            for(const auto name:{L"ttpcomm.dll",L"ttpres.dll"})
                if(fs::exists(fs::path(argv[1])/name)) fs::copy_file(fs::path(argv[1])/name,runtime/name);
            auto command=app::QuoteWorkerArgument(child.wstring())+L" --fixture";
            STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
            Require(CreateProcessW(child.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                nullptr,runtime.c_str(),&startup,&process),"cannot launch isolated tests");
            const auto waited=WaitForSingleObject(process.hProcess,80000);
            if(waited!=WAIT_OBJECT_0) TerminateProcess(process.hProcess,2);
            DWORD code{}; GetExitCodeProcess(process.hProcess,&code);
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
            std::wcout << L"Fixture: " << runtime << L'\n';
            if(code) {
                std::cout << "Fixture exit code: " << code << '\n';
                std::ifstream error(runtime/L"failure.txt"); if(error) std::cerr << error.rdbuf();
                std::ifstream events(runtime/L"monitor-events.txt"); if(events) std::cerr << events.rdbuf();
            }
            Require(waited==WAIT_OBJECT_0 && code==0,"isolated media library tests failed");
            std::ifstream validation(runtime/L"validation.txt"); std::cout << validation.rdbuf();
            return 0;
        }
        Require(SUCCEEDED(OleInitialize(nullptr)),"OLE initialization");
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        ui::detail::EnableEmbeddedFileInfoProbe();
        testing::ProgressSeekAccess::Run(fs::current_path());
        CheckTtbl(fs::current_path()); CheckMonitor(fs::current_path());
        OleUninitialize();
        std::cout << "library edit races, revival, startup, category queries, WAV scan, TTBL and directory recovery passed\n";
        return 0;
    } catch(const std::exception& error) {
        if(argc>1 && std::wstring_view(argv[1])==L"--fixture")
            std::ofstream("failure.txt") << error.what() << '\n';
        std::cerr << "media library: " << error.what() << '\n'; return 1;
    }
}
