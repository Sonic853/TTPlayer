#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
struct Canvas {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap{}; HGDIOBJ old{}; uint32_t* pixels{};
    int width, height;
    Canvas(int w, int h) : width(w), height(h) {
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
        Require(dc && bitmap && pixels,"DIB creation failed"); old = SelectObject(dc,bitmap);
    }
    ~Canvas() { SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); }
    std::vector<uint32_t> Copy() const { GdiFlush(); return {pixels,pixels+static_cast<size_t>(width)*height}; }
};
struct Window { HWND value{}; ~Window() { if (IsWindow(value)) DestroyWindow(value); } };
uint32_t Over(uint32_t pixel, COLORREF background) {
    const unsigned keep = 255-(pixel>>24);
    const auto channel=[&](int shift, unsigned c) { return ((pixel>>shift)&255)+((c*keep+127)/255); };
    return 0xff000000U | (channel(16,GetRValue(background))<<16) |
        (channel(8,GetGValue(background))<<8) | channel(0,GetBValue(background));
}
void CheckAlpha(const std::vector<uint32_t>& frame) {
    size_t edge{},solid{},blank{};
    for (const auto pixel:frame) {
        const auto a=pixel>>24;
        edge+=a>0 && a<255; solid+=a==255; blank+=a==0;
        Require(((pixel>>16)&255)<=a && ((pixel>>8)&255)<=a && (pixel&255)<=a,
            "glyph channels are not premultiplied (fringe risk)");
    }
    Require(edge>100 && solid>100 && blank>1000,"missing grayscale edges, solid glyphs or transparent background");
    std::cout << "coverage: " << edge << " partial / " << solid << " solid / " << blank << " transparent\n";
}
void SavePng(const fs::path& file, const Canvas& canvas, COLORREF background) {
    using Microsoft::WRL::ComPtr;
    const auto ok=[](HRESULT hr) { Require(SUCCEEDED(hr),"PNG capture failed"); };
    auto pixels=canvas.Copy(); for(auto& p:pixels) p=Over(p,background);
    ComPtr<IWICImagingFactory> factory;
    ok(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream; ok(factory->CreateStream(&stream));
    ok(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; ok(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));
    ok(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; ok(encoder->CreateNewFrame(&frame,nullptr)); ok(frame->Initialize(nullptr));
    ok(frame->SetSize(canvas.width,canvas.height)); auto format=GUID_WICPixelFormat32bppBGRA;
    ok(frame->SetPixelFormat(&format));
    ok(frame->WritePixels(canvas.height,canvas.width*4,canvas.width*canvas.height*4,reinterpret_cast<BYTE*>(pixels.data())));
    ok(frame->Commit()); ok(encoder->Commit());
}
void Pump() {
    const auto end=GetTickCount64()+60;
    do {
        MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0,nullptr,FALSE,5,QS_ALLINPUT);
    } while(GetTickCount64()<end);
    DwmFlush();
}
}
namespace ttplayer::testing {
struct ProgressSeekAccess {
    static inline COLORREF background=RGB(24,48,80);
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        const auto p=reinterpret_cast<ui::PlayerWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(p) return p->HandleLyricControlMessage(hwnd,message,wp,lp);
        if(message==WM_PAINT) {
            PAINTSTRUCT paint{}; const auto dc=BeginPaint(hwnd,&paint);
            RECT client{}; GetClientRect(hwnd,&client); const auto brush=CreateSolidBrush(background);
            FillRect(dc,&client,brush); DeleteObject(brush); EndPaint(hwnd,&paint); return 0;
        }
        return DefWindowProcW(hwnd,message,wp,lp);
    }
    static void Run(HMODULE resources, const fs::path& capture) {
        settings::Settings settings;
        settings.general.tray_icon=settings.general.fade_windows=settings.general.send_title_to_msn=false;
        settings.lyric.auto_download=false;
        ui::PlayerWindow p(settings); p.instance_=GetModuleHandleW(nullptr); p.SetSkinResourceModule(resources);
        Require(p.LoadSkinResource(resources),"original 5.7.9 lyric skin unavailable");
        WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance=p.instance_; cls.lpfnWndProc=Proc;
        cls.lpszClassName=L"TTPlayerFullscreenRenderTest"; Require(RegisterClassExW(&cls)!=0,"test class unavailable");
        Window owner{CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,60,60,660,260,nullptr,nullptr,p.instance_,nullptr)};
        Window lyric{CreateWindowExW(0,cls.lpszClassName,L"",WS_CHILD,0,0,640,240,owner.value,nullptr,p.instance_,nullptr)};
        Require(owner.value && lyric.value,"test HWND unavailable");
        p.window_=owner.value; p.lyric_control_=lyric.value;
        SetWindowLongPtrW(lyric.value,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&p));
        struct Cleanup {
            ui::PlayerWindow& p; HWND lyric;
            ~Cleanup() {
                p.DestroyFullScreenLyricInput(); SetWindowLongPtrW(lyric,GWLP_USERDATA,0);
                p.window_=p.lyric_control_=nullptr; p.fullscreen_mode_=0; p.fullscreen_lyric_detached_=false;
            }
        } cleanup{p,lyric.value};
        p.lyrics_=lyrics::ParseLrc("[00:00.00]全屏歌词边缘 Aa 0123\n[00:10.00]悠悠回望 曾属于彼此的晚上\n[00:40.00]千千阙歌 · 陈慧娴\n");
        { std::scoped_lock lock(p.audio_.mutex_);
          p.audio_.state_=audio::PlaybackState::paused; p.audio_.position_ms_=23000;
          p.audio_.duration_ms_=60000; p.audio_.backend_=audio::AudioEngine::Backend::wave_out; }
        p.fullscreen_mode_=3; p.fullscreen_lyric_detached_=true;
        auto& options=p.settings_.lyric;
        options.fullscreen_auto_font=false; options.fullscreen_transparent=true;
        options.fullscreen_drag_lyric=false; options.fullscreen_row_interval=8;
        options.fullscreen_fade_highlight=false; options.fullscreen_karaoke_mode=false;
        options.fullscreen_font.lfHeight=-38; options.fullscreen_font.lfWeight=FW_NORMAL;
        options.fullscreen_font.lfQuality=CLEARTYPE_QUALITY;
        wcscpy_s(options.fullscreen_font.lfFaceName,L"Microsoft YaHei");
        const auto configured=options.fullscreen_font;
        p.RebuildLyricFont(false);
        LOGFONTW installed{}; GetObjectW(p.lyric_font_,sizeof(installed),&installed);
        Require(installed.lfQuality==ANTIALIASED_QUALITY,"fullscreen alpha font does not use grayscale antialiasing");
        Require(memcmp(&configured,&options.fullscreen_font,sizeof(configured))==0,"render quality mutated saved FontFS");
        Canvas canvas(640,240);
        const auto render=[&] { p.PaintLyricControl(lyric.value,canvas.dc,false); return canvas.Copy(); };
        for(int scroll:{0,1}) {
            options.fullscreen_scroll_mode=scroll;
            options.fullscreen_text_color=options.fullscreen_highlight_color=RGB(255,255,255);
            options.fullscreen_karaoke_mode=false;
            const auto base=render(); CheckAlpha(base);
            for(COLORREF color:{RGB(0,0,0),RGB(255,255,255),RGB(100,180,240)}) {
                options.fullscreen_text_color=options.fullscreen_highlight_color=color;
                options.fullscreen_background_color=color;
                const auto solid=render(); CheckAlpha(solid);
                for(size_t i=0;i<base.size();++i) Require((base[i]>>24)==(solid[i]>>24),"text color changed coverage");
                options.fullscreen_karaoke_mode=true;
                Require(render()==solid,"karaoke double-painted/removed antialiased glyph edges");
                options.fullscreen_karaoke_mode=false;
            }
            options.fullscreen_text_color=RGB(100,180,240); options.fullscreen_highlight_color=RGB(255,220,60);
            options.fullscreen_karaoke_mode=true;
            const auto karaoke=render();
            for(size_t i=0;i<base.size();++i) Require((base[i]>>24)==(karaoke[i]>>24),"karaoke split changed glyph coverage");
            options.fullscreen_background_color=RGB(240,20,170);
            Require(render()==karaoke,"transparent text contains color-key background fringe");
            p.lyric_line_dragging_=true; render();
            const auto guide=scroll ? canvas.pixels[10*640+320] : canvas.pixels[120*640+10];
            Require((guide>>24)==255,"drag seek guide lost opacity"); p.lyric_line_dragging_=false;
        }
        options.fullscreen_scroll_mode=0; options.fullscreen_karaoke_mode=false;
        options.fullscreen_text_color=options.fullscreen_highlight_color=RGB(220,235,255);
        const auto alpha=render();
        if(!capture.empty()) SavePng(capture,canvas,background);
        // Opaque/split fullscreen still honors FontFS, with the original
        // antialiased default; the extension must not force that path to 3.
        options.fullscreen_transparent=false;
        options.fullscreen_font.lfQuality=ANTIALIASED_QUALITY; p.RebuildLyricFont(false);
        options.fullscreen_text_color=options.fullscreen_highlight_color=RGB(255,255,255);
        options.fullscreen_background_color=RGB(0,0,0); options.fullscreen_fade_index=0;
        const auto opaque=render(); size_t opaque_edges{};
        for(const auto pixel:opaque) opaque_edges+=(pixel&0xffffffU)!=0 && (pixel&0xffffffU)!=0xffffffU;
        Require(opaque_edges>100,"opaque fullscreen lost antialiased edges");
        options.fullscreen_transparent=true; options.fullscreen_font=configured;
        options.fullscreen_text_color=options.fullscreen_highlight_color=RGB(220,235,255); p.RebuildLyricFont(false);
        const auto handles=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        {
            SetWindowPos(lyric.value,nullptr,0,0,1920,1200,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE);
            Canvas large(1920,1200); const auto start=std::chrono::steady_clock::now();
            for(int i=0;i<30;++i) p.PaintLyricControl(lyric.value,large.dc,false);
            GdiFlush();
            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            std::cout << "1920x1200 software paint: " << elapsed/30 << " ms/frame (30 frames)\n";
        }
        SetWindowPos(lyric.value,nullptr,0,0,640,240,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE);
        Require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<=handles+2,"repeated alpha painting leaked GDI objects");
        // Real DWM composition: sample partial glyph pixels over two live
        // backgrounds, without mouse input or altering any player/config.
        const HWND foreground=GetForegroundWindow();
        struct Focus { HWND window; ~Focus(){ if(IsWindow(window)) SetForegroundWindow(window); } } focus{foreground};
        SetWindowPos(owner.value,HWND_TOPMOST,60,60,660,260,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        p.fullscreen_lyric_detached_=false;
        p.DetachLyricControl({70,70,710,310},HWND_TOPMOST); Pump();
        Require(p.lyric_control_==lyric.value,"fullscreen alpha rendering recreated the HWND");
        for(const auto color:{RGB(24,48,80),RGB(180,120,60)}) {
            background=color; RedrawWindow(owner.value,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW); Pump();
            const auto screen=GetDC(nullptr); size_t checked{};
            bool pixels_match=true;
            for(size_t i=0;i<alpha.size() && checked<150;++i) {
                const auto a=alpha[i]>>24; if(a<20 || a>235) continue;
                const auto actual=GetPixel(screen,70+static_cast<int>(i%640),70+static_cast<int>(i/640));
                const auto expected=Over(alpha[i],color);
                pixels_match &= std::abs(int(GetRValue(actual))-int((expected>>16)&255))<=2 &&
                    std::abs(int(GetGValue(actual))-int((expected>>8)&255))<=2 &&
                    std::abs(int(GetBValue(actual))-int(expected&255))<=2;
                ++checked;
            }
            ReleaseDC(nullptr,screen);
            Require(checked==150 && pixels_match,"DWM pixels do not match alpha coverage over the live background");
        }
        for(int cycle=0;cycle<3;++cycle) {
            // Also migrate a previously color-keyed HWND without replacing it.
            SetLayeredWindowAttributes(lyric.value,RGB(0,0,0),255,LWA_COLORKEY);
            p.ApplyFullScreenLyricTransparency(); Pump();
            DWORD flags{};
            Require(!GetLayeredWindowAttributes(lyric.value,nullptr,nullptr,&flags),"stale color-key state survived alpha migration");
            options.fullscreen_transparent=false; p.RebuildLyricFont(false); p.ApplyFullScreenLyricTransparency();
            Require(!(GetWindowLongPtrW(lyric.value,GWL_EXSTYLE)&WS_EX_LAYERED),"opaque fullscreen retained alpha surface");
            options.fullscreen_transparent=true; p.RebuildLyricFont(false); p.ApplyFullScreenLyricTransparency(); Pump();
        }
        p.RestoreLyricControl();
        Require(p.lyric_control_==lyric.value && GetParent(lyric.value)==owner.value &&
            !(GetWindowLongPtrW(lyric.value,GWL_EXSTYLE)&WS_EX_LAYERED),"exit failed to restore the original child window");
        options.transparent=true; p.RebuildLyricFont(false); GetObjectW(p.lyric_font_,sizeof(installed),&installed);
        Require(installed.lfQuality==NONANTIALIASED_QUALITY,"ordinary color-key lyrics changed unexpectedly");
        std::cout << "grayscale/premultiplied pixels, black/same-background text, horizontal/vertical karaoke,\n"
                     "drag guide, DWM live backgrounds, alpha/opaque migration and HWND restore passed\n";
    }
};
}
int wmain(int argc,wchar_t** argv) {
    try {
        Require(argc>=2,"expected original resource directory"); SetProcessDPIAware();
        Require(SUCCEEDED(OleInitialize(nullptr)),"OLE initialization failed");
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        const auto resources=LoadLibraryExW((fs::path(argv[1])/L"ttpres.dll").c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Require(resources!=nullptr,"5.7.9 resources missing");
        testing::ProgressSeekAccess::Run(resources,argc>2?fs::path(argv[2]):fs::path{});
        FreeLibrary(resources); OleUninitialize(); return 0;
    } catch(const std::exception& e) { std::cerr << "fullscreen lyric render: " << e.what() << '\n'; return 1; }
}
