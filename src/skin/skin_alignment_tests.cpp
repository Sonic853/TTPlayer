#include "ttplayer/skin/skin.h"
#include "ttplayer/skin/skin_package.h"
#include "ttplayer/ui/player_window.h"
#include "../ui/player_window_internal.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
using namespace ttplayer::ui::detail;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void SameRect(const RECT& actual, RECT expected) {
    Require(EqualRect(&actual, &expected) != FALSE, "skin anchor rectangle mismatch");
}
void GeometryTests() {
    // FUN_0047A9C5: center ignores the XML origin, right/bottom preserve the
    // XML far-edge inset, undersized clients disable only the affected axis.
    const RECT xml{7, 8, 62, 21};
    const SIZE native{268, 90};
    SameRect(ResolveAlignedRect(xml, 2, native, 268, 90), {106, 8, 161, 21});
    SameRect(ResolveAlignedRect(xml, 2, native, 269, 90), {107, 8, 162, 21});
    SameRect(ResolveAlignedRect(xml, 2, native, 400, 165), {172, 8, 227, 21});
    SameRect(ResolveAlignedRect(xml, 0x22, native, 400, 165), {172, 76, 227, 89});
    SameRect(ResolveAlignedRect(xml, 0x22, native, 200, 165), {7, 76, 62, 89});
    SameRect(ResolveAlignedRect(xml, 0x22, native, 400, 60), {172, 8, 227, 21});
    SameRect(ResolveAlignedRect(xml, 0x22, native, 200, 60), xml);
    SameRect(ResolveAlignedRect(xml, 0x11, native, 400, 165), xml);
    SameRect(ResolveAlignedRect(xml, 0x33, native, 400, 165), {139, 83, 194, 96});
    SameRect(ResolveAlignedRect({245, 6, 260, 21}, 3, native, 400, 165),
             {377, 6, 392, 21});
    // Image dimensions, not a padded XML box, determine title centering and
    // drawing size. The original XML right/bottom still anchor those modes.
    const RECT padded{9, 4, 109, 34};
    SameRect(ResolveAlignedRect(padded, 0x22, native, 268, 90, {55, 13}),
             {106, 38, 161, 51});
    SameRect(ResolveAlignedRect(padded, 0x33, native, 400, 165, {55, 13}),
             {186, 96, 241, 109});
}

