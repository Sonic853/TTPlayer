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
void SavePng(const fs::path& file, int width, int height, bool opaque = false) {
    Gdiplus::Bitmap image(width,height,PixelFormat32bppARGB);
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        const BYTE alpha = opaque ? 255 : x%4==0 ? 0 : x%4==1 ? 128 : 255;
        image.SetPixel(x,y,Gdiplus::Color(alpha,255,0,255));
    }
    const auto encoder=PngEncoder();
    Require(image.Save(file.c_str(),&encoder)==Gdiplus::Ok,"PNG fixture save failed");
}

void AllPngSlots(const fs::path& output) {
    const auto directory=output/L"all-png-slots";
    fs::create_directories(directory);
    SavePng(directory/L"all.PNG",32,8);
    // PNG content works even under the old extension; no filename guessing.
    fs::copy_file(directory/L"all.PNG",directory/L"encoded.bmp");
    const char* layers=" image=\"all.PNG\" bar_image=\"all.PNG\" fill_image=\"all.PNG\""
        " fill_image2=\"all.PNG\" thumb_image=\"all.PNG\" flash_image=\"all.PNG\""
        " flash_mode=\"2\" frame_count=\"4\" position=\"0,0,8,8\"";
    {
        std::ofstream xml(directory/L"Skin.xml",std::ios::binary);
        xml<<"<skin transparent_color=\"#ff00ff\">";
        for (const auto window:{"player_window","mini_window"}) {
            xml<<'<'<<window<<" image=\"encoded.bmp\">";
            for (const auto name:{"play","pause","prev","next","stop","open","mute",
                    "playlist","equalizer","lyric","minimize","minimode","close","ontop",
                    "set","mode_single","mode_repeat","mode_order","mode_random","mode_repeat_one",
                    "progress","volume","led","icon","title"})
                xml<<'<'<<name<<layers<<"/>";
            xml<<"</"<<window<<'>';
        }
        xml<<"<lyric_window image=\"all.PNG\">";
        for (const auto name:{"title","close","ontop","desklrc"}) xml<<'<'<<name<<layers<<"/>";
        xml<<"</lyric_window><equalizer_window image=\"all.PNG\">";
        for (const auto name:{"title","close","enabled","profile","reset","balance",
                "surround","preamp","eqfactor"}) xml<<'<'<<name<<layers<<"/>";
        xml<<"</equalizer_window><desklrc_bar image=\"all.PNG\">";
        for (const auto name:{"play","pause","prev","next","list","settings","kalaok",
                "lines","lock","ontop","zoomin","zoomout","return","close"})
            xml<<'<'<<name<<layers<<"/>";
        xml<<"</desklrc_bar><playlist_window image=\"all.PNG\">"
               "<title image=\"all.PNG\"/><close image=\"all.PNG\"/>"
               "<toolbar image=\"all.PNG\" hot_image=\"all.PNG\"/>"
               "<scrollbar buttons_image=\"all.PNG\" thumb_image=\"all.PNG\" bar_image=\"all.PNG\"/>"
               "<playlist selected_image=\"all.PNG\" splitter_bar_image=\"all.PNG\""
               " splitter_arrow_image=\"all.PNG\"/></playlist_window></skin>";
    }
    auto layout=skin::LegacySkin::Load(directory);
    size_t slots=0;
    const auto check=[&](const skin::SkinImage& image) {
        Require(image.IsGdiPlus() && image.Size().cx==32 && image.Size().cy==8,
                "PNG image slot not loaded / wrong dimensions"); ++slots;
        Canvas actual(32,8), expected(32,8);
        image.Draw(actual.dc,0,0,32,8,0,0,32,8,RGB(255,0,255));
        Gdiplus::Bitmap original((directory/L"all.PNG").c_str());
        { Gdiplus::Graphics graphics(expected.dc); graphics.DrawImage(&original,0,0,32,8); }
        EqualPixels(actual,expected,"PNG slot lost alpha or keyed opaque magenta");
    };
    const auto element=[&](const skin::SkinElement& e) {
        check(e.image); check(e.bar_image); check(e.fill_image);
        check(e.fill_image2); check(e.thumb_image); check(e.flash_image);
    };
    check(layout.Background()); check(layout.MiniBackground());
    for (const auto& e:layout.Elements()) element(e);
    for (const auto& e:layout.MiniElements()) element(e);
    const auto& lyric=layout.Lyric(); check(lyric.background.image);
    for (const auto* e:{&lyric.title,&lyric.close,&lyric.ontop,&lyric.desklrc}) element(*e);
    const auto& eq=layout.Equalizer(); check(eq.background.image);
    for (const auto* e:{&eq.title,&eq.close,&eq.enabled,&eq.profile,&eq.reset,
            &eq.balance,&eq.surround,&eq.preamp}) element(*e);
    for (const auto& e:eq.bands) element(e);
    const auto& bar=layout.DesktopLyricBar(); check(bar.background.image);
    for (const auto* e:{&bar.play,&bar.pause,&bar.previous,&bar.next,&bar.list,&bar.settings,
            &bar.karaoke,&bar.lines,&bar.lock,&bar.ontop,&bar.zoom_in,&bar.zoom_out,
            &bar.return_to_window,&bar.close}) element(*e);
    const auto& list=layout.Playlist();
    for (const auto* image:{&list.background.image,&list.title.image,&list.close.image,
            &list.toolbar.image,&list.toolbar_hot.image,&list.selected.image,
            &list.splitter_bar.image,&list.splitter_arrow.image,&list.scrollbar_buttons.image,
            &list.scrollbar_thumb.image,&list.scrollbar_bar.image}) check(*image);
    for (bool mini:{false,true}) {
        HRGN region=layout.CreateWindowRegion(mini);
        Require(region && !PtInRegion(region,0,0) && PtInRegion(region,1,0) &&
                PtInRegion(region,2,0),"PNG native region must use alpha, not magenta");
        DeleteObject(region);
    }
    // Independent coverage reference: render source alpha over black. All
    // nonzero alpha is part of the binary HWND region, including opaque key RGB.
    for (bool tile:{false,true}) for (SIZE size:{SIZE{32,8},SIZE{48,20},SIZE{16,4}}) {
        const RECT resize{4,2,28,6};
        Canvas reference(size.cx,size.cy);
        RECT client{0,0,size.cx,size.cy};
        FillRect(reference.dc,&client,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        ui::detail::DrawResizableSkinBitmap(reference.dc,list.background,resize,size.cx,size.cy,tile);
        HRGN region=ui::detail::CreateSkinWindowRegion(list.background,resize,size.cx,size.cy,
                                                     tile,RGB(255,0,255));
        Require(region!=nullptr,"PNG resized region missing");
        for (int y=0;y<size.cy;++y) for (int x=0;x<size.cx;++x)
            Require((PtInRegion(region,x,y)!=FALSE)==(GetPixel(reference.dc,x,y)!=RGB(0,0,0)),
                    "PNG resized/tiled region differs from rendered coverage");
        DeleteObject(region);
    }
    std::cout<<"All PNG slots: "<<slots<<" image references, alpha/key pixels, normal/mini and resized regions passed\n";
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
    const auto* led=moved.Find(L"led");
    Require(led!=nullptr,"default skin LED missing");
    const HBITMAP preview=ui::detail::RenderSkinPreview(moved);
    Require(preview!=nullptr,"default skin preview missing");
    Canvas actual(336,179),expected(336,179);
    skin::SkinImage(preview).Draw(actual.dc,0,0,336,179,0,0,336,179);
    DeleteObject(preview);
    moved.Background().Draw(expected.dc,0,0,336,179,0,0,336,179);
    ui::detail::DrawSkinLed(expected.dc,*led,L"00:00",moved.TransparentColor());
    for (int y=68;y<82;++y) for (int x=221;x<329;++x)
        Require(GetPixel(actual.dc,x,y)==GetPixel(expected.dc,x,y),
                "preview drew the full LED sprite strip across the progress bar");
}
} // namespace

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void PngBackgrounds(const fs::path& output) {
        const auto directory=output/L"png-backgrounds";
        fs::create_directories(directory);
        SavePng(directory/L"background.png",96,64);
        SavePng(directory/L"button.png",16,4);
        Canvas bmp(96,64);
        GdiFlush(); std::fill_n(bmp.pixels,96*64,0x00ff00ff);
        bmp.Save(directory/L"background.bmp");
        const auto load=[&](const char* background) {
            {
                std::ofstream xml(directory/L"Skin.xml",std::ios::binary);
                xml<<"<skin transparent_color=\"#ff00ff\"><player_window image=\""<<background<<"\">"
                    "<play image=\"button.png\" position=\"4,4,8,8\"/>"
                    "<visual position=\"16,16,48,48\"/></player_window>"
                    "<mini_window image=\"background.png\"/>"
                    "<equalizer_window image=\"background.png\"/>"
                    "<lyric_window image=\"background.png\"><close image=\"button.png\" position=\"4,4,8,8\"/>"
                    "</lyric_window></skin>";
            }
            return skin::LegacySkin::Load(directory);
        };
        // Preview uses a window-colour matte for both BMP and PNG backings;
        // opaque magenta PNG buttons must survive even over a keyed BMP.
        for (const auto file:{"background.png","background.bmp"}) {
            const auto layout=load(file);
            HBITMAP preview=ui::detail::RenderSkinPreview(layout);
            Require(preview!=nullptr,"PNG preview render failed");
            Canvas actual(96,64),expected(96,64);
            skin::SkinImage(preview).Draw(actual.dc,0,0,96,64,0,0,96,64);
            const RECT bounds{0,0,96,64};
            FillRect(expected.dc,&bounds,GetSysColorBrush(COLOR_WINDOW));
            if (layout.Background().IsGdiPlus())
                layout.Background().Draw(expected.dc,0,0,96,64,0,0,96,64);
            layout.Find(L"play")->image.Draw(expected.dc,4,4,4,4,0,0,4,4);
            DeleteObject(preview);
            EqualPixels(actual,expected,"preview alpha/matte/opaque-magenta mismatch");
        }
        settings::Settings settings;
        settings.general.send_title_to_msn=false; settings.general.tray_icon=false;
        settings.lyric.transparent=false; settings.visual.type=0;
        ui::PlayerWindow player(settings);
        player.skin_.emplace(load("background.png"));
        const auto window=[&](HWND parent=nullptr,int id=0,int x=0,int y=0,int w=96,int h=64) {
            const HWND value=CreateWindowExW(0,L"STATIC",L"",parent?WS_CHILD:WS_POPUP,x,y,w,h,
                parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            Require(value!=nullptr,"PNG backing test window failed"); return value;
        };
        player.window_=window(); player.lyric_window_=window();
        player.visual_window_=window(player.window_,1,16,16,32,32);
        const HWND close=window(player.lyric_window_,ui::detail::kCmdShowLyrics,4,4,4,4);
        const auto cleanup=[&] {
            player.ResetSkinControlAnimations();
            DestroyWindow(player.window_); DestroyWindow(player.lyric_window_);
            player.window_=player.visual_window_=player.lyric_window_=nullptr;
        };
        try {
            Canvas actual(96,64),expected(96,64);
            const RECT bounds{0,0,96,64};
            FillRect(expected.dc,&bounds,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            player.skin_->Background().Draw(expected.dc,0,0,96,64,0,0,96,64);
            player.PaintEqualizer(actual.dc); EqualPixels(actual,expected,"EQ PNG backing is uninitialized");
            player.PaintLyricWindow(actual.dc); EqualPixels(actual,expected,"lyric PNG backing differs");
            Canvas child(4,4),child_expected(4,4);
            BitBlt(child_expected.dc,0,0,4,4,expected.dc,4,4,SRCCOPY);
            player.skin_->Lyric().close.image.Draw(child_expected.dc,0,0,4,4,0,0,4,4);
            player.PaintLyricControl(close,child.dc);
            EqualPixels(child,child_expected,"lyric PNG button backing/alpha mismatch");
            player.UpdateVisualWindowLayout();
            Canvas visual(32,32),visual_expected(32,32);
            BitBlt(visual_expected.dc,0,0,32,32,expected.dc,16,16,SRCCOPY);
            player.PaintVisualControl(visual.dc);
            EqualPixels(visual,visual_expected,"visual PNG backing cache lost alpha");
            player.skin_->Find(L"play")->image.Draw(expected.dc,4,4,4,4,0,0,4,4);
            player.PaintSkin(actual.dc);
            // Live visual child is intentionally clipped out of the parent paint.
            for (int y=0;y<16;++y) for(int x=0;x<96;++x)
                Require(GetPixel(actual.dc,x,y)==GetPixel(expected.dc,x,y),"main PNG backing/button mismatch");
            cleanup();
        } catch (...) {cleanup();throw;}
        std::cout<<"PNG backgrounds: main/EQ/lyric/button/visual cache and mixed BMP/PNG previews passed\n";
    }

    static void PngPlaylist(const fs::path& output) {
        const auto directory=output/L"png-playlist";
        fs::create_directories(directory);
        SavePng(directory/L"background.png",300,200);
        SavePng(directory/L"buttons.png",12,8);
        SavePng(directory/L"thumb.png",12,6);
        SavePng(directory/L"bar.png",4,4);
        SavePng(directory/L"arrow.png",8,4);
        {
            std::ofstream xml(directory/L"Skin.xml",std::ios::binary);
            xml<<"<skin transparent_color=\"#ff00ff\"><player_window image=\"background.png\"/>"
                "<playlist_window image=\"background.png\" resize_rect=\"8,8,292,192\">"
                "<scrollbar buttons_image=\"buttons.png\" thumb_image=\"thumb.png\" bar_image=\"bar.png\"/>"
                "<playlist position=\"8,24,292,180\" splitter_bar_image=\"bar.png\""
                " splitter_arrow_image=\"arrow.png\"/></playlist_window></skin>";
        }
        settings::Settings settings;
        settings.general.send_title_to_msn=false; settings.general.tray_icon=false;
        settings.playlist.split_on_lists=60;
        ui::PlayerWindow player(settings);
        player.skin_.emplace(skin::LegacySkin::Load(directory));
        player.playlists_.NewList(L"PNG fixture");
        for (int i=0;i<20;++i) {
            playlist::Track track; track.title="Fixture"; track.duration_ms=1000;
            player.playlists_.Active().Add(std::move(track));
        }
        player.playlist_window_=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,
            0,0,300,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Require(player.playlist_window_!=nullptr,"PNG playlist test window failed");
        try {
            auto& layout=const_cast<skin::PlaylistSkin&>(player.skin_->Playlist());
            const auto geometry=ui::detail::MakePlaylistGeometry(layout,60,300,200,20);
            Require(geometry.scrollbar_width==4,"fixture did not show scrollbar");
            for (const int center:{0,2}) for (bool tile:{false,true}) for (int frame=0;frame<3;++frame) {
                layout.scrollbar_thumb_resize_center=center;
                layout.scrollbar_thumb_resize_tile=tile;
                // Give each part the requested state in independent paints.
                for (auto part:{ui::PlaylistScrollbarPart::line_up,ui::PlaylistScrollbarPart::line_down,
                        ui::PlaylistScrollbarPart::thumb}) {
                    player.playlist_scrollbar_hover_=frame ? part : ui::PlaylistScrollbarPart::none;
                    player.playlist_scrollbar_pressed_=frame==2 ? part : ui::PlaylistScrollbarPart::none;
                    Canvas actual(300,200),expected(300,200);
                    player.PaintPlaylist(actual.dc);
                    const RECT client{0,0,300,200};
                    FillRect(expected.dc,&client,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                    layout.background.image.Draw(expected.dc,0,0,300,200,0,0,300,200);
                    // The native-size background must take the same alpha path
                    // as resized backgrounds (previously it was raw BitBlt).
                    for (int x=0;x<300;++x)
                        Require(GetPixel(actual.dc,x,0)==GetPixel(expected.dc,x,0),"playlist PNG backing bypassed alpha");
                    ui::detail::TileBitmap(expected.dc,layout.scrollbar_bar,geometry.scrollbar);
                    const auto state=ui::MakePlaylistScrollbarMetrics(24,180,4,6,center,20,
                        static_cast<size_t>(geometry.visible_rows),0);
                    RECT area{};
                    if (part!=ui::PlaylistScrollbarPart::thumb) {
                        const bool down=part==ui::PlaylistScrollbarPart::line_down;
                        area={geometry.scrollbar.left,down?176:24,geometry.scrollbar.right,down?180:28};
                        layout.scrollbar_buttons.image.Draw(expected.dc,area.left,area.top,4,4,
                            frame*4,down?4:0,4,4);
                    } else {
                        area={geometry.scrollbar.left,state.thumb_top,geometry.scrollbar.right,state.thumb_bottom};
                        const auto draw=[&](int y,int height,int sy,int sh) {
                            layout.scrollbar_thumb.image.Draw(expected.dc,area.left,y,4,height,frame*4,sy,4,sh);
                        };
                        if (!center) draw(area.top,6,0,6);
                        else {
                            draw(area.top,2,0,2); draw(area.bottom-2,2,4,2);
                            const int extent=area.bottom-area.top-4;
                            if (!tile) draw(area.top+2,extent,2,2);
                            else for (int offset=0;offset<extent;offset+=2)
                                draw(area.top+2+offset,std::min(2,extent-offset),2,std::min(2,extent-offset));
                        }
                    }
                    for (int y=area.top;y<area.bottom;++y) for (int x=area.left;x<area.right;++x)
                        Require(GetPixel(actual.dc,x,y)==GetPixel(expected.dc,x,y),"PNG scrollbar frame/cap/center alpha mismatch");
                    // Arrow is drawn over the splitter, not colour-key copied.
                    ui::detail::TileBitmap(expected.dc,layout.splitter_bar,geometry.splitter);
                    const int arrow_y=geometry.list.top+(geometry.list.bottom-geometry.list.top-4)/2;
                    layout.splitter_arrow.image.Draw(expected.dc,geometry.splitter.right-4,arrow_y,4,4,0,0,4,4);
                    for (int y=arrow_y;y<arrow_y+4;++y) for (int x=geometry.splitter.right-4;x<geometry.splitter.right;++x)
                        Require(GetPixel(actual.dc,x,y)==GetPixel(expected.dc,x,y),"PNG splitter arrow lost alpha or opaque magenta");
                }
            }
            DestroyWindow(player.playlist_window_); player.playlist_window_=nullptr;
        } catch (...) {DestroyWindow(player.playlist_window_);player.playlist_window_=nullptr;throw;}
        std::cout<<"PNG playlist: native backing, splitter arrow, scrollbar three states, thumb caps/stretch/tile pixels passed\n";
    }

    static void VolumeSlider(const fs::path& output) {
        const auto fixture=output/L"volume-slider";
        fs::create_directories(fixture);
        Canvas background(368,160), bar(53,4), fill(53,4), thumb(72,18);
        GdiFlush();
        std::fill(bar.pixels,bar.pixels+53*4,0x00101020);
        std::fill(fill.pixels,fill.pixels+53*4,0x00ff2020);
        std::fill(thumb.pixels,thumb.pixels+72*18,0x00ffffff);
        background.Save(fixture/L"background.bmp");bar.Save(fixture/L"bar.bmp");
        fill.Save(fixture/L"fill.bmp");thumb.Save(fixture/L"thumb.bmp");
        {
            std::ofstream xml(fixture/L"Skin.xml",std::ios::binary);
            xml << "<skin><player_window image=\"background.bmp\">"
                   "<volume position=\"284,105,357,123\" bar_image=\"bar.bmp\" "
                   "fill_image=\"fill.bmp\" thumb_image=\"thumb.bmp\"/>"
                   "</player_window></skin>";
        }
        settings::Settings settings;
        settings.general.send_title_to_msn=false;
        settings.general.tray_icon=false;
        ui::PlayerWindow player(settings);
        player.skin_.emplace(skin::LegacySkin::Load(fixture));
        player.window_=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,
            0,0,368,160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Require(player.window_!=nullptr,"volume test window failed");
        try {
            // Same one-pixel inset / thumb-centre mapping as FUN_00428F41.
            for (const auto& sample : {std::pair{294,0},std::pair{320,49},
                    std::pair{347,100},std::pair{370,100},std::pair{270,0}}) {
                player.SetSkinVolumeFromPoint({sample.first,114});
                Require(player.settings_.player.volume==sample.second,"volume endpoints not reachable");
            }
            Canvas paint(368,160);
            player.PaintSkin(paint.dc);
            Require(GetPixel(paint.dc,320,114)==RGB(16,16,32),"volume background layer missing at zero");
            Require(GetPixel(paint.dc,294,114)==RGB(255,255,255),"zero-volume knob centre shifted");
            Require(GetPixel(paint.dc,284,114)==GetPixel(background.dc,284,114),
                    "zero-volume fill extends before knob travel");
            player.SetSkinVolumeFromPoint({320,114});player.PaintSkin(paint.dc);
            Require(GetPixel(paint.dc,305,114)==RGB(255,32,32) &&
                    GetPixel(paint.dc,339,114)==RGB(16,16,32),"volume fill/background order mismatch");
            player.SetSkinVolumeFromPoint({347,114});player.PaintSkin(paint.dc);
            Require(GetPixel(paint.dc,320,114)==RGB(255,32,32) &&
                    GetPixel(paint.dc,347,114)==RGB(255,255,255),"full-volume endpoint mismatch");
            DestroyWindow(player.window_);player.window_=nullptr;
        } catch (...) {DestroyWindow(player.window_);player.window_=nullptr;throw;}
        std::cout << "Volume slider: zero/middle/full/outside drag, bar/fill/thumb order and endpoint alignment passed\n";
    }
    static void ToolbarLayout(const fs::path& output) {
        const auto fixture = output / L"toolbar-layout";
        fs::create_directories(fixture);
        Canvas background(368,630), hot(359,56);
        background.Save(fixture / L"background.bmp");
        GdiFlush();
        std::fill(hot.pixels, hot.pixels + hot.width * hot.height, 0x00e01020);
        hot.Save(fixture / L"hot.bmp");
        const char* custom =
            "<item index=\"0\" position=\"63,28,112,56\"/>"
            "<item index=\"1\" position=\"161,28,211,56\"/>"
            "<item index=\"2\" position=\"211,28,260,56\"/>"
            "<item index=\"3\" position=\"112,28,161,56\"/>"
            "<item index=\"4\" position=\"207,2,225,20\"/>"
            "<item index=\"5\" position=\"260,28,309,56\"/>"
            "<item index=\"6\" position=\"309,28,359,56\"/>";
        auto load = [&](const char* items) {
            {
                std::ofstream xml(fixture/L"Skin.xml",std::ios::binary);
                xml << "<skin><player_window image=\"background.bmp\"/>"
                       "<playlist_window image=\"background.bmp\"><toolbar position=\"8,145,367,201\" "
                       "image=\"hot.bmp\" hot_image=\"hot.bmp\">" << items <<
                       "</toolbar><playlist position=\"1,202,367,629\"/></playlist_window></skin>";
            }
            return skin::LegacySkin::Load(fixture);
        };
        settings::Settings settings;
        settings.general.send_title_to_msn = false;
        settings.general.tray_icon = false;
        ui::PlayerWindow player(settings);
        player.playlists_.NewList(L"Toolbar fixture");
        player.playlist_window_ = CreateWindowExW(0,L"STATIC",L"",WS_POPUP,
            0,0,368,630,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Require(player.playlist_window_ != nullptr,"toolbar test window failed");
        try {
            player.skin_.emplace(load(custom));
            const auto& layout = player.skin_->Playlist();
            Require(layout.valid && layout.toolbar_items.has_value(),"custom toolbar XML not parsed");
            const RECT toolbar{8,145,367,201};
            const RECT expected[] = {{71,173,120,201},{169,173,219,201},{219,173,268,201},
                {120,173,169,201},{215,147,233,165},{268,173,317,201},{317,173,367,201}};
            for (size_t index=0;index<7;++index) {
                const RECT rect = ui::detail::PlaylistToolbarItemBounds(layout,toolbar,index);
                Require(EqualRect(&rect,&expected[index]),"custom toolbar rectangle mismatch");
                for (int y=rect.top;y<rect.bottom;++y) for (int x=rect.left;x<rect.right;++x)
                    Require(player.PlaylistToolbarButtonAt({x,y})==index,"custom toolbar mouse hit mismatch");
                Canvas actual(368,220), baseline(368,220);
                ui::detail::DrawPlaylistToolbarBitmap(actual.dc,layout.toolbar_hot,toolbar,
                    RGB(255,0,255),index,255,&layout);
                GdiFlush();
                for (int y=0;y<220;++y) for (int x=0;x<368;++x) {
                    const bool inside=PtInRect(&rect,POINT{x,y})!=FALSE;
                    const DWORD want=inside?0x00e01020:baseline.pixels[y*368+x]&0xffffff;
                    Require((actual.pixels[y*368+x]&0xffffff)==want,"hot frame spills into another item");
                }
            }
            for (POINT blank : {POINT{10,150},POINT{214,156},POINT{233,156},POINT{224,166},POINT{60,188}})
                Require(!player.PlaylistToolbarButtonAt(blank),"open field / toolbar gap captured by toolbar");
            player.skin_.emplace(load("<item index=\"4\" position=\"207,2,225,20\"/>"));
            Require(!player.PlaylistToolbarButtonAt({130,188}) && player.PlaylistToolbarButtonAt({224,156})==size_t{4},
                    "undeclared custom toolbar item remains clickable");
            for (const char* fallback : {"", "<item index=\"8\" position=\"1,1,10,10\"/>"
                "<item index=\"0\" position=\"0,0,500,20\"/><item index=\"1\" position=\"-1,0,2,2\"/>"}) {
                player.skin_.emplace(load(fallback));
                Require(!player.skin_->Playlist().toolbar_items,"invalid custom item changed legacy layout");
                Require(player.PlaylistToolbarButtonAt({20,150})==size_t{0} &&
                        !player.PlaylistToolbarButtonAt({20,190}) &&
                        player.PlaylistToolbarButtonAt({224,190})==size_t{4} &&
                        !player.PlaylistToolbarButtonAt({224,150}),"legacy two-row toolbar behavior changed");
            }
            DestroyWindow(player.playlist_window_);player.playlist_window_=nullptr;
        } catch (...) {
            DestroyWindow(player.playlist_window_);player.playlist_window_=nullptr;throw;
        }
        std::cout << "Toolbar layouts: XML, all seven hit regions, isolated hot frames, gaps, hidden items, legacy fallback passed\n";
    }
    static void MiniLyricAppearance(const fs::path& output) {
        const auto fixture = output / L"mini-style";
        fs::create_directories(fixture);
        Canvas background(200, 35);
        background.Save(fixture / L"background.bmp");
        settings::Settings settings;
        settings.general.send_title_to_msn = false;
        settings.general.tray_icon = false;
        settings.lyric.auto_download = false;
        settings.lyric.text_color = RGB(12, 34, 56);
        settings.lyric.highlight_color = RGB(34, 56, 78);
        settings.lyric.background_color = RGB(56, 78, 90);
        settings.lyric.font_valid = true;
        settings.lyric.font.lfHeight = -20;
        settings.lyric.font.lfWeight = FW_NORMAL;
        wcscpy_s(settings.lyric.font.lfFaceName, L"SimSun");
        ui::PlayerWindow player(settings);
        player.lyric_window_ = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
            0, 0, 200, 35, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(player.lyric_window_ != nullptr, "mini test parent creation failed");
        try {
            player.lyric_control_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
                0, 0, 200, 35, player.lyric_window_, nullptr, GetModuleHandleW(nullptr), nullptr);
            Require(player.lyric_control_ != nullptr, "mini test child creation failed");
            const char* variants[] = {
                "",
                "<mini_lyric Font=\"-12,0,0,0,400,0,0,0,1,0,0,4,0,SimSun\" "
                "TextColor=\"#646464\" HilightColor=\"#646464\" BkgndColor=\"#f5f5f7\" padding=\"6,6,6,6\"/>",
                "<mini_lyric Font=\"invalid\" TextColor=\"invalid\" padding=\"-1,2,3,4\"/>"};
            for (int variant = 0; variant < 3; ++variant) {
                {
                    std::ofstream xml(fixture / L"Skin.xml", std::ios::binary);
                    xml << "<skin><player_window image=\"background.bmp\"/>"
                           "<mini_window image=\"background.bmp\"/>"
                           "<lyric_window image=\"background.bmp\"><lyric position=\"8,8,192,27\"/>"
                           "<mini_border left_top_color=\"#323f6c\" right_bottom_color=\"#323f6c\"/>"
                        << variants[variant] << "</lyric_window></skin>";
                }
                player.skin_.emplace(skin::LegacySkin::Load(fixture));
                Require(player.skin_->Valid() && player.skin_->Lyric().valid,
                        "mini style fixture failed to parse");
                const bool custom = variant == 1;
                for (bool mini : {false, true, false, true}) {
                    player.mini_mode_ = mini;
                    player.RebuildLyricFont(false);
                    LOGFONTW font{};
                    Require(GetObjectW(player.lyric_font_, sizeof(font), &font) != 0,
                            "mini lyric font not rebuilt");
                    Require(font.lfHeight == (mini && custom ? -12 : -20),
                            "mini font leaked into normal mode or failed to apply");
                    Require(player.ActiveLyricTextColor() == (mini && custom ? RGB(100,100,100) : settings.lyric.text_color) &&
                            player.ActiveLyricHighlightColor() == (mini && custom ? RGB(100,100,100) : settings.lyric.highlight_color) &&
                            player.ActiveLyricBackgroundColor() == (mini && custom ? RGB(245,245,247) : settings.lyric.background_color),
                            "mini style override/fallback mismatch");
                    if (mini) {
                        const RECT expected = custom ? RECT{6,6,194,29} : RECT{2,2,196,33};
                        const RECT actual = player.LyricTextBounds();
                        Require(EqualRect(&actual, &expected), "mini padding changed legacy default");
                        Canvas paint(200,35); player.PaintLyricWindow(paint.dc);
                        Require(GetPixel(paint.dc, 1, 1) == player.ActiveLyricBackgroundColor() &&
                                GetPixel(paint.dc, 0, 0) == RGB(50,63,108),
                                "mini lyric frame/background mismatch");
                        MoveWindow(player.lyric_window_, 0, 0, 3, 3, FALSE);
                        const RECT tiny = player.LyricTextBounds();
                        Require(tiny.left <= tiny.right && tiny.top <= tiny.bottom &&
                                tiny.right <= 3 && tiny.bottom <= 3, "mini padding inverted small client");
                        MoveWindow(player.lyric_window_, 0, 0, 200, 35, FALSE);
                    }
                }
                player.fullscreen_lyric_detached_ = true;
                Require(player.ActiveLyricBackgroundColor() == settings.lyric.fullscreen_background_color &&
                        player.ActiveLyricTextColor() == settings.lyric.fullscreen_text_color,
                        "mini style overrides full-screen preferences");
                player.fullscreen_lyric_detached_ = false;
                player.settings_.lyric.transparent = true;
                player.ApplySkinWindowAlpha(player.lyric_window_, 255);
                COLORREF key{}; BYTE alpha{}; DWORD flags{};
                Require(GetLayeredWindowAttributes(player.lyric_window_, &key, &alpha, &flags) &&
                        (flags & LWA_COLORKEY) && key == player.ActiveLyricBackgroundColor(),
                        "mini lyric transparency key does not match its background");
                player.settings_.lyric.transparent = false;
                Require(player.settings_.lyric.font.lfHeight == -20 && player.settings_.lyric.background_color == RGB(56,78,90),
                        "mini style overwrote user settings");
            }
            player.DestroyLyricControls();
            DestroyWindow(player.lyric_window_); player.lyric_window_ = nullptr;
        } catch (...) {
            player.DestroyLyricControls();
            DestroyWindow(player.lyric_window_); player.lyric_window_ = nullptr;
            throw;
        }
        std::cout << "Mini lyric appearance: optional XML, legacy fallback, font/mode transitions, frame pixels passed\n";
    }
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
        fs::create_directories(output); SyntheticTests(output); AllPngSlots(output);
        testing::SkinRebindAccess::MiniLyricAppearance(output);
        testing::SkinRebindAccess::ToolbarLayout(output);
        testing::SkinRebindAccess::VolumeSlider(output);
        testing::SkinRebindAccess::PngPlaylist(output);
        testing::SkinRebindAccess::PngBackgrounds(output);
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
