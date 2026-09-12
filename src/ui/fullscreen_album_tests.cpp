#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "album_background.h"
#include "../app/resource_ids.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <olectl.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace fs = std::filesystem;
using namespace ttplayer;
using namespace ttplayer::ui::detail;
using Microsoft::WRL::ComPtr;

namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Hr(HRESULT status) { Require(SUCCEEDED(status), "image fixture COM operation failed"); }
struct Temp {
    fs::path path = fs::temp_directory_path() / (L"TTPlayer-album-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Temp() { Require(fs::create_directory(path), "cannot create isolated fixture directory"); }
    ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
struct Window {
    HWND value{};
    ~Window() { if (value && IsWindow(value)) DestroyWindow(value); }
};
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
    void Unselect() { if (old) { SelectObject(dc, old); old = nullptr; } }
    ~Canvas() { Unselect(); DeleteObject(bitmap); DeleteDC(dc); }
};
void SavePng(const fs::path& path, UINT width, UINT height, const DWORD* bgra) {
    ComPtr<IWICImagingFactory> factory;
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream; Hr(factory->CreateStream(&stream));
    Hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    Hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    Hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    Hr(encoder->CreateNewFrame(&frame, nullptr)); Hr(frame->Initialize(nullptr));
    Hr(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Hr(frame->SetPixelFormat(&format));
    Require(format == GUID_WICPixelFormat32bppBGRA, "PNG fixture pixel format changed");
    Hr(frame->WritePixels(height, width * 4, width * height * 4,
                         reinterpret_cast<BYTE*>(const_cast<DWORD*>(bgra))));
    Hr(frame->Commit()); Hr(encoder->Commit());
}
void Same(RECT actual, RECT expected) {
    Require(EqualRect(&actual, &expected) != FALSE, "cover crop mismatch");
}
void Pixel(HDC dc, int x, int y, COLORREF expected, int tolerance = 0) {
    const auto got = GetPixel(dc, x, y);
    if (std::abs(int(GetRValue(got)) - GetRValue(expected)) > tolerance ||
        std::abs(int(GetGValue(got)) - GetGValue(expected)) > tolerance ||
        std::abs(int(GetBValue(got)) - GetBValue(expected)) > tolerance) {
        std::cerr << "pixel " << x << ',' << y << ": " << std::hex << got <<
                     " expected " << expected << std::dec << '\n';
        throw std::runtime_error("background compositing mismatch");
    }
}
void GeometryAndAlpha() {
    Same(AlbumCoverSourceRect({400,200}, {100,100}), {100,0,300,200});
    Same(AlbumCoverSourceRect({200,400}, {100,100}), {0,100,200,300});
    Same(AlbumCoverSourceRect({200,200}, {1600,900}), {0,44,200,156});
    Same(AlbumCoverSourceRect({1,1}, {400,100}), {0,0,1,1});
    Same(AlbumCoverSourceRect({0,200}, {100,100}), {});
    Canvas source(4,2), target(16,16);
    // Only the green middle half survives a square cover crop.
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x) source.pixels[y*4+x] = x == 1 || x == 2 ? 0xff00ff00 : 0xffff0000;
    source.Unselect();
    const RECT bounds{0,0,16,16};
    PaintAlbumBackground(target.dc, bounds, RGB(0,0,255), 0, source.bitmap, {4,2}, nullptr);
    for (const POINT p : {POINT{0,0}, POINT{15,15}, POINT{7,7}}) Pixel(target.dc, p.x, p.y, RGB(0,255,0));
    PaintAlbumBackground(target.dc, bounds, RGB(0,0,255), 50, source.bitmap, {4,2}, nullptr);
    Pixel(target.dc, 7,7, RGB(0,128,127), 1);
    PaintAlbumBackground(target.dc, bounds, RGB(17,34,51), 100, source.bitmap, {4,2}, nullptr);
    Pixel(target.dc, 7,7, RGB(17,34,51));
    PaintAlbumBackground(target.dc, bounds, RGB(17,34,51), 0, nullptr, {}, nullptr);
    Pixel(target.dc, 7,7, RGB(17,34,51));
    // WIC PBGRA: half-transparent red, combined with half global opacity.
    std::fill_n(source.pixels, 8, 0x80800000);
    PaintAlbumBackground(target.dc, bounds, RGB(0,0,255), 50, source.bitmap, {4,2}, nullptr);
    Pixel(target.dc, 7,7, RGB(64,0,191), 2);
    std::fill_n(source.pixels, 8, 0xffff0000);
    PICTDESC description{};
    description.cbSizeofstruct = sizeof(description);
    description.picType = PICTYPE_BITMAP;
    description.bmp.hbitmap = source.bitmap;
    ComPtr<IPicture> ole;
    Hr(OleCreatePictureIndirect(&description, IID_IPicture, FALSE,
                               reinterpret_cast<void**>(ole.GetAddressOf())));
    PaintAlbumBackground(target.dc, bounds, RGB(0,0,255), 50, nullptr, {}, ole.Get());
    Pixel(target.dc,7,7,RGB(128,0,127),1);
    std::cout << "cover geometry, enlargement, source alpha and global transparency passed\n";
}
void SettingsRoundtrip(const fs::path& directory) {
    settings::Settings values;
    values.fullscreen.visual_type = 4;
    values.fullscreen.position_relation = {1,0,1,0,1};
    values.fullscreen.lyric_size = {1,2,3,4,8};
    values.fullscreen.album_fallback_image = (directory / L"备用 & 图片.png").wstring();
    values.fullscreen.album_transparency_percent = 37;
    values.lyric.fullscreen_background_color = RGB(12,34,56);
    const auto xml = directory / L"roundtrip.xml";
    settings::SaveWindowState(xml, values);
    auto loaded = settings::LoadLegacyXml(xml);
    Require(loaded.fullscreen.visual_type == 4 &&
        loaded.fullscreen.position_relation == values.fullscreen.position_relation &&
        loaded.fullscreen.lyric_size == values.fullscreen.lyric_size &&
        loaded.fullscreen.album_fallback_image == values.fullscreen.album_fallback_image &&
        loaded.fullscreen.album_transparency_percent == 37 &&
        loaded.lyric.fullscreen_background_color == values.lyric.fullscreen_background_color,
        "fullscreen album settings did not round-trip");
    { std::ofstream file(xml); file << "<ttplayer><FullScreen VisualType=\"3\" PosRelationSpectrum=\"1\" LrcSizeSpectrum=\"7\"/></ttplayer>"; }
    loaded = settings::LoadLegacyXml(xml);
    Require(loaded.fullscreen.visual_type == 3 && loaded.fullscreen.lyric_size[2] == 7 &&
        loaded.fullscreen.album_fallback_image.empty() && loaded.fullscreen.album_transparency_percent == 0 &&
        loaded.fullscreen.position_relation[4] == 1 && loaded.fullscreen.lyric_size[4] == 2,
        "older settings did not retain backward-compatible defaults");
    { std::ofstream file(xml); file << "<ttplayer><FullScreen VisualType=\"99\" AlbumTransparency=\"300\"/></ttplayer>"; }
    loaded = settings::LoadLegacyXml(xml);
    Require(loaded.fullscreen.visual_type == 4 && loaded.fullscreen.album_transparency_percent == 100,
            "invalid album settings not bounded");
    std::cout << "XML roundtrip, Unicode paths, shared colour and old-config defaults passed\n";
}
} // namespace

