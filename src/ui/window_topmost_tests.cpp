#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"

#include <iostream>
#include <future>
#include <stdexcept>
#include <thread>

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
                      << std::hex << GetWindowLongPtrW(window, GWL_EXSTYLE) << std::dec
                      << " hung=" << IsHungAppWindow(window) << '\n';
        Require(((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) == topmost,
                message);
    }
    static bool Above(HWND first, HWND second) {
        for (HWND h = GetWindow(second, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV))
            if (h == first) return true;
        return false;
    }
    struct OtherThreadWindow {
        HWND window{};
        DWORD thread_id{};
        std::thread worker;
        OtherThreadWindow() {
            std::promise<std::pair<HWND, DWORD>> ready;
            auto future = ready.get_future();
            worker = std::thread([&ready] {
                HWND h = CreateWindowExW(0, L"STATIC", L"Z-order witness", WS_POPUP,
                    3000, 3000, 80, 80, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                ready.set_value({h, GetCurrentThreadId()});
                if (!h) return;
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                DestroyWindow(h);
            });
            const auto result = future.get();
            window = result.first; thread_id = result.second;
            if (!window) { worker.join(); throw std::runtime_error("cannot create Z-order witness"); }
        }
        ~OtherThreadWindow() { PostThreadMessageW(thread_id, WM_QUIT, 0, 0); worker.join(); }
    };
    static void ActivationOrder(ui::PlayerWindow& p) {
        OtherThreadWindow witness;
        struct Observation { unsigned owner_raises{}; } observed;
        const auto watch = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                              UINT_PTR, DWORD_PTR data) -> LRESULT {
            if (message == WM_WINDOWPOSCHANGING && lparam) {
                const auto& position = *reinterpret_cast<const WINDOWPOS*>(lparam);
                // User32 resolves HWND_TOP to a real insert-after HWND when
                // the owner must stay below one of its own popups.
                if ((position.flags & (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER)) ==
                        (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
                    ++reinterpret_cast<Observation*>(data)->owner_raises;
            }
            return DefSubclassProc(window, message, wparam, lparam);
        };
        constexpr UINT_PTR watch_id = 0x5a4f;
        Require(SetWindowSubclass(p.window_, watch, watch_id, reinterpret_cast<DWORD_PTR>(&observed)),
            "cannot observe activation Z-order requests");
        bool cross_thread = true, internal = true, inactive = true;
        for (HWND auxiliary : {p.lyric_window_, p.equalizer_window_, p.playlist_window_}) {
            const auto interleave = [&] {
                SetWindowPos(witness.window, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                SetWindowPos(auxiliary, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                observed = {};
            };
            interleave();
            // Deliver the same WM_ACTIVATE boundary as a click from another
            // GUI thread; no external user's window or foreground is needed.
            // Assert the native HWND_TOP/0x13 owner request, not whether a
            // different thread/foreground app reordered itself immediately
            // afterwards. Startup/dialog tests separately check actual Z order.
            SendMessageW(auxiliary, WM_ACTIVATE, WA_ACTIVE, reinterpret_cast<LPARAM>(witness.window));
            cross_thread = cross_thread && observed.owner_raises != 0;
            interleave();
            SendMessageW(auxiliary, WM_ACTIVATE, WA_ACTIVE, reinterpret_cast<LPARAM>(p.window_));
            internal = internal && observed.owner_raises == 0;
            observed.owner_raises = 0;
            SendMessageW(auxiliary, WM_ACTIVATE, WA_INACTIVE, reinterpret_cast<LPARAM>(witness.window));
            inactive = inactive && observed.owner_raises == 0;
        }
        RemoveWindowSubclass(p.window_, watch, watch_id);
        Require(cross_thread, "auxiliary activation did not request the original owner-group raise");
        Require(internal, "internal activation unexpectedly reordered the group");
        Require(inactive, "deactivation unexpectedly raised the group");
    }
    static void Check(ui::PlayerWindow& p, const char* phase) {
        // This matrix lives longer than User32's hung-window timeout. Like
        // Application::Run, keep servicing native messages between commands;
        // otherwise ghost HWNDs replace visible windows and invalidate real
        // Z-order/band observations even though the production loop is healthy.
        MSG message{};
        for (unsigned count = 0; count < 256 &&
             PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) continue; // Previous fixture teardown.
            if (!p.PreTranslateMessage(message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        Require(!IsHungAppWindow(p.window_), "test UI stopped servicing its message queue");
        std::cout << phase << " mini=" << p.mini_mode_ << std::endl;
        const bool main = p.mini_mode_ ? p.settings_.player.mini_top_most
                                      : p.settings_.player.top_most;
        const bool lyric = main || p.ActiveLyricTopMost();
        const bool desktop = lyric || p.settings_.desktop_lyric.topmost;
        CheckWindow(p.window_, main, "main topmost preference/native state mismatch");
        CheckWindow(p.lyric_window_, lyric, "lyric effective topmost mismatch");
        CheckWindow(p.playlist_window_, main, "playlist owner topmost mismatch");
        CheckWindow(p.equalizer_window_, main, "equalizer owner topmost mismatch");
        for (const HWND auxiliary : {p.lyric_window_, p.playlist_window_, p.equalizer_window_}) {
            if (IsWindowVisible(auxiliary) && IsWindowVisible(p.window_) && !IsIconic(p.window_)) {
                if (!Above(auxiliary, p.window_)) {
                    wchar_t name[128]{};
                    GetClassNameW(auxiliary, name, 128);
                    std::wcerr << L"below owner: " << name << L" window=" << auxiliary
                        << L" main=" << p.window_ << L" main_top=" << main
                        << L" lyric_top=" << lyric << L" desktop_top=" << desktop
                        << L" main_hung=" << IsHungAppWindow(p.window_)
                        << L" auxiliary_hung=" << IsHungAppWindow(auxiliary) << L'\n';
                }
                Require(Above(auxiliary, p.window_), "visible popup ended up below its owner");
            }
        }
        CheckWindow(p.desktop_lyrics_.ControlHandle(), desktop, "desktop control topmost mismatch");
        CheckWindow(p.desktop_lyrics_.PaintHandle(), desktop, "desktop paint topmost mismatch");
        CheckWindow(p.desktop_lyrics_.BarHandle(), desktop, "desktop toolbar topmost mismatch");
        if (p.options_window_ && IsWindow(p.options_window_))
            CheckWindow(p.options_window_, main, "options retained a stale owner topmost band");
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
        // Production fullscreen hides the normal owner before detaching its
        // controls. Keeping it visible here creates an impossible intermediate
        // Z-order state while the child is promoted to a separate popup.
        SetWindowPos(p.window_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
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
    static void DialogTransitions(ui::PlayerWindow& p, const fs::path& runtime) {
        using namespace ui::detail;
        const bool initial_pin = p.settings_.player.top_most;
        p.ShowOptions(0);
        Require(p.options_window_ && IsWindow(p.options_window_), "options did not open");
        const HWND original_options = p.options_window_;
        constexpr auto identity_key = L"TTPlayer.OptionsLifetimeTest";
        Require(SetPropW(original_options, identity_key, reinterpret_cast<HANDLE>(1)),
            "cannot tag the options HWND");
        // Native captioned owned windows model the nested file/color dialogs
        // launched from options; include an initially hidden grandchild.
        HWND nested = CreateWindowExW(0, L"STATIC", L"Owned dialog", WS_POPUP | WS_CAPTION | WS_VISIBLE,
            3000, 3000, 100, 80, p.options_window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND child = CreateWindowExW(0, L"STATIC", L"Nested dialog", WS_POPUP | WS_CAPTION,
            3000, 3000, 100, 80, nested, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND lyric_dialog = CreateWindowExW(0, L"STATIC", L"Lyric-owned dialog", WS_POPUP | WS_CAPTION,
            3000, 3000, 100, 80, p.lyric_window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(nested && child && lyric_dialog, "cannot create nested dialog fixtures");
        const auto check = [&] {
            Check(p, "options and nested dialog bands");
            Require(p.options_window_ == original_options &&
                GetPropW(original_options, identity_key) == reinterpret_cast<HANDLE>(1),
                "window transition destroyed/recreated the options sheet");
            const bool pin = p.mini_mode_ ? p.settings_.player.mini_top_most : p.settings_.player.top_most;
            CheckWindow(nested, pin, "nested dialog retained stale topmost state");
            CheckWindow(child, pin, "hidden grandchild dialog retained stale topmost state");
            CheckWindow(lyric_dialog, pin || p.ActiveLyricTopMost(), "lyric-owned dialog lost independent pin inheritance");
            Require(Above(nested, p.options_window_), "dialog went behind its options owner");
        };
        p.ApplySkinWindowTopMost(); check();
        for (bool main : {false, true, false, true}) {
            const bool sibling_order = Above(p.playlist_window_, p.equalizer_window_);
            Pin(p, main, false); check();
            Require(Above(p.playlist_window_, p.equalizer_window_) == sibling_order,
                "pin/unpin reversed the playlist/equalizer stacking order");
            // A no-op reconciliation must not reorder siblings or dialogs.
            const bool order = Above(p.playlist_window_, p.equalizer_window_);
            p.ApplySkinWindowTopMost(); check();
            Require(Above(p.playlist_window_, p.equalizer_window_) == order,
                "unchanged pin policy reordered sibling windows");
        }
        Pin(p, false, true); check();
        Pin(p, true, false); check();
        p.settings_.player.mini_top_most = false;
        Mini(p, true); check();
        ShowWindow(child, SW_SHOWNOACTIVATE); check();
        Mini(p, false); check();
        Require(p.LoadSkinPackage(runtime / L"Skin/LX-iPlay.skn"), "options-open skin switch failed");
        check();
        Require(p.HandleContextCommand(kCmdDefaultSkin), "options-open default restore failed");
        check();
        for (int mode : {1, 2, 3}) { FullscreenRestore(p, mode); check(); }
        DestroyWindow(child); DestroyWindow(nested); DestroyWindow(lyric_dialog);
        const HWND options = p.options_window_;
        SendMessageW(options, WM_COMMAND, IDOK, 0);
        Require(!IsWindow(options), "options Close button did not destroy the sheet");
        Pin(p, initial_pin, false);
        Check(p, "closing options restored the player group");
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
            Require(Above(p.playlist_window_, p.equalizer_window_) &&
                Above(p.equalizer_window_, p.lyric_window_) && Above(p.lyric_window_, p.window_),
                "startup window stacking differs from 00467B9B's lyric/EQ/playlist order");
            if (!startup_top) ActivationOrder(p);
            DialogTransitions(p, runtime);
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
