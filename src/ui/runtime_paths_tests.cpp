#include "ttplayer/ui/player_window.h"
#include "ttplayer/ui/player_runtime_policy.h"
#include "player_window_internal.h"

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
        FreeLibrary(comm); FreeLibrary(resources); OleUninitialize();
        std::cout << "runtime settings/skin paths tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime_paths_tests: " << error.what() << '\n'; return 1;
    }
}