namespace ttplayer::testing {
// Existing test seam: publish a decoder snapshot without starting an output
// device or playing the user's music. Production Metadata()/State() are used.
struct ProgressSeekAccess {
    static void Publish(audio::AudioEngine& engine, audio::AudioMetadata metadata,
                        audio::PlaybackState state) {
        std::scoped_lock lock(engine.mutex_);
        engine.metadata_ = std::move(metadata);
        engine.state_.store(state);
    }
};
struct SkinRebindAccess {
    static inline std::wstring menu_album_label;
    static inline bool album_checked{};
    static inline bool capture_lyric_menu{}, lyric_menu_seen{}, lyric_separator{}, lyric_commands{}, monitor_menu{};
    static inline std::array<UINT,4> lyric_ids{}, lyric_states{};
    static inline std::array<std::wstring,4> lyric_labels;
    static LRESULT CALLBACK Owner(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_INITMENUPOPUP) {
            // Exercise the production popup initializer too: recognizing the
            // new first command must not check a hidden effect in lyrics-only mode.
            if (auto* p = reinterpret_cast<ui::PlayerWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA)))
                p->HandleMessage(message,wp,lp);
            const HMENU menu = reinterpret_cast<HMENU>(wp);
            wchar_t text[128]{};
            if (GetMenuStringW(menu, kCmdVisualCover, text, 128, MF_BYCOMMAND)) {
                menu_album_label = text;
                album_checked = (GetMenuState(menu, kCmdVisualCover, MF_BYCOMMAND) & MF_CHECKED) != 0;
            }
            if (capture_lyric_menu && GetMenuItemID(menu,0) == kCmdVisualDream) {
                lyric_menu_seen = true;
                for (UINT i = 0; i < 4; ++i) {
                    lyric_ids[i] = GetMenuItemID(menu,i);
                    lyric_states[i] = GetMenuState(menu,i,MF_BYPOSITION);
                    wchar_t label[128]{};
                    GetMenuStringW(menu,i,label,128,MF_BYPOSITION); lyric_labels[i] = label;
                }
                lyric_separator = (GetMenuState(menu,4,MF_BYPOSITION) & MF_SEPARATOR) != 0;
                lyric_commands = FindCommandMenu(menu,kCmdLyricCopy) && FindCommandMenu(menu,kCmdLyricScrollMode);
                monitor_menu = FindCommandMenu(menu,kCmdFullscreenMonitorFirst) != nullptr;
            }
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    static void Run(HMODULE resources, const fs::path& directory, const fs::path& screenshot) {
        settings::Settings settings;
        settings.general.tray_icon = false; settings.general.send_title_to_msn = false;
        settings.general.fade_windows = false; settings.lyric.auto_download = false;
        settings.source_path = directory / L"test-only.xml";
        ui::PlayerWindow p(settings);
        p.SetSkinResourceModule(resources); p.instance_ = GetModuleHandleW(nullptr);
        // Exercise the real command policy with no HWNDs: no audio, no main
        // window teardown, no writes to installed player settings.
        p.fullscreen_mode_ = 3; p.SetVisualType(4);
        Require(p.settings_.visual.type == 4, "combined mode rejects album background");
        p.HandleMessage(WM_COMMAND,kVisualControlId,0);
        Require(p.settings_.visual.type == 1, "combined album click does not cycle to dream");
        p.SetVisualType(3); p.HandleMessage(WM_COMMAND,kVisualControlId,0);
        Require(p.settings_.visual.type == 4, "combined scope click skips album background");
        p.fullscreen_mode_ = 2; p.SetVisualType(4);
        Require(p.settings_.visual.type == 1, "visual-only fullscreen changed its legacy cycle");
        p.SetVisualType(3); p.HandleMessage(WM_COMMAND,kVisualControlId,0);
        Require(p.settings_.visual.type == 1, "visual-only scope click no longer cycles to dream");
        p.fullscreen_mode_ = 0; p.SetVisualType(4);
        Require(p.settings_.visual.type == 4, "embedded album mode changed");
        for (int from : {1,3}) for (int type = 1; type <= 4; ++type)
            for (auto state : {audio::PlaybackState::playing,audio::PlaybackState::paused}) {
                p.fullscreen_mode_ = from;
                ProgressSeekAccess::Publish(p.audio_,{},state);
                const auto monitor = p.fullscreen_monitor_device_;
                p.fullscreen_saved_visual_type_ = 2;
                p.fullscreen_saved_lyric_transparent_ = true;
                p.settings_.lyric.fullscreen_drag_lyric = false;
                const auto before = p.audio_.Position();
                p.HandleMessage(WM_COMMAND,kCmdVisualFirst+type,0);
                Require(p.fullscreen_mode_ == 3 && p.settings_.visual.type == type,
                        "lyric fullscreen effect command did not display the requested effect");
                Require(p.fullscreen_monitor_device_ == monitor && p.fullscreen_saved_visual_type_ == 2 &&
                        p.fullscreen_saved_lyric_transparent_ && !p.settings_.lyric.fullscreen_drag_lyric,
                        "lyric fullscreen effect command lost monitor/restore/drag state");
                Require(p.audio_.Position() == before && p.audio_.State() == state,
                        "effect selection changed audio playback position/state");
            }
        ProgressSeekAccess::Publish(p.audio_,{},audio::PlaybackState::stopped);
        p.settings_.lyric.fullscreen_drag_lyric = true;
        WNDCLASSEXW cls{sizeof(cls)};
        cls.hInstance = p.instance_; cls.lpfnWndProc = Owner;
        cls.lpszClassName = L"TTPlayerAlbumTest";
        Require(RegisterClassExW(&cls) != 0, "cannot register test owner");
        Window owner{CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"Album test",
            WS_POPUP, 0,0,128,80,nullptr,nullptr,p.instance_,nullptr)};
        Window visual{CreateWindowExW(0, WC_STATICW, L"", WS_CHILD,
            0,0,128,80,owner.value,nullptr,p.instance_,nullptr)};
        Require(owner.value && visual.value, "test windows unavailable");
        p.window_ = owner.value; p.visual_window_ = visual.value;
        SetWindowLongPtrW(owner.value,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&p));
        try {
            p.fullscreen_mode_ = 3; p.fullscreen_visual_detached_ = true;
            p.settings_.lyric.fullscreen_background_color = RGB(0,0,255);
            p.settings_.visual.type = 4;
            Canvas canvas(128,80);
            const auto render = [&] { p.UpdateVisualWindowLayout(); p.PaintVisualControl(canvas.dc); };
            render(); Pixel(canvas.dc, 64,40, RGB(0,0,255));
            const auto fallback = directory / L"备用 & 图片.png";
            const DWORD green[4]{0xff00ff00,0xff00ff00,0xff00ff00,0xff00ff00};
            SavePng(fallback,2,2,green);
            p.settings_.fullscreen.album_fallback_image = fallback.wstring();
            render(); Pixel(canvas.dc,0,0,RGB(0,255,0)); Pixel(canvas.dc,127,79,RGB(0,255,0));
            Require(!p.VisualFallbackHit({64,40}), "background exposes normal missing-cover prompt");
            const auto album = directory / L"embedded.png";
            const DWORD red[4]{0xffff0000,0xffff0000,0xffff0000,0xffff0000};
            SavePng(album,2,2,red);
            std::ifstream input(album,std::ios::binary);
            audio::AudioMetadata metadata;
            metadata.thumbnail_interface = true;
            metadata.thumbnail.assign(std::istreambuf_iterator<char>(input),{});
            p.opened_track_.emplace(); p.opened_track_->path = directory / L"track1.flac";
            ProgressSeekAccess::Publish(p.audio_,metadata,audio::PlaybackState::playing);
            p.UpdateVisualFrame(); p.PaintVisualControl(canvas.dc);
            Pixel(canvas.dc,64,40,RGB(255,0,0)); // current album wins over green fallback
            ProgressSeekAccess::Publish(p.audio_,metadata,audio::PlaybackState::paused);
            p.UpdateVisualWindowLayout(); p.UpdateVisualFrame(); p.PaintVisualControl(canvas.dc);
            Pixel(canvas.dc,64,40,RGB(255,0,0)); // pause + resize must retain the cover
            p.opened_track_->path = directory / L"track2.flac";
            metadata.thumbnail.clear();
            ProgressSeekAccess::Publish(p.audio_,metadata,audio::PlaybackState::playing);
            p.UpdateVisualFrame(); p.PaintVisualControl(canvas.dc);
            Pixel(canvas.dc,64,40,RGB(0,255,0)); // no stale previous cover
            metadata.thumbnail = {0,1,2,3};
            ProgressSeekAccess::Publish(p.audio_,metadata,audio::PlaybackState::playing);
            p.UpdateVisualFrame(); p.PaintVisualControl(canvas.dc);
            Pixel(canvas.dc,64,40,RGB(0,255,0)); // undecodable embedded picture falls back too
            ProgressSeekAccess::Publish(p.audio_,{},audio::PlaybackState::stopped);
            p.UpdateVisualFrame(); p.PaintVisualControl(canvas.dc);
            Pixel(canvas.dc,64,40,RGB(0,255,0));
            p.settings_.fullscreen.album_transparency_percent = 50;
            render(); Pixel(canvas.dc,64,40,RGB(0,128,127),1);
            for (int i = 0; i < 20; ++i) p.PaintVisualControl(canvas.dc);
            Pixel(canvas.dc,64,40,RGB(0,128,127),1); // cached frames must not accumulate alpha
            p.settings_.fullscreen.album_transparency_percent = 100;
            render(); Pixel(canvas.dc,64,40,RGB(0,0,255));
            const DWORD transparent_red[4]{0x80ff0000,0x80ff0000,0x80ff0000,0x80ff0000};
            const auto transparent = directory / L"alpha.png";
            SavePng(transparent,2,2,transparent_red);
            p.settings_.fullscreen.album_fallback_image = transparent.wstring();
            p.settings_.fullscreen.album_transparency_percent = 50;
            render(); Pixel(canvas.dc,64,40,RGB(64,0,191),2);
            const auto broken = directory / L"broken.png";
            { std::ofstream file(broken); file << "not an image"; }
            p.settings_.fullscreen.album_fallback_image = broken.wstring();
            render(); Pixel(canvas.dc,64,40,RGB(0,0,255));
            p.settings_.fullscreen.album_fallback_image = (directory / L"absent.png").wstring();
            render(); Pixel(canvas.dc,64,40,RGB(0,0,255));
            // Test the actual detached context-menu path, auto-dismissed.
            menu_album_label.clear(); album_checked = false;
            const auto timer = SetTimer(nullptr,0,150,[](HWND,UINT,UINT_PTR,DWORD){ EndMenu(); });
            Require(timer != 0, "menu timeout unavailable");
            p.ShowVisualContextMenu({10,10}); KillTimer(nullptr,timer);
            Require(menu_album_label == L"专辑图片" && album_checked, "combined menu missing album option/checkmark");
            p.fullscreen_mode_ = 2; menu_album_label.clear();
            const auto visual_timer = SetTimer(nullptr,0,150,[](HWND,UINT,UINT_PTR,DWORD){ EndMenu(); });
            p.ShowVisualContextMenu({10,10}); KillTimer(nullptr,visual_timer);
            Require(menu_album_label.empty(), "visual-only menu unexpectedly includes combined album mode");
            // Real TrackPopupMenu from the lyric area: exact top-level order,
            // labels, checkmarks, separator and original lyric/monitor entries.
            p.lyric_control_ = visual.value;
            capture_lyric_menu = true;
            for (int mode : {1,3}) for (int type = 1; type <= 4; ++type) {
                p.fullscreen_mode_ = mode; p.settings_.visual.type = type;
                lyric_menu_seen = false;
                const auto dismiss = SetTimer(nullptr,0,120,[](HWND,UINT,UINT_PTR,DWORD){ EndMenu(); });
                Require(dismiss != 0,"lyric menu timeout unavailable");
                p.ShowFullScreenLyricContextMenu({10,10}); KillTimer(nullptr,dismiss);
                if (!(lyric_menu_seen && lyric_separator && lyric_commands && monitor_menu))
                    std::cerr << "lyric menu flags: " << lyric_menu_seen << ',' << lyric_separator << ','
                              << lyric_commands << ',' << monitor_menu << " first=" << std::hex
                              << lyric_ids[0] << std::dec << '\n';
                Require(lyric_menu_seen && lyric_separator && lyric_commands && monitor_menu,
                        "lyric fullscreen menu lost its effect header or existing commands");
                const std::array<std::wstring,4> expected{L"梦幻星空",L"频谱分析",L"示波显示",L"专辑图片"};
                Require(lyric_labels == expected,"lyric fullscreen effect labels/order incorrect");
                for (UINT i = 0; i < 4; ++i) {
                    Require(lyric_ids[i] == kCmdVisualDream+i,"lyric effect command ID incorrect");
                    Require((lyric_states[i] & (MF_DISABLED | MF_GRAYED | MF_OWNERDRAW | MF_POPUP)) == 0,
                            "lyric effect should be an enabled native top-level command");
                    Require(((lyric_states[i] & MF_CHECKED) != 0) == (mode == 3 && type == static_cast<int>(i+1)),
                            "lyric effect menu checkmark is stale or checks a hidden visual");
                }
            }
            capture_lyric_menu = false; p.lyric_control_ = nullptr;
            const HMENU ordinary = DetachFirstPopup(LoadMenuW(resources,MAKEINTRESOURCEW(kMenuLyricDisplay)));
            Require(ordinary != nullptr,"normal lyric menu fixture unavailable");
            p.PrepareLyricMenu(ordinary);
            const bool unchanged = GetMenuItemID(ordinary,0) != kCmdVisualDream &&
                                   !FindCommandMenu(ordinary,kCmdVisualCover);
            DestroyMenu(ordinary);
            Require(unchanged,"normal lyric context menu was changed by the fullscreen extension");
            p.fullscreen_mode_ = 0; p.fullscreen_visual_detached_ = false;
            p.visual_window_ = nullptr;
            Window page{CreateDialogParamW(resources, MAKEINTRESOURCEW(263), owner.value,
                [](HWND window,UINT message,WPARAM wp,LPARAM lp)->INT_PTR {
                    if (message == WM_INITDIALOG) {
                        SetWindowLongPtrW(window,DWLP_USER,lp); return TRUE;
                    }
                    auto* player = reinterpret_cast<ui::PlayerWindow*>(GetWindowLongPtrW(window,DWLP_USER));
                    return player && message == WM_DRAWITEM
                        ? player->HandleOptionsPageDialog(window,message,wp,lp) : FALSE;
                },reinterpret_cast<LPARAM>(&p))};
            Require(page.value != nullptr, "original fullscreen options template failed");
            SetPropW(page.value,L"TTPlayer.Options.Template",reinterpret_cast<HANDLE>(263));
            p.InitializeOptionsPage(page.value,263);
            Require(SendDlgItemMessageW(page.value,2232,CB_GETCOUNT,0,0) == 4,
                    "fullscreen options does not contain four effects");
            wchar_t label[256]{};
            SendDlgItemMessageW(page.value,2232,CB_GETLBTEXT,3,reinterpret_cast<LPARAM>(label));
            Require(std::wstring_view(label) == L"专辑图片", "fourth effect label wrong");
            Require(GetDlgItem(page.value,1158) != nullptr, "existing lyric background colour control removed");
            const HWND drag_option = GetDlgItem(page.value,IDC_FULLSCREEN_LYRIC_DRAG);
            Require(drag_option && IsDlgButtonChecked(page.value,IDC_FULLSCREEN_LYRIC_DRAG) == BST_CHECKED,
                    "fullscreen drag option missing or default changed");
            CheckDlgButton(page.value,IDC_FULLSCREEN_LYRIC_DRAG,BST_UNCHECKED);
            p.HandleOptionsPageDialog(page.value,WM_COMMAND,MAKEWPARAM(IDC_FULLSCREEN_LYRIC_DRAG,BN_CLICKED),0);
            Require(!p.settings_.lyric.fullscreen_drag_lyric && p.settings_.lyric.drag_lyric,
                    "fullscreen drag option not committed independently");
            CheckDlgButton(page.value,IDC_FULLSCREEN_LYRIC_DRAG,BST_CHECKED);
            p.HandleOptionsPageDialog(page.value,WM_COMMAND,MAKEWPARAM(IDC_FULLSCREEN_LYRIC_DRAG,BN_CLICKED),0);
            Require(p.settings_.lyric.fullscreen_drag_lyric, "fullscreen drag option not enabled");
            RECT drag_rect{}, font_rect{}, overlap{};
            GetWindowRect(drag_option,&drag_rect); GetWindowRect(GetDlgItem(page.value,1036),&font_rect);
            Require(!IntersectRect(&overlap,&drag_rect,&font_rect), "drag checkbox overlaps font picker");
            RECT client{}; GetClientRect(page.value,&client);
            for (const int id : {IDC_FULLSCREEN_ALBUM_PATH,IDC_FULLSCREEN_ALBUM_BROWSE,
                 IDC_FULLSCREEN_ALBUM_CLEAR,IDC_FULLSCREEN_ALBUM_TRANSPARENCY,IDC_FULLSCREEN_ALBUM_PERCENT,
                 IDC_FULLSCREEN_LYRIC_DRAG}) {
                const HWND item = GetDlgItem(page.value,id); Require(item != nullptr, "album option missing");
                RECT rect{}; GetWindowRect(item,&rect);
                MapWindowPoints(nullptr,page.value,reinterpret_cast<POINT*>(&rect),2);
                Require(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right &&
                        rect.bottom <= client.bottom, "album option clipped by page");
            }
            SendDlgItemMessageW(page.value,2232,CB_SETCURSEL,3,0);
            p.HandleOptionsPageDialog(page.value,WM_COMMAND,MAKEWPARAM(2232,CBN_SELCHANGE),0);
            SendDlgItemMessageW(page.value,2231,CB_SETCURSEL,7,0);
            p.HandleOptionsPageDialog(page.value,WM_COMMAND,MAKEWPARAM(2231,CBN_SELCHANGE),0);
            Require(p.settings_.fullscreen.visual_type == 4 && p.settings_.fullscreen.lyric_size[4] == 8 &&
                p.settings_.fullscreen.lyric_size[1] == 2, "album layout overwrote another effect profile");
            CheckDlgButton(page.value,2233,BST_CHECKED);
            p.HandleOptionsPageDialog(page.value,WM_COMMAND,MAKEWPARAM(2233,BN_CLICKED),0);
            Require(p.settings_.fullscreen.visual_type == 0 && !IsWindowEnabled(GetDlgItem(page.value,2232)),
                    "shared layout mode no longer supported");
            const auto slider = GetDlgItem(page.value,IDC_FULLSCREEN_ALBUM_TRANSPARENCY);
            SendMessageW(slider,TBM_SETPOS,TRUE,73);
            p.HandleOptionsPageDialog(page.value,WM_HSCROLL,TB_THUMBPOSITION,reinterpret_cast<LPARAM>(slider));
            Require(p.settings_.fullscreen.album_transparency_percent == 73, "transparency slider not committed");
            p.HandleOptionsPageDialog(page.value,WM_COMMAND,MAKEWPARAM(IDC_FULLSCREEN_ALBUM_CLEAR,BN_CLICKED),0);
            Require(p.settings_.fullscreen.album_fallback_image.empty(), "clear fallback not committed");
            p.settings_.fullscreen.album_fallback_image = fallback.wstring();
            p.InitializeOptionsPage(page.value,263);
            CheckDlgButton(page.value,2233,BST_UNCHECKED);
            p.HandleOptionsPageDialog(page.value,WM_COMMAND,MAKEWPARAM(2233,BN_CLICKED),0);
            SendDlgItemMessageW(page.value,2232,CB_SETCURSEL,3,0);
            p.HandleOptionsPageDialog(page.value,WM_COMMAND,MAKEWPARAM(2232,CBN_SELCHANGE),0);
            // Artifact for human layout review, using the real 5.7.9 template.
            if (!screenshot.empty()) {
                SetWindowPos(owner.value,nullptr,50,50,client.right,client.bottom,
                             SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
                SetWindowPos(page.value,nullptr,0,0,0,0,SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
                ShowWindow(page.value,SW_SHOWNOACTIVATE);
                UpdateWindow(page.value);
                Canvas capture(client.right,client.bottom);
                PrintWindow(page.value,capture.dc,PW_CLIENTONLY);
                GdiFlush();
                for (int i = 0; i < capture.width*capture.height; ++i) capture.pixels[i] |= 0xff000000;
                SavePng(screenshot,capture.width,capture.height,capture.pixels);
            }
            p.window_ = p.visual_window_ = nullptr; p.fullscreen_mode_ = 0;
        } catch (...) {
            p.window_ = p.visual_window_ = p.lyric_control_ = nullptr; p.fullscreen_mode_ = 0;
            p.fullscreen_visual_detached_ = false;
            throw;
        }
        std::cout << "host HWND rendering, WIC PNG fallback/alpha, missing/corrupt images, cached paint,\n"
                     "album priority, track changes, paused reconfiguration and OLE fallback renderer,\n"
                     "real fullscreen menus, four-effect options and independent/shared layout passed\n";
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc >= 2, "expected original repository root");
        Hr(OleInitialize(nullptr));
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&common) != FALSE,"common controls unavailable");
        Temp fixture;
        GeometryAndAlpha(); SettingsRoundtrip(fixture.path);
        const HMODULE resources = LoadLibraryExW((fs::path(argv[1])/L"ttpres.dll").c_str(),nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Require(resources != nullptr, "5.7.9 ttpres.dll unavailable");
        testing::SkinRebindAccess::Run(resources,fixture.path,argc >= 3 ? fs::path(argv[2]) : fs::path{});
        FreeLibrary(resources); OleUninitialize(); return 0;
    } catch (const std::exception& error) {
        std::cerr << "fullscreen album test: " << error.what() << '\n'; return 1;
    }
}
