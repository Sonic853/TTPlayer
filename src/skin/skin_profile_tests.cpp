#include "ttplayer/ui/player_window.h"
#include "ttplayer/skin/skin_package.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Write(const fs::path& path, const char* text) {
    std::ofstream file(path, std::ios::binary);
    Require(static_cast<bool>(file << text), "cannot write test profile");
}
void SparseProfileTest(const fs::path& runtime) {
    settings::Settings s;
    s.player.player_window = {17, 23, 344, 164};
    s.player.mini_lyric_window = {30, 40, 50, 60};
    s.player.lyric_visible = true;
    s.playlist.background_color = RGB(11, 22, 33);
    s.playlist.font = L"Tahoma";
    s.playlist.font_height = -17;
    s.playlist.item_tips = false;
    s.lyric.background_color = RGB(44, 55, 66);
    s.lyric.scroll_mode = 1;
    s.visual.type = 4;
    s.visual.frames_per_second = 37;
    const auto profile = runtime / L"sparse.xml";
    Write(profile, "<ttplayer><Player LyricWnd2=\"0,0,0,0\"/>"
        "<PlayList Color_Text=\"#123456\" Font=\"invalid\"/>"
        "<Lyric TextColor=\"#654321\"/><Visual Type=\"1\" FramesPerSec=\"1\" "
        "TextColor=\"#abcdef\"/></ttplayer>");
    Require(settings::LoadSkinVisualProfile(profile, s.player, s.playlist, s.lyric, s.visual),
            "sparse profile rejected");
    Require(s.playlist.background_color == RGB(11, 22, 33) &&
            s.playlist.font == L"Tahoma" && s.playlist.font_height == -17,
            "absent playlist fields replaced the skin baseline");
    Require(s.playlist.text_color == RGB(0x12, 0x34, 0x56) &&
            s.lyric.text_color == RGB(0x65, 0x43, 0x21) &&
            s.lyric.background_color == RGB(44, 55, 66), "sparse colors not merged");
    const RECT original{17, 23, 344, 164};
    Require(EqualRect(&s.player.player_window, &original) &&
            IsRectEmpty(&s.player.mini_lyric_window) && s.player.lyric_visible,
            "absent geometry/visibility differs from explicit zero geometry");
    Require(!s.playlist.item_tips && s.lyric.scroll_mode == 1 &&
            s.visual.type == 4 && s.visual.frames_per_second == 37 &&
            s.visual.text_color == RGB(0xab, 0xcd, 0xef), "profile changed global behavior");
    Write(profile, "<ttplayer>");
    Require(!settings::LoadSkinVisualProfile(profile, s.player, s.playlist, s.lyric, s.visual) &&
            s.playlist.text_color == RGB(0x12, 0x34, 0x56), "invalid profile changed state");
    Write(profile, "<unrelated/>");
    Require(!settings::LoadSkinVisualProfile(profile, s.player, s.playlist, s.lyric, s.visual),
            "unrelated XML accepted as a skin profile");
    std::cout << "sparse profile overlay passed\n";
}
struct Canvas {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap{};
    HGDIOBJ old{};
    DWORD* pixels{};
    int width{}, height{};
    explicit Canvas(HWND window) {
        RECT rect{}; GetClientRect(window, &rect);
        width = rect.right; height = rect.bottom;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
            reinterpret_cast<void**>(&pixels), nullptr, 0);
        Require(dc && bitmap, "cannot create paint buffer");
        old = SelectObject(dc, bitmap);
    }
    ~Canvas() { SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); }
    std::vector<DWORD> Snapshot() const {
        GdiFlush();
        std::vector<DWORD> result(pixels, pixels + width * height);
        for (auto& pixel : result) pixel &= 0xffffff;
        return result;
    }
};
} // namespace

