#include "ttplayer/ui/player_window.h"
#include "ttplayer/ui/player_runtime_policy.h"
#include "player_window_internal.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;

namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Write(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    Require(static_cast<bool>(output << text), "cannot write fixture");
}
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "cannot read fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
DWORD RunChild(const fs::path& executable, const wchar_t* arguments, const fs::path& directory) {
    std::wstring command = L"\"" + executable.wstring() + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    Require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr,
        directory.c_str(), &startup, &child) != FALSE, "cannot start isolated player/test");
    CloseHandle(child.hThread);
    const DWORD waited = WaitForSingleObject(child.hProcess, 20000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &code);
    else TerminateProcess(child.hProcess, 1); // only this test's own child
    CloseHandle(child.hProcess);
    return code;
}
void StartupTests(const fs::path& runtime, const fs::path& built_executable) {
    Require(built_executable.filename() == L"TTPlayerRebuild.exe", "wrong CMake output executable name");
    const auto executable = runtime / L"TTPlayerRebuild.exe";
    fs::copy_file(built_executable, executable);
    const auto previous = runtime / L"TTPlayer.xml";
    const auto current = runtime / settings::kSettingsFileName;
    // Reuse only this test's synthetic settings; exercise the actual renamed
    // wWinMain's first-run migration and subsequent startup skin resolution.
    fs::rename(current, previous);
    const auto old_xml = Read(previous);
    Require(RunChild(executable, L"--smoke-test", runtime.parent_path()) == 0,
            "renamed player first launch failed");
    Require(fs::exists(current) && Read(previous) == old_xml, "startup migration changed old config");
    Require(SUCCEEDED(OleInitialize(nullptr)), "startup inspection OLE init failed");
    Require(settings::LoadLegacyXml(current).skin_file == L"new\\common.skn",
            "actual startup lost Skin/new selection");
    Write(previous, "<ttplayer><Player Volume=\"99\"/><Skin PackageName=\"common.skn\"/></ttplayer>");
    const auto changed_old = Read(previous);
    Require(RunChild(executable, L"--smoke-test", runtime.parent_path()) == 0,
            "renamed player second launch failed");
    Require(settings::LoadLegacyXml(current).skin_file == L"new\\common.skn" &&
            Read(previous) == changed_old, "subsequent startup reused/wrote old configuration");
    // Closing on an external skin leaves both its styles in the main XML
    // and its adjacent profile on disk. Remove only the synthetic package,
    // exactly as a user deleting a previously selected .skn would do.
    auto selected = settings::LoadLegacyXml(current);
    selected.lyric.background_color = RGB(91, 17, 29);
    selected.playlist.background_color = RGB(73, 19, 37);
    settings::SaveWindowState(current, selected);
    const auto orphan = runtime / L"Skin/new/common.skn.xml";
    Require(settings::SaveSkinVisualProfile(orphan, selected.player, selected.playlist,
        selected.lyric, selected.visual), "cannot save removed skin fixture profile");
    auto embedded = selected;
    embedded.lyric.background_color = RGB(13, 43, 71);
    embedded.playlist.background_color = RGB(23, 47, 89);
    const auto default_profile = runtime / L"Skin/Default.xml";
    Require(settings::SaveSkinVisualProfile(default_profile, embedded.player, embedded.playlist,
        embedded.lyric, embedded.visual), "cannot save default fixture profile");
    const auto orphan_xml = Read(orphan);
    fs::rename(runtime / L"Skin/new/common.skn", runtime / L"Skin/new/common.skn.removed");
    Require(RunChild(executable, L"--smoke-test", runtime.parent_path()) == 0,
            "removed skin fallback startup failed");
    const auto fallback = settings::LoadLegacyXml(current);
    Require(fallback.skin_file == L"<Default_Skin>" &&
            fallback.lyric.background_color == embedded.lyric.background_color &&
            fallback.playlist.background_color == embedded.playlist.background_color,
            "missing package fallback retained the removed skin's styles");
    Require(Read(orphan) == orphan_xml, "fallback overwrote the removed skin's profile");
    // The first fallback must not poison Default.xml on close. Its next
    // ordinary-default startup should retain exactly the recovered styles.
    Require(RunChild(executable, L"--smoke-test", runtime.parent_path()) == 0,
            "default restart after fallback failed");
    const auto restarted = settings::LoadLegacyXml(current);
    Require(restarted.lyric.background_color == embedded.lyric.background_color &&
            restarted.playlist.background_color == embedded.playlist.background_color,
            "fallback shutdown contaminated the default profile");
    // Deleting the orphan as well must not resurrect old styles from the
    // main XML. This additionally exercises the non-Skin/new resolution.
    selected.skin_file = L"removed-root.skn";
    settings::SaveWindowState(current, selected);
    Require(RunChild(executable, L"--smoke-test", runtime.parent_path()) == 0,
            "fallback without an orphan profile failed");
    const auto no_orphan = settings::LoadLegacyXml(current);
    Require(no_orphan.skin_file == L"<Default_Skin>" &&
            no_orphan.lyric.background_color == embedded.lyric.background_color &&
            no_orphan.playlist.background_color == embedded.playlist.background_color &&
            Read(previous) == changed_old, "fallback without a sidecar retained main XML styles");
    OleUninitialize();
}
void SettingsTests(const fs::path& runtime) {
    const auto directory = runtime / L"settings-fixture";
    fs::create_directory(directory);
    const auto previous = directory / L"TTPlayer.xml";
    const auto current = directory / settings::kSettingsFileName;
    const auto defaults = settings::LoadRuntimeSettings(directory);
    Require(defaults.source_path == current && !fs::exists(current), "missing config used old save path");
    const std::string old_xml = "\xef\xbb\xbf<ttplayer><Player Volume=\"37\"/>"
        "<Skin PackageName=\"new\\common.skn\"/><Unknown Preserve=\"yes\"/></ttplayer>";
    Write(previous, old_xml);
    auto imported = settings::LoadRuntimeSettings(directory);
    Require(imported.source_path == current && imported.player.volume == 37 &&
            imported.skin_file == L"new\\common.skn", "legacy config not imported to new identity");
    Require(Read(current) == old_xml && Read(previous) == old_xml, "migration changed XML content");
    imported.player.volume = 62;
    settings::SaveWindowState(imported.source_path, imported);
    Require(Read(previous) == old_xml, "save overwrote original player's configuration");
    Require(Read(current).find("Preserve=\"yes\"") != std::string::npos, "save lost unknown XML fields");
    Require(settings::LoadRuntimeSettings(directory).player.volume == 62, "new config not preferred");
    Write(current, "<ttplayer>");
    Require(settings::LoadRuntimeSettings(directory).player.volume != 37,
            "malformed new config silently resurrected old settings");
    Require(Read(previous) == old_xml, "old config changed during failed load");
}
void PathTests(const fs::path& runtime) {
    const auto directory = runtime / L"Skin";
    const auto config = runtime / settings::kSettingsFileName;
    Require(skin::ResolveSkinPackagePath(directory, L"new/common.skn") == directory / L"new/common.skn",
            "new package path was flattened");
    Require(ui::ResolveSkinProfilePath(config, L"new\\common.skn") == directory / L"new/common.skn.xml",
            "new profile is not beside the package");
    Require(ui::ResolveSkinProfilePath(config, L"NEW/common") == directory / L"new/common.skn.xml",
            "case/extension normalization failed");
    Require(ui::ResolveSkinProfilePath(config, L"common.skn") == directory / L"common.skn.xml",
            "legacy root skin profile changed");
    Require(ui::ResolveSkinProfilePath(config, L"<Default_Skin>") == directory / L"Default.xml",
            "embedded profile moved into new directory");
    for (const auto selector : {L"../outside.skn", L"new/../../outside.skn", L"D:\\outside.skn"})
        Require(skin::ResolveSkinPackagePath(directory, selector) == directory / L"outside.skn",
                "selector escaped EXE-local Skin directory");
    Require(skin::SkinPackageSelector(directory, directory / L"new/common.skn") == L"new\\common.skn",
            "installed skin drop lost subdirectory identity");
}
} // namespace

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void StartupFallbacks(const fs::path& runtime, HMODULE resources, HMODULE comm) {
        settings::Settings seed;
        seed.source_path = runtime / settings::kSettingsFileName;
        seed.general.fade_windows = seed.general.tray_icon = seed.general.send_title_to_msn = false;
        seed.lyric.auto_download = false;
        seed.player.mute = true; seed.player.volume = 37;
        seed.player.top_most = true; seed.player.mini_top_most = false;
        seed.player.lyric_top_most = true;
        seed.player.player_window = seed.player.mini_player_window =
            seed.player.lyric_window = seed.player.mini_lyric_window =
            seed.player.playlist_window = seed.player.equalizer_window = {21, 43, 777, 888};
        seed.playlist.font = L"Arial"; seed.playlist.font_height = -29;
        seed.playlist.font_descriptor_valid = true;
        seed.playlist.font_descriptor.lfHeight = -29;
        seed.playlist.background_color = seed.playlist.alternate_background_color =
            seed.playlist.text_color = seed.playlist.highlight_color =
            seed.playlist.number_color = seed.playlist.duration_color =
            seed.playlist.selected_color = RGB(91, 17, 29);
        seed.lyric.font.lfHeight = -31; seed.lyric.font_valid = true;
        seed.lyric.background_color = seed.lyric.text_color =
            seed.lyric.highlight_color = RGB(73, 19, 37);
        seed.visual.spectrum_top_color = seed.visual.spectrum_bottom_color =
            seed.visual.spectrum_middle_color = seed.visual.spectrum_peak_color =
            seed.visual.text_color = seed.visual.blur_scope_color = RGB(79, 23, 41);
        seed.visual.font = seed.lyric.font; seed.visual.font_valid = true;
        seed.visual.type = 4; seed.visual.frames_per_second = 37;
        seed.lyric.scroll_mode = 1; seed.playlist.item_tips = false;

        const auto profile = runtime / L"Skin/Default.xml";
        const auto previous_profile = Read(profile); // generated by Run, never a user file
        const auto main_xml = Read(seed.source_path);
        for (const auto selector : {L"missing.skn", L"new\\missing.skn",
                                    L"broken.skn", L"new\\broken.skn"}) {
            seed.skin_file = selector;
            const auto package = skin::ResolveSkinPackagePath(runtime / L"Skin", selector);
            if (std::wstring_view(selector).find(L"broken") != std::wstring_view::npos)
                Write(package, "invalid ZIP data");
            const auto orphan = fs::path(package.wstring() + L".xml");
            Write(orphan, "<ttplayer><Player PlayerWnd=\"999,999,1999,1999\"/>"
                "<Lyric TextColor=\"#ffffff\"/><PlayList CreateNewVerPlayList=\"1\"/></ttplayer>");
            const auto orphan_xml = Read(orphan);
            for (int kind = 0; kind < 4; ++kind) {
                if (kind == 0) fs::remove(profile); // this isolated test's own profile
                if (kind == 1) Write(profile, "<ttplayer>");
                if (kind == 2) Write(profile, "<unrelated/>");
                if (kind == 3) Write(profile,
                    "<ttplayer><Player PlayerWnd=\"60,70,387,211\" PlayListVisible=\"0\"/>"
                    "<Lyric TextColor=\"#123456\" Font=\"invalid\"/>"
                    "<PlayList Color_Select=\"#654321\" Font=\"invalid\"/>"
                    "<Visual TextColor=\"#abcdef\" Type=\"1\" FramesPerSec=\"1\"/></ttplayer>");
                const auto before = fs::exists(profile) ? Read(profile) : std::string{};
                ui::PlayerWindow p(seed);
                p.SetSkinResourceModule(resources); p.SetTtpCommModule(comm);
                Require(p.LoadStartupSkin(resources) && p.skin_ &&
                    p.settings_.skin_file == L"<Default_Skin>" && p.CurrentSkinProfilePath() == profile,
                    "startup did not select the real default package/profile");
                const auto& s = p.settings_;
                const auto& list = p.skin_->Playlist();
                const auto& lyric = p.skin_->Lyric();
                const auto& visual = p.skin_->Visual();
                Require(s.playlist.font == list.font && s.playlist.font_height == list.font_height &&
                    !s.playlist.font_descriptor_valid && s.lyric.font_valid &&
                    std::memcmp(&s.lyric.font, &lyric.font, sizeof(LOGFONTW)) == 0,
                    "fallback retained a deleted skin font");
                Require(s.playlist.background_color == list.background_color &&
                    s.playlist.alternate_background_color == list.alternate_background_color &&
                    s.playlist.text_color == list.text_color && s.playlist.highlight_color == list.highlight_color &&
                    s.playlist.number_color == list.number_color && s.playlist.duration_color == list.duration_color &&
                    s.playlist.selected_color == (kind == 3 ? RGB(0x65,0x43,0x21) : list.selected_color) &&
                    s.lyric.background_color == lyric.background_color &&
                    s.lyric.highlight_color == lyric.highlight_color &&
                    s.lyric.text_color == (kind == 3 ? RGB(0x12,0x34,0x56) : lyric.text_color),
                    "fallback ignored default package colors / sparse profile overrides");
                const settings::VisualSettings base;
                Require(s.visual.spectrum_top_color == visual.spectrum_top_color.value_or(base.spectrum_top_color) &&
                    s.visual.spectrum_bottom_color == visual.spectrum_bottom_color.value_or(base.spectrum_bottom_color) &&
                    s.visual.spectrum_middle_color == visual.spectrum_middle_color.value_or(base.spectrum_middle_color) &&
                    s.visual.spectrum_peak_color == visual.spectrum_peak_color.value_or(base.spectrum_peak_color) &&
                    s.visual.blur_scope_color == visual.blur_scope_color.value_or(base.blur_scope_color) &&
                    s.visual.text_color == (kind == 3 ? RGB(0xab,0xcd,0xef) : visual.text_color.value_or(base.text_color)) &&
                    s.visual.font_valid == visual.font.has_value(), "fallback retained an old visual palette/font");
                const RECT main = kind == 3 ? RECT{60,70,387,211} : RECT{};
                Require(EqualRect(&s.player.player_window, &main) &&
                    IsRectEmpty(&s.player.mini_player_window) && IsRectEmpty(&s.player.lyric_window) &&
                    IsRectEmpty(&s.player.mini_lyric_window) && IsRectEmpty(&s.player.playlist_window) &&
                    IsRectEmpty(&s.player.equalizer_window) && s.player.lyric_visible &&
                    s.player.equalizer_visible && s.player.playlist_visible == (kind != 3),
                    "fallback kept another skin's geometry or ignored default visibility");
                Require(s.player.volume == 37 && s.player.mute && s.player.top_most &&
                    !s.player.mini_top_most && s.player.lyric_top_most && !s.playlist.item_tips &&
                    s.lyric.scroll_mode == 1 && s.visual.type == 4 && s.visual.frames_per_second == 37 &&
                    !s.playlist.legacy_playlist_generation, "fallback changed global options/read an orphan profile");
                Require(Read(orphan) == orphan_xml && Read(seed.source_path) == main_xml &&
                    (kind == 0 ? !fs::exists(profile) : Read(profile) == before),
                    "pre-Create fallback wrote a configuration file");
            }
        }
        // Valid-package startup is deliberately different from fallback: do
        // not reset saved global visual preferences on ordinary restarts.
        for (const auto selector : {L"<Default_Skin>", L"common.skn", L"new\\common.skn"}) {
            seed.skin_file = selector;
            ui::PlayerWindow p(seed);
            p.SetSkinResourceModule(resources); p.SetTtpCommModule(comm);
            Require(p.LoadStartupSkin(resources) && p.settings_.skin_file == selector &&
                p.settings_.visual.text_color == seed.visual.text_color &&
                p.settings_.visual.font.lfHeight == seed.visual.font.lfHeight &&
                p.settings_.visual.type == seed.visual.type, "valid startup was treated as fallback");
        }
        Write(profile, previous_profile);
        std::cout << "16 missing/corrupt package/profile combinations + 3 normal startups passed\n";
    }
    static void CheckMenus(ui::PlayerWindow& p) {
        using namespace ui::detail;
        const HMENU menu = CreatePopupMenu();
        Require(menu != nullptr, "cannot create skin menu fixture");
        AppendMenuW(menu, MF_STRING, kCmdDefaultSkin, L"default");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 1, L"options");
        p.PopulateSkinMenu(menu);
        bool checked{};
        for (const auto& entry : p.skin_commands_) {
            if (entry.package_name == L"new\\common.skn")
                checked = (GetMenuState(menu, entry.command, MF_BYCOMMAND) & MF_CHECKED) != 0;
        }
        DestroyMenu(menu);
        Require(checked, "skin menu did not check the selected new package");
        const HWND pane = CreateWindowExW(0, L"STATIC", L"skin options fixture", WS_POPUP,
            0, 0, 200, 200, p.window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(pane != nullptr, "cannot create options fixture");
        const HWND list = CreateWindowExW(0, L"LISTBOX", nullptr, WS_CHILD,
            0, 0, 100, 100, pane, reinterpret_cast<HMENU>(1064), GetModuleHandleW(nullptr), nullptr);
        if (!list) { DestroyWindow(pane); throw std::runtime_error("cannot create options skin list"); }
        p.PopulateOptionsSkinPage(pane);
        const auto row = SendMessageW(list, LB_GETCURSEL, 0, 0);
        const auto item = row == LB_ERR ? LB_ERR : SendMessageW(list, LB_GETITEMDATA, row, 0);
        const bool selected = item >= 0 && static_cast<size_t>(item) < p.options_skin_entries_.size() &&
            p.options_skin_entries_[static_cast<size_t>(item)].package_name == L"new\\common.skn";
        const bool preview = p.options_skin_preview_ != nullptr;
        DestroyWindow(pane);
        Require(selected && preview, "options skin selection/PNG preview did not resolve new package");
    }
    static void Run(const fs::path& runtime, HMODULE resources, HMODULE comm) {
        using namespace ui::detail;
        settings::Settings s;
        s.source_path = runtime / settings::kSettingsFileName;
        s.general.fade_windows = s.general.tray_icon = s.general.send_title_to_msn = false;
        s.lyric.auto_download = false;
        s.player.mute = true; s.player.volume = 0;
        s.skin_file = L"<Default_Skin>";
        ui::PlayerWindow p(s);
        p.SetSkinResourceModule(resources); p.SetTtpCommModule(comm);
        Require(p.LoadSkinResource(resources) && p.Create(GetModuleHandleW(nullptr), SW_HIDE),
                "cannot create fixture player");
        try {
            p.CompleteSkinWindowFadeForReplacement();
            const auto original = p.window_;
            p.StartSkinMenuCatalogLoad();
            Require(p.PublishReadySkinMenuCatalog(INFINITE), "async catalog not published");
            Require(p.skin_catalog_cache_.size() == 3, "catalog did not scan exactly Skin and Skin/new");
            for (const auto selector : {L"common.skn", L"new\\common.skn"})
                Require(std::count_if(p.skin_catalog_cache_.begin(), p.skin_catalog_cache_.end(),
                    [selector](const auto& e) { return e.package_name == selector; }) == 1,
                    "same-named packages collided in catalog");
            SIZE old_size{};
            for (int round = 0; round < 2; ++round) {
                for (const auto selector : {L"common.skn", L"new\\common.skn"}) {
                    const auto path = skin::ResolveSkinPackagePath(runtime / L"Skin", selector);
                    p.skin_commands_ = {{kCmdFirstSkin, path, selector, {}, false}};
                    Require(p.HandleContextCommand(kCmdFirstSkin), "skin menu command failed");
                    Require(p.settings_.skin_file == selector, "skin switch lost selector");
                    auto sidecar = path; sidecar += L".xml";
                    Require(p.CurrentSkinProfilePath() == sidecar, "active profile path is wrong");
                    p.SaveCurrentSkinProfile();
                    Require(fs::exists(sidecar), "sidecar was not saved beside package");
                    const auto size = p.skin_->WindowSize();
                    if (std::wstring_view(selector) == L"common.skn") old_size = size;
                    else {
                        Require(size.cx != old_size.cx || size.cy != old_size.cy,
                                "same filename/timestamp reused the other skin's cache");
                        p.ApplyOptionsChangeMask(0xffff, -2);
                        Require(p.settings_.skin_file == selector && p.skin_->WindowSize().cx == size.cx,
                                "options application loaded root package instead of new package");
                        CheckMenus(p);
                    }
                    Require(p.window_ == original, "skin path update recreated player HWND");
                }
                const auto old_profile = Read(runtime / L"Skin/common.skn.xml");
                const auto new_profile = Read(runtime / L"Skin/new/common.skn.xml");
                Require(p.LoadSkinResource(resources), "cannot return from new package to embedded skin");
                Require(p.CurrentSkinProfilePath() == runtime / L"Skin/Default.xml" &&
                        !fs::exists(runtime / L"Skin/new/Default.xml"), "default profile followed new directory");
                Require(Read(runtime / L"Skin/common.skn.xml") == old_profile,
                        "new skin save changed same-named root profile");
                Require(!new_profile.empty(), "new skin profile was empty");
            }
            Require(p.LoadSkinPackage(runtime / L"Skin/new/common.skn"), "final new skin load failed");
            p.PersistWindowState();
            const auto restored = settings::LoadRuntimeSettings(runtime);
            Require(restored.skin_file == L"new\\common.skn" &&
                    restored.source_path == runtime / settings::kSettingsFileName,
                    "restart configuration lost subdirectory or new settings filename");
            Require(!fs::exists(runtime / L"TTPlayer.xml"), "fresh runtime wrote legacy main config");
            DestroyWindow(p.window_);
        } catch (...) {
            if (IsWindow(p.window_)) DestroyWindow(p.window_);
            throw;
        }
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2 || argc == 3, "expected repository root + built EXE or --isolated");
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        if (std::wstring_view(argv[1]) != L"--isolated") {
            const fs::path root = argv[1];
            for (const auto path : {L"ttpres.dll", L"ttpcomm.dll", L"Skin/LX-iPlay.skn",
                                    L"Skin/DEFAULT_SKIN__6120.skn"})
                if (!fs::exists(root / path)) { std::cout << "native fixtures unavailable\n"; return 77; }
            const auto runtime = fs::temp_directory_path() / (L"TTPlayerRuntimePaths-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            fs::create_directories(runtime / L"Skin/new");
            fs::create_directories(runtime / L"Skin/ignored");
            fs::create_directories(runtime / L"PlayList");
            fs::copy_file(executable, runtime / L"runtime_paths_tests.exe");
            for (const auto dll : {L"ttpres.dll", L"ttpcomm.dll"}) fs::copy_file(root / dll, runtime / dll);
            fs::copy_file(root / L"Skin/LX-iPlay.skn", runtime / L"Skin/common.skn");
            fs::copy_file(root / L"Skin/LX-iPlay.skn", runtime / L"Skin/ignored/ignored.skn");
            fs::copy_file(root / L"Skin/DEFAULT_SKIN__6120.skn", runtime / L"Skin/new/common.skn");
            fs::last_write_time(runtime / L"Skin/new/common.skn", fs::last_write_time(runtime / L"Skin/common.skn"));
            const DWORD code = RunChild(runtime / L"runtime_paths_tests.exe", L"--isolated", runtime);
            std::wcout << L"runtime path artifacts: " << runtime.wstring() << L'\n';
            if (code == 0 && argc == 3) StartupTests(runtime, argv[2]);
            return static_cast<int>(code);
        }
        Require(SUCCEEDED(OleInitialize(nullptr)), "OLE init failed");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        const auto runtime = fs::path(executable).parent_path();
        SettingsTests(runtime); PathTests(runtime);
        HMODULE resources = LoadLibraryExW((runtime / L"ttpres.dll").c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        HMODULE comm = LoadLibraryW((runtime / L"ttpcomm.dll").c_str());
        Require(resources && comm, "cannot load native DLL fixtures");
        testing::SkinRebindAccess::Run(runtime, resources, comm);
        testing::SkinRebindAccess::StartupFallbacks(runtime, resources, comm);
        FreeLibrary(comm); FreeLibrary(resources); OleUninitialize();
        std::cout << "runtime settings/skin paths tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime_paths_tests: " << error.what() << '\n'; return 1;
    }
}
