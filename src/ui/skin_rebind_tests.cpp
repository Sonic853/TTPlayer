#include "ttplayer/ui/player_window.h"
#include "ttplayer/core/text.h"
#include "player_window_internal.h"

#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <richedit.h>

namespace ttplayer::testing {
struct SkinRebindAccess {
    static inline int main_hides{}, destroyed{};
    static inline int hidden_zorders{}, hidden_geometry{}, nested_redraws{};
    static inline bool redraw_suspended{};
    static void Require(bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    }
    static LRESULT CALLBACK Watch(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                   UINT_PTR, DWORD_PTR) {
        if (message == WM_SHOWWINDOW && !wp) ++main_hides;
        if (message == WM_DESTROY) ++destroyed;
        if (message == WM_SETREDRAW) redraw_suspended = wp == FALSE;
        if (message == WM_WINDOWPOSCHANGING && lp && redraw_suspended &&
            !(reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_NOZORDER)) {
            ++hidden_zorders;
            std::cerr << "skin rebind Z-order request while main redraw is suspended; flags="
                << std::hex << reinterpret_cast<const WINDOWPOS*>(lp)->flags << std::dec << '\n';
        }
        if (message == WM_WINDOWPOSCHANGING && lp && redraw_suspended &&
            (reinterpret_cast<const WINDOWPOS*>(lp)->flags & (SWP_NOMOVE | SWP_NOSIZE)) !=
                (SWP_NOMOVE | SWP_NOSIZE)) ++hidden_geometry;
        return DefSubclassProc(window, message, wp, lp);
    }
    static LRESULT CALLBACK WatchAuxiliary(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                           UINT_PTR, DWORD_PTR) {
        if (message == WM_SETREDRAW && !wp && redraw_suspended) ++nested_redraws;
        return DefSubclassProc(window, message, wp, lp);
    }
    static std::vector<HWND> Windows(ui::PlayerWindow& p) {
        std::vector<HWND> result{p.window_, p.lyric_window_, p.playlist_window_, p.equalizer_window_,
            p.visual_window_, p.lyric_control_, p.lyric_close_, p.lyric_desklrc_, p.lyric_ontop_,
            p.lyric_hidden_button_, p.lyric_editor_, p.lyric_editor_toolbar_, p.playlist_tree_control_,
            p.playlist_list_control_, p.playlist_track_control_};
        for (const auto& item : p.equalizer_controls_) result.push_back(item.second);
        for (const auto& item : p.playlist_tool_controls_) result.push_back(item.second);
        return result;
    }
    static void Verify(ui::PlayerWindow& p, const std::vector<HWND>& original) {
        const auto current = Windows(p);
        for (size_t index = 0; index < original.size(); ++index) {
            Require(original[index] && IsWindow(original[index]), "skin destroyed a live HWND");
            Require(GetPropW(original[index], L"TTPlayer.SkinRebindTest") ==
                reinterpret_cast<HANDLE>(index + 1), "HWND was destroyed and its value recycled");
            Require(std::find(current.begin(), current.end(), original[index]) != current.end(),
                    "skin replaced a live control reference");
        }
        Require(!main_hides && !destroyed, "normal skin switch hid/recreated the main taskbar window");
        Require(!hidden_zorders, "skin rebind changed Z order while WM_SETREDRAW hid the main HWND");
        Require(!hidden_geometry, "skin geometry applied before WM_SETREDRAW(TRUE), unlike 0046D0C1");
        Require(!nested_redraws, "auxiliary skin rebind started while the main HWND was temporarily invisible");
        Require(IsWindowVisible(p.window_) != FALSE, "skin switch left the main window hidden");
        const bool main_top = p.mini_mode_ ? p.settings_.player.mini_top_most
                                          : p.settings_.player.top_most;
        Require(((GetWindowLongPtrW(p.window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) == main_top,
                "skin switch lost the main window's configured topmost state");
        Require(((GetWindowLongPtrW(p.lyric_window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) ==
                    (main_top || p.ActiveLyricTopMost()),
                "skin switch lost the lyric window's effective topmost state");
        Require(p.playlist_selected_rows_ == std::set<size_t>{1, 3} && p.playlist_selection_ == size_t{3} &&
                p.playlist_list_selection_ == size_t{0} && p.playlist_list_focus_ == size_t{0},
                "skin rebind reset playlist selection/caret");
        wchar_t text[128]{};
        GetWindowTextW(p.lyric_editor_, text, 128);
        Require(std::wstring(text) == L"[00:01.00]unsaved lyric\r\n[00:02.00]second line",
                "skin rebind discarded or rewrote the unsaved lyric document");
        Require(SendMessageW(p.lyric_editor_, EM_GETMODIFY, 0, 0) != 0,
                "skin rebind cleared the editor modified flag");
        CHARRANGE selected{};
        SendMessageW(p.lyric_editor_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selected));
        Require(selected.cpMin == 12 && selected.cpMax == 18, "skin rebind changed editor selection");
    }
    static void Run(const std::filesystem::path& runtime, HMODULE resources, HMODULE comm) {
        using namespace ui::detail;
        settings::Settings s;
        s.source_path = runtime / L"TTPlayer.xml";
        s.skin_file = L"<Default_Skin>";
        s.general.fade_windows = false;
        s.general.tray_icon = false;
        s.general.send_title_to_msn = false;
        s.player.mute = true;
        s.player.volume = 0;
        s.player.player_window = {3000, 3000, 3327, 3141};
        s.player.lyric_window = {3000, 3450, 3327, 3572};
        s.player.playlist_window = {3000, 3200, 3327, 3446};
        s.player.equalizer_window = {3400, 3000, 3727, 3116};
        s.player.lyric_visible = s.player.playlist_visible = s.player.equalizer_visible = true;
        s.lyric.auto_download = false;
        s.playlist.read_info_mode = 2;
        ui::PlayerWindow p(s);
        p.SetSkinResourceModule(resources);
        p.SetTtpCommModule(comm);
        Require(p.LoadSkinResource(resources), "default resource skin did not load");
        Require(p.Create(GetModuleHandleW(nullptr), SW_HIDE), "test player did not initialize");
        try {
            p.CompleteSkinWindowFadeForReplacement();
            for (int index = 0; index < 5; ++index) {
                playlist::Track track;
                track.path = runtime / (L"silent-" + std::to_wstring(index) + L".wav");
                track.title = "Fixture"; track.duration_ms = 10000;
                p.ActivePlaylist().Add(std::move(track));
            }
            p.RefreshPlaylist();
            p.playlist_selected_rows_ = {1, 3};
            p.playlist_selection_ = 3;
            Require(p.EnterLyricEditor(), "lyric editor fixture did not open");
            p.SetLyricEditorText(L"[00:01.00]unsaved lyric\r\n[00:02.00]second line", true);
            CHARRANGE selected{12, 18};
            SendMessageW(p.lyric_editor_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
            const auto original = Windows(p);
            for (size_t index = 0; index < original.size(); ++index)
                Require(SetPropW(original[index], L"TTPlayer.SkinRebindTest",
                    reinterpret_cast<HANDLE>(index + 1)) != FALSE, "cannot tag initial HWND");
            SetWindowSubclass(p.window_, Watch, 0x534b494e, 0);
            for (const auto auxiliary : {p.lyric_window_, p.playlist_window_, p.equalizer_window_})
                SetWindowSubclass(auxiliary, WatchAuxiliary, 0x534b494e, 0);
            main_hides = destroyed = 0;
            Verify(p, original);
            for (int round = 0; round < 4; ++round) {
                // Cover missing and existing sidecars, both main/lyric
                // settings and actual Win32 owner/owned topmost propagation.
                const bool topmost = (round & 1) == 0;
                const bool lyric_topmost = (round & 2) != 0;
                if (p.settings_.player.top_most != topmost)
                    Require(p.HandleContextCommand(kCmdAlwaysOnTop), "topmost menu was not handled");
                if (p.ActiveLyricTopMost() != lyric_topmost)
                    Require(p.HandleLyricCommand(kCmdLyricTopMost), "lyric topmost menu was not handled");
                for (const auto name : {L"LX-iPlay.skn", L"TT2012.skn", L"Let's Vista (浅蓝+蓝灰).skn"}) {
                    const auto path = runtime / L"Skin" / name;
                    // Exercise the real menu command route and the profile
                    // path, not only the lower-level bitmap loader.
                    p.skin_commands_ = {{kCmdFirstSkin, path, name, {}, false}};
                    Require(p.HandleContextCommand(kCmdFirstSkin), "skin menu was not handled");
                    Require(p.settings_.skin_file == name, "skin menu did not commit selector");
                    Verify(p, original);
                    std::cout << "round=" << round << " skin=" << core::WideToUtf8(name)
                              << " retained_hwnds=" << original.size() << '\n';
                    RECT bounds{}; GetWindowRect(p.playlist_window_, &bounds);
                    SetWindowPos(p.playlist_window_, nullptr, 0, 0,
                        bounds.right - bounds.left + 20, bounds.bottom - bounds.top + 20,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    p.UpdatePlaylistToolRects();
                    Verify(p, original);
                }
                Require(p.HandleContextCommand(kCmdDefaultSkin), "default menu was not handled");
                Require(p.settings_.skin_file == L"<Default_Skin>", "default selector not committed");
                Verify(p, original);
            }
            for (const auto name : {L"Classic.skn", L"DEFAULT_SKIN_579.skn",
                                    L"DEFAULT_SKIN__6120.skn", L"new/BaiduMusic8209.skn"}) {
                const auto path = runtime / L"Skin" / name;
                if (!std::filesystem::exists(path)) continue;
                for (bool pin : {false, true}) {
                    if (p.settings_.player.top_most != pin) p.HandleContextCommand(kCmdAlwaysOnTop);
                    Require(p.LoadSkinPackage(path), "additional package failed to load");
                    Verify(p, original);
                    Require(p.HandleContextCommand(kCmdDefaultSkin), "additional default return failed");
                    Verify(p, original);
                }
                std::cout << "retained HWNDs: " << core::WideToUtf8(name) << '\n';
            }
            const auto invalid = runtime / L"Skin" / L"broken.skn";
            { std::ofstream output(invalid); output << "not a skin archive"; }
            Require(!p.LoadSkinPackage(invalid), "invalid skin was accepted");
            Verify(p, original);
            // A profile's hidden auxiliary state must stay hidden. Resume
            // redraw only for previously visible windows (WM_SETREDRAW).
            ShowWindow(p.playlist_window_, SW_HIDE);
            ShowWindow(p.lyric_window_, SW_HIDE);
            ShowWindow(p.equalizer_window_, SW_HIDE);
            p.SaveCurrentSkinProfile();
            Require(p.LoadSkinPackage(runtime / L"Skin" / L"LX-iPlay.skn"), "hidden round trip failed");
            Require(p.LoadSkinResource(resources), "hidden default round trip failed");
            Require(!IsWindowVisible(p.playlist_window_) && !IsWindowVisible(p.lyric_window_) &&
                    !IsWindowVisible(p.equalizer_window_), "redraw/profile rebind revealed hidden auxiliary windows");
            Verify(p, original);
            Require(std::filesystem::exists(runtime / L"Skin" / L"Default.xml") &&
                    std::filesystem::exists(runtime / L"Skin" / L"LX-iPlay.skn.xml"),
                    "skin profile transaction was not persisted");
            p.ToggleMiniMode();
            p.CompleteSkinWindowFadeForReplacement();
            Require(p.mini_mode_, "mini fixture did not enter mini mode");
            Require(p.LoadSkinPackage(runtime / L"Skin" / L"TT2012.skn") && !p.mini_mode_,
                    "package switch did not leave mini mode on the original HWND");
            main_hides = 0; // mini exit intentionally hides/shows in 00464B6C
            Verify(p, original);
            RemoveWindowSubclass(p.window_, Watch, 0x534b494e);
            for (const auto auxiliary : {p.lyric_window_, p.playlist_window_, p.equalizer_window_})
                RemoveWindowSubclass(auxiliary, WatchAuxiliary, 0x534b494e);
            p.DestroyLyricEditor(); // discard this synthetic document without a save prompt
            DestroyWindow(p.window_);
        } catch (...) {
            if (p.window_ && IsWindow(p.window_)) {
                p.DestroyLyricEditor();
                DestroyWindow(p.window_);
            }
            throw;
        }
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    namespace fs = std::filesystem;
    try {
        if (argc != 2) throw std::runtime_error("expected repository root or --isolated");
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        if (std::wstring_view(argv[1]) != L"--isolated") {
            const auto target = fs::temp_directory_path() /
                (L"TTPlayerSkinRebind-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            fs::create_directories(target / L"Skin");
            fs::create_directories(target / L"PlayList");
            fs::copy_file(executable, target / L"skin_rebind_tests.exe");
            const fs::path repository = argv[1];
            for (const auto dll : {L"ttpres.dll", L"ttpcomm.dll"}) fs::copy_file(repository / dll, target / dll);
            for (const auto name : {L"LX-iPlay.skn", L"TT2012.skn", L"Let's Vista (浅蓝+蓝灰).skn"})
                fs::copy_file(repository / L"Skin" / name, target / L"Skin" / name);
            for (const auto name : {L"Classic.skn", L"DEFAULT_SKIN_579.skn",
                                    L"DEFAULT_SKIN__6120.skn", L"new/BaiduMusic8209.skn"}) {
                const auto source = repository / L"rebuild/build/Release/Skin" / name;
                if (!fs::exists(source)) continue;
                const auto destination = target / L"Skin" / name;
                fs::create_directories(destination.parent_path());
                fs::copy_file(source, destination);
            }
            std::wstring command = L"\"" + (target / L"skin_rebind_tests.exe").wstring() + L"\" --isolated";
            STARTUPINFOW startup{sizeof(startup)};
            startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION child{};
            if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                                nullptr, target.c_str(), &startup, &child))
                throw std::runtime_error("cannot start isolated skin regression");
            CloseHandle(child.hThread);
            const DWORD waited = WaitForSingleObject(child.hProcess, 60000);
            DWORD status = 1;
            if (waited == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &status);
            CloseHandle(child.hProcess);
            std::wcout << L"isolated skin test artifacts: " << target.wstring() << L'\n';
            return static_cast<int>(status);
        }
        const auto runtime = fs::path(executable).parent_path();
        const HRESULT ole = OleInitialize(nullptr);
        if (FAILED(ole)) throw std::runtime_error("OLE initialization failed");
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&common);
        HMODULE resources = LoadLibraryExW((runtime / L"ttpres.dll").c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        HMODULE comm = LoadLibraryW((runtime / L"ttpcomm.dll").c_str());
        if (!resources || !comm) throw std::runtime_error("fixture DLLs unavailable");
        ttplayer::testing::SkinRebindAccess::Run(runtime, resources, comm);
        FreeLibrary(comm); FreeLibrary(resources); OleUninitialize();
        std::cout << "skin rebind tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "skin rebind tests: " << error.what() << '\n';
        return 1;
    }
}