struct Canvas {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap{};
    HGDIOBJ old{};
    DWORD* pixels{};
    int width, height;
    Canvas(int w, int h) : width(w), height(h) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
            reinterpret_cast<void**>(&pixels), nullptr, 0);
        Require(dc && bitmap, "canvas allocation failed");
        old = SelectObject(dc, bitmap);
    }
    ~Canvas() { SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); }
    void Save(const fs::path& path) const {
        GdiFlush();
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + width * height * 4;
        info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
        info.biPlanes = 1; info.biBitCount = 32;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        out.write(reinterpret_cast<const char*>(pixels), width * height * 4);
    }
};
struct Module {
    HMODULE value;
    ~Module() { if (value) FreeLibrary(value); }
};
struct TestWindow {
    HWND value = CreateWindowExW(0, L"STATIC", L"Skin alignment test", WS_POPUP,
        0, 0, 268, 165, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~TestWindow() { if (value) DestroyWindow(value); }
};
} // namespace

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void ClassicTests(const fs::path& directory, const fs::path& output) {
        auto layout = skin::LegacySkin::Load(directory);
        Require(layout.Valid() && layout.Playlist().valid && layout.Lyric().valid,
                "Classic skin did not parse");
        for (const auto* title : {&layout.Playlist().title, &layout.Lyric().title}) {
            SameRect(title->bounds, {0, 8, 55, 21});
            Require(title->alignment == 2 && title->image &&
                    title->image_size.cx == 55 && title->image_size.cy == 13,
                    "Classic title image/alignment parse mismatch");
        }
        settings::Settings settings;
        settings.general.send_title_to_msn = false;
        settings.general.tray_icon = false;
        settings.general.fade_windows = false;
        settings.player.mute = true;
        settings.lyric.auto_download = false;
        ui::PlayerWindow player(settings);
        player.skin_.emplace(std::move(layout));
        player.playlists_.NewList(L"Title test");
        TestWindow playlist, lyric;
        Require(playlist.value && lyric.value, "test HWND allocation failed");
        player.playlist_window_ = playlist.value;
        player.lyric_window_ = lyric.value;
        try {
            for (const int width : {268, 269, 400, 401, 268}) {
                SetWindowPos(playlist.value, nullptr, 0, 0, width, 165, SWP_NOACTIVATE);
                SetWindowPos(lyric.value, nullptr, 0, 0, width, 165, SWP_NOACTIVATE);
                const auto& pl = player.skin_->Playlist();
                const auto& lr = player.skin_->Lyric();
                const RECT expected{(width - 55) / 2, 8, (width - 55) / 2 + 55, 21};
                const auto geometry = MakePlaylistGeometry(pl, 60, width, 165, 0);
                SameRect(geometry.title, expected);
                SameRect(player.LyricElementBounds(lr.title), expected);
                SameRect(geometry.close, {width - 23, 6, width - 8, 21});
                SameRect(player.LyricElementBounds(lr.close), geometry.close);
                for (const bool is_playlist : {true, false}) {
                    Canvas actual(width, 165), reference(width, 165);
                    const auto& background = is_playlist ? pl.background : lr.background;
                    const auto& title = is_playlist ? pl.title : lr.title;
                    DrawResizableSkinBitmap(reference.dc, background,
                        is_playlist ? pl.resize_rect : lr.resize_rect, width, 165,
                        is_playlist ? pl.resize_tile : lr.resize_tile);
                    // The original title painter (0047EC2F / 004493DC) copies
                    // the native image at the 0042912E -> 0047A9C5 origin.
                    Require(title.image.Draw(reference.dc, expected.left, expected.top,
                        55, 13, 0, 0, 55, 13, player.skin_->TransparentColor()),
                        "reference title drawing failed");
                    if (is_playlist) player.PaintPlaylist(actual.dc);
                    else player.PaintLyricWindow(actual.dc);
                    GdiFlush();
                    // Caption band excluding the separately drawn close
                    // control: catches both a missing center and stale left title.
                    for (int y = 0; y < 23; ++y)
                        for (int x = 0; x < width - 28; ++x)
                            Require((actual.pixels[y * width + x] & 0xffffff) ==
                                    (reference.pixels[y * width + x] & 0xffffff),
                                    "Classic caption pixels shifted/stretched");
                    actual.Save(output / ((is_playlist ? L"playlist-" : L"lyric-") +
                        std::to_wstring(width) + L".bmp"));
                }
            }
        } catch (...) {
            player.playlist_window_ = player.lyric_window_ = nullptr;
            throw;
        }
        player.playlist_window_ = player.lyric_window_ = nullptr;
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    try {
        GeometryTests();
        const fs::path root = argc > 1 ? argv[1] : L".";
        const auto package_path = root / L"Skin/Classic.skn";
        if (fs::exists(package_path) && fs::exists(root / L"ttpcomm.dll")) {
            const auto output = fs::temp_directory_path() / (L"TTPlayer-Alignment-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            Module comm{LoadLibraryW(fs::absolute(root / L"ttpcomm.dll").c_str())};
            Require(comm.value != nullptr, "test decompressor missing");
            skin::SkinPackage::Open(package_path).ExtractTo(output / L"Classic", comm.value);
            testing::SkinRebindAccess::ClassicTests(output / L"Classic", output);
            std::wcout << L"PASS Classic parsing, live window geometry and caption pixels: "
                       << output << L'\n';
        } else std::cout << "SKIP optional Classic corpus; pure alignment tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