namespace ttplayer::testing {
struct SkinRebindAccess {
    static std::vector<DWORD> Paint(ui::PlayerWindow& p, int surface) {
        const HWND window = surface == 0 ? p.playlist_window_ :
                            surface == 1 ? p.lyric_window_ : p.lyric_control_;
        Canvas canvas(window);
        if (surface == 0) p.PaintPlaylist(canvas.dc);
        else if (surface == 1) p.PaintLyricWindow(canvas.dc);
        else p.PaintLyricControl(window, canvas.dc);
        return canvas.Snapshot();
    }
    static void Run(const fs::path& runtime, HMODULE resources, HMODULE comm) {
        const auto external = runtime / L"Skin/DEFAULT_SKIN_579.skn";
        const auto sidecar = fs::path(external.wstring() + L".xml");
        const auto embedded = skin::SkinPackage::OpenResource(resources, L"<Default_Skin>");
        const auto archive = skin::SkinPackage::Open(external);
        Require(embedded.ByteSize() == archive.ByteSize() &&
                embedded.Fingerprint() == archive.Fingerprint(), "fixture ZIP differs from DLL");
        Require(embedded.Entries().size() == archive.Entries().size(), "ZIP entries differ");
        for (const auto& entry : embedded.Entries())
            Require(embedded.ReadEntry(entry.name, comm) == archive.ReadEntry(entry.name, comm),
                    "extracted package bytes differ");
        std::cout << "identical ZIP entries=" << embedded.Entries().size() << '\n';
        settings::Settings s;
        s.source_path = runtime / L"TTPlayer.xml";
        s.skin_file = L"<Default_Skin>";
        s.general.fade_windows = s.general.tray_icon = s.general.send_title_to_msn = false;
        s.lyric.auto_download = false;
        s.player.mute = true;
        s.playlist.read_info_mode = 2;
        s.playlist.background_color = RGB(17, 31, 47);
        s.lyric.background_color = RGB(53, 67, 79);
        s.lyric.text_color = RGB(100, 110, 120);
        s.visual.type = 0;
        s.visual.text_color = RGB(181, 191, 201);
        ui::PlayerWindow p(s);
        p.SetSkinResourceModule(resources); p.SetTtpCommModule(comm);
        Require(p.LoadSkinResource(resources), "embedded skin failed");
        Require(p.settings_.playlist.background_color == s.playlist.background_color &&
                p.settings_.visual.text_color == s.visual.text_color,
                "startup replaced saved globals with package defaults");
        Require(p.Create(GetModuleHandleW(nullptr), SW_HIDE), "cannot create test player");
        try {
            p.CompleteSkinWindowFadeForReplacement();
            p.SaveCurrentSkinProfile();
            fs::copy_file(runtime / L"Skin/Default.xml", sidecar);
            const auto saved = p.settings_;
            std::vector<std::vector<DWORD>> reference;
            for (int surface = 0; surface < 3; ++surface) reference.push_back(Paint(p, surface));
            const HWND main = p.window_, list = p.playlist_window_, lyric = p.lyric_window_;
            Require(p.LoadSkinPackage(external), "external skin failed");
            Require(p.settings_.playlist.background_color == saved.playlist.background_color &&
                    p.settings_.lyric.background_color == saved.lyric.background_color &&
                    p.settings_.visual.text_color == saved.visual.text_color,
                    "copied profile failed to retain custom colors");
            for (int surface = 0; surface < 3; ++surface)
                Require(Paint(p, surface) == reference[surface],
                        "DLL/external painted pixels differ with identical profiles");
            Require(p.LoadSkinResource(resources), "default round trip failed");
            for (int surface = 0; surface < 3; ++surface)
                Require(Paint(p, surface) == reference[surface], "round trip changed painted pixels");
            std::cout << "identical-profile playlist/lyric chrome/lyric text pixels match\n";

            // Only remove our generated temporary sidecar, never user profiles.
            Require(fs::remove(sidecar), "cannot clear temporary profile");
            Require(p.LoadSkinPackage(external), "skin without sidecar failed");
            const auto package_defaults = p.settings_;
            Require(package_defaults.playlist.background_color == p.skin_->Playlist().background_color &&
                    package_defaults.lyric.background_color == p.skin_->Lyric().background_color,
                    "missing profile did not use package palette");
            Require(p.LoadSkinResource(resources), "default restore after missing profile failed");
            for (const char* xml : {"<ttplayer/>", "<ttplayer>"}) {
                Write(sidecar, xml);
                Require(p.LoadSkinPackage(external), "empty/invalid profile blocked skin loading");
                Require(p.settings_.playlist.background_color == package_defaults.playlist.background_color &&
                        p.settings_.lyric.background_color == package_defaults.lyric.background_color &&
                        p.settings_.visual.text_color == package_defaults.visual.text_color,
                        "empty/invalid profile did not use package baseline");
                Require(p.LoadSkinResource(resources), "default restore after invalid profile failed");
            }

            Write(sidecar, "<ttplayer><Lyric TextColor=\"#123456\"/>"
                "<PlayList Color_Text=\"#654321\"/></ttplayer>");
            Require(p.LoadSkinPackage(external), "sparse target profile failed");
            Require(p.settings_.lyric.background_color == p.skin_->Lyric().background_color &&
                    p.settings_.playlist.background_color == p.skin_->Playlist().background_color,
                    "target profile omitted colors but package defaults were not applied first");
            Require(p.settings_.lyric.text_color == RGB(0x12, 0x34, 0x56) &&
                    p.settings_.playlist.text_color == RGB(0x65, 0x43, 0x21), "overrides were lost");
            Require(p.window_ == main && p.playlist_window_ == list && p.lyric_window_ == lyric,
                    "profile switch recreated a window");
            Require(p.LoadSkinResource(resources), "default customization did not reload");
            Require(p.settings_.lyric.background_color == saved.lyric.background_color &&
                    p.settings_.playlist.background_color == saved.playlist.background_color,
                    "external customization changed default profile");
            std::cout << "package baseline, sparse overrides and independent profiles passed\n";
            DestroyWindow(p.window_);
        } catch (...) {
            if (p.window_ && IsWindow(p.window_)) DestroyWindow(p.window_);
            throw;
        }
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2, "expected repository root or --isolated");
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        if (std::wstring_view(argv[1]) != L"--isolated") {
            const fs::path root = argv[1];
            if (!fs::exists(root / L"Skin/DEFAULT_SKIN_579.skn") ||
                !fs::exists(root / L"ttpres.dll") || !fs::exists(root / L"ttpcomm.dll")) {
                std::cout << "5.7.9 local fixtures unavailable\n"; return 77;
            }
            const auto runtime = fs::temp_directory_path() / (L"TTPlayerSkinProfile-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            fs::create_directories(runtime / L"Skin");
            fs::create_directories(runtime / L"PlayList");
            fs::copy_file(executable, runtime / L"skin_profile_tests.exe");
            for (const auto file : {L"ttpres.dll", L"ttpcomm.dll", L"Skin/DEFAULT_SKIN_579.skn"})
                fs::copy_file(root / file, runtime / file);
            std::wstring command = L"\"" + (runtime / L"skin_profile_tests.exe").wstring() + L"\" --isolated";
            STARTUPINFOW startup{sizeof(startup)};
            startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION child{};
            Require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                nullptr, runtime.c_str(), &startup, &child) != FALSE, "cannot start isolated test");
            CloseHandle(child.hThread);
            const DWORD waited = WaitForSingleObject(child.hProcess, 45000);
            DWORD status = 1;
            if (waited == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &status);
            else TerminateProcess(child.hProcess, 1); // only our timed-out fixture process
            CloseHandle(child.hProcess);
            std::wcout << L"isolated test artifacts: " << runtime.wstring() << L'\n';
            return static_cast<int>(status);
        }
        Require(SUCCEEDED(OleInitialize(nullptr)), "OLE init failed");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        const auto runtime = fs::path(executable).parent_path();
        SparseProfileTest(runtime);
        HMODULE resources = LoadLibraryExW((runtime / L"ttpres.dll").c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        HMODULE comm = LoadLibraryW((runtime / L"ttpcomm.dll").c_str());
        Require(resources && comm, "fixture DLL unavailable");
        testing::SkinRebindAccess::Run(runtime, resources, comm);
        FreeLibrary(comm); FreeLibrary(resources); OleUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "skin profile tests: " << error.what() << '\n'; return 1;
    }
}
