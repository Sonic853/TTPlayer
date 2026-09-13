#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"

#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace ttplayer::testing {
// Use the existing skin-lifecycle seam. Tests run real HWND operations, not a
// boolean-only model: User32 propagates topmost changes along the owner chain.
struct SkinRebindAccess {
    static void Require(bool ok, const char* message) {
        if (!ok) throw std::runtime_error(message);
    }
    static void CheckWindow(HWND window, bool topmost, const char* message) {
        Require(window && IsWindow(window), "missing test HWND");
        if (((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != topmost)
            std::cerr << "HWND=" << window << " owner=" << GetWindow(window, GW_OWNER)
                      << " expected=" << topmost << " exstyle="
                      << std::hex << GetWindowLongPtrW(window, GWL_EXSTYLE) << std::dec << '\n';
        Require(((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) == topmost,
                message);
    }
    static void Check(ui::PlayerWindow& p, const char* phase) {
        std::cout << phase << " mini=" << p.mini_mode_ << std::endl;
        const bool main = p.mini_mode_ ? p.settings_.player.mini_top_most
                                      : p.settings_.player.top_most;
        const bool lyric = main || p.ActiveLyricTopMost();
        const bool desktop = lyric || p.settings_.desktop_lyric.topmost;
        CheckWindow(p.window_, main, "main topmost preference/native state mismatch");
        CheckWindow(p.lyric_window_, lyric, "lyric effective topmost mismatch");
        CheckWindow(p.playlist_window_, main, "playlist owner topmost mismatch");
        CheckWindow(p.equalizer_window_, main, "equalizer owner topmost mismatch");
        CheckWindow(p.desktop_lyrics_.ControlHandle(), desktop, "desktop control topmost mismatch");
        CheckWindow(p.desktop_lyrics_.PaintHandle(), desktop, "desktop paint topmost mismatch");
        CheckWindow(p.desktop_lyrics_.BarHandle(), desktop, "desktop toolbar topmost mismatch");
    }
    static void Pin(ui::PlayerWindow& p, bool main, bool lyric) {
        using namespace ui::detail;
        const bool current = p.mini_mode_ ? p.settings_.player.mini_top_most
                                         : p.settings_.player.top_most;
        if (current != main)
            Require(p.HandleContextCommand(kCmdAlwaysOnTop), "main topmost command not handled");
        Check(p, "main pin/unpin");
        if (p.ActiveLyricTopMost() != lyric)
            Require(p.HandleLyricCommand(kCmdLyricTopMost), "lyric topmost command not handled");
        Check(p, "lyric pin/unpin");
    }
    static void Mini(ui::PlayerWindow& p, bool enabled) {
        p.ToggleMiniMode();
        p.CompleteSkinWindowFadeForReplacement();
        Require(p.mini_mode_ == enabled, "mini transition failed");
        Check(p, enabled ? "enter mini" : "leave mini");
    }
    static void FullscreenRestore(ui::PlayerWindow& p, int mode) {
        // Exercise the real detach/reparent/restore paths on small off-screen
        // surfaces. No audio device, fake playback or monitor takeover needed.
        p.fullscreen_saved_lyric_transparent_ = p.settings_.lyric.fullscreen_transparent;
        p.fullscreen_main_was_visible_ = IsWindowVisible(p.window_) != FALSE;
        p.fullscreen_main_was_iconic_ = IsIconic(p.window_) != FALSE;
        p.fullscreen_lyric_window_was_visible_ = IsWindowVisible(p.lyric_window_) != FALSE;
        p.fullscreen_desktop_lyric_was_visible_ = false;
        p.fullscreen_mode_ = mode;
        const RECT target{3000, 3000, 3160, 3080};
        if (mode != 1) p.DetachVisualWindow(target);
        if (mode != 2) p.DetachLyricControl(target, HWND_TOPMOST);
        Check(p, "fullscreen detached surfaces");
        p.LeaveFullScreen();
        Require(!p.fullscreen_visual_detached_ && !p.fullscreen_lyric_detached_,
                "fullscreen controls were not restored");
        Require(GetParent(p.visual_window_) == p.window_ &&
                GetParent(p.lyric_control_) == p.lyric_window_, "fullscreen parent not restored");
        Check(p, "fullscreen return");
    }
    static void Run(const fs::path& runtime, HMODULE resources, HMODULE comm, bool startup_top,
                    bool startup_mini = false) {
        using namespace ui::detail;
        settings::Settings settings;
        settings.source_path = runtime / L"TTPlayer.xml";
        settings.skin_file = L"<Default_Skin>";
        settings.general.fade_windows = false;
        settings.general.tray_icon = false;
        settings.general.send_title_to_msn = false;
        settings.lyric.auto_download = false;
        settings.player.mute = true;
        settings.player.volume = 0;
        settings.player.top_most = startup_mini ? !startup_top : startup_top;
        settings.player.mini_top_most = startup_top;
        settings.player.mini_mode = startup_mini;
        settings.player.lyric_top_most = false;
        settings.desktop_lyric.topmost = false;
        settings.player.player_window = {3000, 3000, 3327, 3141};
        settings.player.lyric_window = {3000, 3450, 3327, 3572};
        settings.player.playlist_window = {3000, 3200, 3327, 3446};
        settings.player.equalizer_window = {3400, 3000, 3727, 3116};
        settings.player.lyric_visible = settings.player.playlist_visible =
            settings.player.equalizer_visible = true;
        ui::PlayerWindow p(settings);
        p.SetSkinResourceModule(resources);
        p.SetTtpCommModule(comm);
        Require(p.LoadSkinResource(resources), "cannot load default skin");
        Require(p.Create(GetModuleHandleW(nullptr), SW_HIDE), "cannot create test player");
        try {
            p.CompleteSkinWindowFadeForReplacement();
            Check(p, "startup");
            Require(p.mini_mode_ == startup_mini, "startup mode was not restored");
            if (startup_mini) {
                Mini(p, false);
                Mini(p, true);
                DestroyWindow(p.window_);
                return;
            }
            const HWND original = p.window_;
            for (bool desktop : {false, true}) {
                p.settings_.desktop_lyric.topmost = desktop;
                p.desktop_lyrics_.ApplySettings();
                Check(p, "apply desktop lyric settings");
                for (bool lyric : {false, true}) for (bool main : {true, false}) {
                    Pin(p, main, lyric);
                    // Desktop settings/visibility must not demote their owners.
                    p.desktop_lyrics_.ApplySettings();
                    Check(p, "desktop settings after owner pin");
                    p.desktop_lyrics_.Show(true);
                    Check(p, "show desktop lyrics");
                    p.desktop_lyrics_.Show(false);
                    Check(p, "hide desktop lyrics");
                    for (const auto name : {L"LX-iPlay.skn", L"TT2012.skn",
                                            L"Let's Vista (浅蓝+蓝灰).skn"}) {
                        p.skin_commands_ = {{kCmdFirstSkin, runtime / L"Skin" / name, name, {}, false}};
                        Require(p.HandleContextCommand(kCmdFirstSkin), "skin command not handled");
                        Require(p.settings_.skin_file == name, "skin selector not committed");
                        Check(p, "skin menu switch");
                    }
                    Require(p.HandleContextCommand(kCmdDefaultSkin), "default skin command not handled");
                    Check(p, "return to default skin");

                    // Opposite and equal mode preferences, including changing
                    // the actual menu check while mini is active.
                    for (bool mini : {false, true}) {
                        p.settings_.player.mini_top_most = mini;
                        p.settings_.player.mini_lyric_top_most = !lyric;
                        Mini(p, true);
                        Pin(p, !mini, lyric);
                        Mini(p, false);
                        Require(p.settings_.player.top_most == main &&
                                p.settings_.player.lyric_top_most == lyric,
                                "mini pin/unpin overwrote normal preferences");
                        Mini(p, true);
                        Require(p.settings_.player.mini_top_most == !mini,
                                "mini topmost preference did not survive return");
                        // A skin command exits mini before rebinding the normal HWND.
                        Require(p.LoadSkinPackage(runtime / L"Skin" / L"TT2012.skn"),
                                "mini skin replacement failed");
                        Require(!p.mini_mode_, "skin replacement stayed in mini mode");
                        Check(p, "mini skin replacement");
                        Require(p.HandleContextCommand(kCmdDefaultSkin), "default restore failed");
                    }
                    p.EnterDesktopLyricMode();
                    Check(p, "enter desktop lyric mode");
                    Mini(p, true);
                    Mini(p, false);
                    p.LeaveDesktopLyricMode();
                    Check(p, "leave desktop lyric mode");
                    // Two rapid commands during the asynchronous fade must
                    // return to normal without applying a stale mini band.
                    p.settings_.general.fade_windows = true;
                    p.ToggleMiniMode();
                    p.ToggleMiniMode();
                    p.CompleteSkinWindowFadeForReplacement();
                    Require(!p.mini_mode_, "queued mini toggle did not return to normal");
                    Check(p, "queued mini fade toggles");
                    for (bool fade : {false, true}) {
                        p.settings_.general.fade_windows = fade;
                        for (HWND window : {p.window_, p.lyric_window_, p.playlist_window_, p.equalizer_window_}) {
                            p.SetSkinWindowVisible(window, false);
                            p.CompleteSkinWindowFadeForReplacement();
                            Check(p, "hide/fade window");
                            p.SetSkinWindowVisible(window, true);
                            p.CompleteSkinWindowFadeForReplacement();
                            Check(p, "show/fade window");
                        }
                    }
                    p.settings_.general.fade_windows = false;
                    ShowWindow(p.window_, SW_MINIMIZE);
                    Check(p, "minimize");
                    ShowWindow(p.window_, SW_RESTORE);
                    Check(p, "restore");
                    p.settings_.player.window_shadow = !p.settings_.player.window_shadow;
                    p.ApplyWindowShadow();
                    Check(p, "shadow class update");
                    p.ApplyOptionsChangeMask(0xffff, -2);
                    Check(p, "apply all settings");
                    for (int mode : {1, 2, 3}) FullscreenRestore(p, mode);
                    Require(p.settings_.player.top_most == main &&
                            p.settings_.player.lyric_top_most == lyric &&
                            p.settings_.desktop_lyric.topmost == desktop,
                            "effective bands overwrote independent preferences");
                    Require(p.window_ == original, "topmost repair recreated main HWND");
                }
            }
            DestroyWindow(p.window_);
        } catch (...) {
            p.LeaveFullScreen();
            if (IsWindow(p.window_)) DestroyWindow(p.window_);
            throw;
        }
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected repository root or --isolated");
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        if (std::wstring_view(argv[1]) != L"--isolated") {
            const fs::path repository = argv[1];
            for (const auto relative : {L"ttpres.dll", L"ttpcomm.dll", L"Skin/LX-iPlay.skn",
                    L"Skin/TT2012.skn", L"Skin/Let's Vista (浅蓝+蓝灰).skn"})
                if (!fs::exists(repository / relative)) {
                    std::wcout << L"missing native fixture: " << relative << L'\n';
                    return 77;
                }
            const auto target = fs::temp_directory_path() / (L"TTPlayerTopmost-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            fs::create_directories(target / L"Skin");
            fs::create_directories(target / L"PlayList");
            fs::copy_file(executable, target / L"window_topmost_tests.exe");
            for (const auto relative : {L"ttpres.dll", L"ttpcomm.dll", L"Skin/LX-iPlay.skn",
                    L"Skin/TT2012.skn", L"Skin/Let's Vista (浅蓝+蓝灰).skn"})
                fs::copy_file(repository / relative, target / relative);
            std::wstring command = L"\"" + (target / L"window_topmost_tests.exe").wstring() + L"\" --isolated";
            STARTUPINFOW startup{sizeof(startup)};
            startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION child{};
            if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                                nullptr, target.c_str(), &startup, &child))
                throw std::runtime_error("cannot start isolated topmost regression");
            CloseHandle(child.hThread);
            const DWORD waited = WaitForSingleObject(child.hProcess, 45000);
            DWORD status = 1;
            if (waited == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &status);
            else { TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, 3000); }
            CloseHandle(child.hProcess);
            std::wcout << L"isolated topmost artifacts: " << target.wstring() << L'\n';
            return static_cast<int>(status);
        }
        const HRESULT ole = OleInitialize(nullptr);
        if (FAILED(ole)) throw std::runtime_error("OLE initialization failed");
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&common);
        const auto runtime = fs::path(executable).parent_path();
        HMODULE resources = LoadLibraryExW((runtime / L"ttpres.dll").c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        HMODULE comm = LoadLibraryW((runtime / L"ttpcomm.dll").c_str());
        if (!resources || !comm) throw std::runtime_error("cannot load fixture DLLs");
        for (bool topmost : {true, false}) {
            ttplayer::testing::SkinRebindAccess::Run(runtime, resources, comm, topmost);
            ttplayer::testing::SkinRebindAccess::Run(runtime, resources, comm, topmost, true);
        }
        FreeLibrary(comm); FreeLibrary(resources); OleUninitialize();
        std::cout << "native window topmost tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "window_topmost_tests: " << error.what() << '\n';
        return 1;
    }
}
