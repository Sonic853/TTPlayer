// Host regression: synthetic alpha/colour-key fixtures always run; original
// resource/package checks run when the optional local RE corpus is available.
#include "ttplayer/skin/skin.h"
#include "ttplayer/skin/skin_package.h"
#include "ttplayer/ui/player_window.h"
#include "../ui/player_window_internal.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Require(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
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
        Clear();
    }
    ~Canvas() { SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); }
    void Clear() {
        GdiFlush();
        for (int i = 0; i < width * height; ++i)
            pixels[i] = ((i / width + i % width) & 1) ? 0xff24486c : 0xffccaa88;
    }
    void Save(const fs::path& path) {
        GdiFlush();
        BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
        file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + width * height * 4;
        info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
        info.biPlanes = 1; info.biBitCount = 32;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file),sizeof(file));
        out.write(reinterpret_cast<const char*>(&info),sizeof(info));
        out.write(reinterpret_cast<const char*>(pixels),width*height*4);
    }
};
void EqualPixels(const Canvas& a, const Canvas& b, const char* message) {
    GdiFlush();
    for (int i=0; i<a.width*a.height; ++i)
        Require((a.pixels[i]&0xffffff) == (b.pixels[i]&0xffffff),message);
}
CLSID PngEncoder() {
    UINT count{},size{};
    Gdiplus::GetImageEncodersSize(&count,&size);
    std::vector<unsigned char> bytes(size);
    auto* encoders=reinterpret_cast<Gdiplus::ImageCodecInfo*>(bytes.data());
    Gdiplus::GetImageEncoders(count,size,encoders);
    for(UINT i=0;i<count;++i)
        if(wcscmp(encoders[i].MimeType,L"image/png")==0)return encoders[i].Clsid;
    throw std::runtime_error("PNG encoder unavailable");
}
void CompareImage(const fs::path& file) {
    const auto image=skin::SkinImage::Load(file);
    Require(image!=nullptr,"image load failed");
    const auto size=image.Size();
    Canvas actual(size.cx+6,size.cy+6), reference(size.cx+6,size.cy+6);
    if(image.IsGdiPlus()) {
        Gdiplus::Bitmap original(file.c_str());
        for(BYTE alpha : {BYTE(255),BYTE(128),BYTE(0)}) {
            actual.Clear(); reference.Clear();
            Require(image.Draw(actual.dc,2,2,size.cx,size.cy,0,0,size.cx,size.cy,
                RGB(255,0,255),alpha),"GDI+ draw failed");
            Gdiplus::Graphics g(reference.dc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            Gdiplus::ColorMatrix matrix{};
            for(int i=0;i<5;++i)matrix.m[i][i]=1;
            matrix.m[3][3]=alpha/255.0f;
            Gdiplus::ImageAttributes attributes; attributes.SetColorMatrix(&matrix);
            g.DrawImage(&original,Gdiplus::Rect(2,2,size.cx,size.cy),0,0,size.cx,size.cy,
                Gdiplus::UnitPixel,&attributes);
            g.Flush(Gdiplus::FlushIntentionSync);
            EqualPixels(actual,reference,"PNG source alpha/global alpha mismatch");
        }
    } else {
        Require(image.Draw(actual.dc,2,2,size.cx,size.cy,0,0,size.cx,size.cy,
            RGB(255,0,255)),"BMP draw failed");
        const HDC source=CreateCompatibleDC(nullptr);
        const auto old=SelectObject(source,static_cast<HBITMAP>(image));
        TransparentBlt(reference.dc,2,2,size.cx,size.cy,source,0,0,size.cx,size.cy,RGB(255,0,255));
        SelectObject(source,old); DeleteDC(source);
        EqualPixels(actual,reference,"legacy BMP colour key changed");
    }
}
void SyntheticTests(const fs::path& output) {
    const auto encoder=PngEncoder();
    {
        Gdiplus::Bitmap png(4,1,PixelFormat32bppARGB);
        png.SetPixel(0,0,Gdiplus::Color(0,255,0,255));
        png.SetPixel(1,0,Gdiplus::Color(128,255,0,0));
        png.SetPixel(2,0,Gdiplus::Color(255,255,0,255));
        png.SetPixel(3,0,Gdiplus::Color(255,0,255,0));
        Require(png.Save((output/L"alpha.png").c_str(),&encoder)==Gdiplus::Ok,"fixture save failed");
    }
    CompareImage(output/L"alpha.png");
    fs::copy_file(output/L"alpha.png",output/L"alpha.bmp",fs::copy_options::overwrite_existing);
    Require(skin::SkinImage::Load(output/L"alpha.bmp").IsGdiPlus(),"dispatch used extension");
    Canvas colors(4,2); colors.pixels[0]=0x00ff00ff; colors.Save(output/L"legacy.bmp");
    CompareImage(output/L"legacy.bmp");
    {std::ofstream bad(output/L"bad.png");bad<<"not a PNG";}
    Require(!skin::SkinImage::Load(output/L"bad.png"),"invalid image accepted");
    auto survivor=skin::SkinImage::Load(output/L"alpha.png");
    auto copy=survivor; survivor=nullptr;
    fs::remove(output/L"alpha.png");
    Require(copy.Draw(colors.dc,0,0,4,1,0,0,4,1),"image/stream lifetime lost after copy");
    skin::SkinAnimation def{1,10,16}; skin::SkinHoverAnimation hover;
    hover.SetState(1,def,100);
    Require(hover.remaining==10 && hover.HotOpacity(def)==0,"hover entry jump");
    for(int i=1;i<=4;++i)Require(hover.Tick(def,100+i*16),"hover timer interval");
    const int before=hover.HotOpacity(def);
    hover.SetState(0,def,164);
    Require(std::abs(before-hover.HotOpacity(def))<=1,"hover reversal jump");
    hover.SetState(2,def,164);Require(hover.remaining==0,"pressed frame should stop animation");
    def.frame_count=0;hover.SetState(1,def,200);Require(!hover.remaining,"invalid count animated");
    def={1,10,80}; skin::SkinPulseAnimation pulse;
    pulse.SetPlaying(true,def,0);Require(!pulse.Opacity(def),"pulse start jump");
    for(int i=1;i<=5;++i)pulse.Tick(def,i*80);
    Require(pulse.Opacity(def)==255,"pulse peak");
    pulse.SetPlaying(false,def,400);
    for(int i=6;i<=10;++i)pulse.Tick(def,i*80);
    Require(!pulse.remaining && !pulse.Opacity(def),"pause pulse did not finish");
}
void LayoutTests(const fs::path& directory) {
    auto layout=skin::LegacySkin::Load(directory);
    Require(layout.Valid()&&layout.SupportsMiniMode(),"player/mini layout failed");
    Require(layout.WindowSize().cx==336&&layout.WindowSize().cy==179,"main dimensions");
    const auto* play=layout.Find(L"play");
    Require(play&&play->image.IsGdiPlus()&&play->frames==4,"PNG four-state button missing");
    Require(play->bounds.right-play->bounds.left==56&&play->animation.frame_count==10&&
        play->animation.frame_interval==16,"animation frames confused with state frames");
    Require(layout.Find(L"set")->frames==4&&layout.Find(L"mode_random")->frames==4,"6.1 buttons missing");
    Require(layout.Find(L"login")->frames==4,"cloud login strip not parsed");
    Require(layout.Find(L"browser")->frames==4,"hidden browser strip should remain parsed");
    Require(layout.Find(L"progress")->flash_image.IsGdiPlus()&&
        layout.Find(L"progress")->fill_image2.IsGdiPlus(),"slider layers missing");
    Require(layout.Playlist().toolbar.image.IsGdiPlus()&&
        layout.Playlist().toolbar_animation.frame_count==8,"PNG toolbar missing");
    Require(layout.Playlist().selected_text_color==RGB(49,112,174),"Color_SelText not parsed");
    Require(layout.Equalizer().enabled.image.IsGdiPlus()&&layout.Lyric().close.image.IsGdiPlus(),
        "auxiliary PNG buttons missing");
    skin::LegacySkin moved=std::move(layout);
    Require(moved.FindMini(L"play")->image.IsGdiPlus(),"move lost PNG ownership");
}
} // namespace

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void Render(const fs::path& directory, const fs::path& output,
                       HMODULE resources, HMODULE comm) {
        settings::Settings settings;
        settings.source_path=output/L"test-player.xml";
        settings.general.fade_windows=false;
        settings.general.send_title_to_msn=false;
        settings.general.tray_icon=false;
        settings.lyric.auto_download=false;
        settings.player.mute=true;
        settings.player.play_mode=0;
        settings.player.playlist_visible=settings.player.equalizer_visible=settings.player.lyric_visible=false;
        ui::PlayerWindow player(settings);
        player.SetSkinResourceModule(resources);
        player.SetTtpCommModule(comm);
        if (resources && comm)
            Require(player.LoadSkinResource(resources),"6.1.2 ZIP resource loader failed");
        else player.skin_.emplace(skin::LegacySkin::Load(directory));
        Require(player.Create(GetModuleHandleW(nullptr),SW_HIDE),"host test window failed");
        try {
            const HWND original=player.window_;
            fs::path package;
            if (resources && comm) {
                fs::create_directories(output/L"Skin");
                package=output/L"Skin/TTPlayer_60.skn";
                fs::copy_file(directory.parent_path()/L"ZIP__DEFAULT_SKIN__2052.zip",package);
                // Exercise the real menu binder, including package colours,
                // rather than retaining this fixture's initial global colours.
                player.skin_commands_={{ui::detail::kCmdFirstSkin,package,L"TTPlayer_60",{},false}};
                Require(player.HandleContextCommand(ui::detail::kCmdFirstSkin)&&
                    player.settings_.skin_file==L"TTPlayer_60.skn"&&player.window_==original,
                    "PNG skin menu rebind replaced window or failed");
                Require(player.settings_.playlist.background_color==RGB(230,242,255),
                    "package playlist colours not applied on menu switch");
            }
            Canvas main(336,179);
            // Parent paint deliberately excludes the live visual child.
            player.ActiveSkinBackground().Draw(main.dc,0,0,336,179,0,0,336,179);
            player.PaintSkin(main.dc); main.Save(output/L"main.bmp");
            Canvas background(336,179);
            player.ActiveSkinBackground().Draw(background.dc,0,0,336,179,0,0,336,179);
            GdiFlush();
            for (const auto name : {L"login", L"login_name", L"browser"}) {
                const auto* hidden=player.skin_->Find(name);
                Require(hidden!=nullptr,"hidden control removed from parser");
                const RECT bounds=hidden->bounds;
                Require(!ui::detail::IsSkinButton(name)&&!player.IsSkinElementEnabled(name),
                    "hidden control still enabled");
                for (int y=bounds.top;y<bounds.bottom;++y)
                    for (int x=bounds.left;x<bounds.right;++x) {
                        Require(player.HitTestSkin({x,y}).empty(),"hidden control still accepts mouse input");
                        Require((main.pixels[y*336+x]&0xffffff)==(background.pixels[y*336+x]&0xffffff),
                            "hidden control still paints over background");
                    }
            }
            Require(std::none_of(player.tooltip_tools_.begin(),player.tooltip_tools_.end(),
                [&](const auto& tool) {return tool.owner==player.window_&&
                    tool.identifier==ui::detail::kCmdShowBrowser;}),"hidden browser still has tooltip");
            const auto* mode=player.skin_->Find(L"mode_single");
            const POINT hit{mode->bounds.left+2,mode->bounds.top+2};
            for(int index=0;index<5;++index) {
                const auto action=player.HitTestSkin(hit);
                Require(action.starts_with(L"mode_"),"mode button not hit-testable");
                player.InvokeSkinAction(action);
                Require(player.settings_.player.play_mode==(index+1)%5,"mode button did not cycle");
            }
            Canvas eq(336,136); player.PaintEqualizer(eq.dc);eq.Save(output/L"equalizer.bmp");
            RECT playlist_bounds{};GetClientRect(player.playlist_window_,&playlist_bounds);
            Canvas playlist(playlist_bounds.right,playlist_bounds.bottom);
            player.PaintPlaylist(playlist.dc);playlist.Save(output/L"playlist.bmp");
            player.ToggleMiniMode(); player.CompleteSkinWindowFadeForReplacement();
            Require(player.mini_mode_&&player.window_==original,"mini mode replaced main HWND");
            Canvas mini(396,30);player.PaintSkin(mini.dc);mini.Save(output/L"mini.bmp");
            player.ToggleMiniMode();player.CompleteSkinWindowFadeForReplacement();
            Require(!player.mini_mode_&&player.window_==original,"mini exit lost HWND");
            if (resources && comm) {
                Require(player.LoadSkinResource(resources)&&player.window_==original,
                    "PNG resource rebind failed");
            }
            player.ResetSkinControlAnimations();
            DestroyWindow(player.window_);
        } catch(...) {if(IsWindow(player.window_))DestroyWindow(player.window_);throw;}
    }
};
}

int wmain(int argc,wchar_t** argv) {
    ULONG_PTR token{}; Gdiplus::GdiplusStartupInput input;
    if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)return 1;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int result=0;
    try {
        const auto output=fs::temp_directory_path()/(L"TTPlayer-PNG-"+
            std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        fs::create_directories(output); SyntheticTests(output);
        const fs::path root=argc>1?argv[1]:L".";
        const auto directory=root/L"TTPlayer6120/reverse/resources/ttpres.dll/ZIP__DEFAULT_SKIN__2052";
        if(fs::exists(directory/L"Skin.xml")) {
            int png=0,bmp=0;
            for(const auto& entry:fs::directory_iterator(directory)) {
                const auto ext=entry.path().extension();
                if(ext!=L".png"&&ext!=L".bmp")continue;
                CompareImage(entry.path());
                if(ext==L".png")++png;else ++bmp;
            }
            Require(png==36&&bmp==75,"unexpected resource inventory");
            LayoutTests(directory);
            const auto resources=LoadLibraryExW((root/L"TTPlayer6120/ttpres.dll").c_str(),
                nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            const auto inflater=LoadLibraryW((root/L"ttpcomm.dll").c_str());
            testing::SkinRebindAccess::Render(directory,output,resources,inflater);
            if(inflater)FreeLibrary(inflater);
            if(resources)FreeLibrary(resources);
            std::cout<<"Real default: 36 PNG + 75 BMP pixel comparisons; main/mini/EQ/playlist rendering passed\n";
        } else std::cout<<"SKIP optional 6.1.2 corpus (synthetic tests passed)\n";
        const HMODULE comm=LoadLibraryW((root/L"ttpcomm.dll").c_str());
        if(comm) {
            int packages=0;
            for(const auto& base:{root/L"Skin",root/L"TTPlayer6120/Skin"}) {
                if(!fs::exists(base))continue;
                for(const auto& entry:fs::directory_iterator(base)) {
                    if(entry.path().extension()!=L".skn")continue;
                    const auto target=output/(L"package-"+std::to_wstring(packages));
                    auto package=skin::SkinPackage::Open(entry.path());
                    package.ExtractTo(target,comm);
                    Require(skin::LegacySkin::Load(target).Valid(),"BMP package no longer loads");
                    ++packages;
                }
            }
            FreeLibrary(comm);
            std::cout<<"Legacy packages loaded: "<<packages<<'\n';
        }
        std::wcout<<L"PASS; rendered fixtures: "<<output<<L'\n';
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';result=1;}
    if(SUCCEEDED(com))CoUninitialize();
    Gdiplus::GdiplusShutdown(token);
    return result;
}
