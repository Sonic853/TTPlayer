#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "../app/resource_ids.h"

#include <gdiplus.h>
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
std::wstring Resource(HMODULE module, UINT id) {
    wchar_t value[1024]{};
    return {value, static_cast<size_t>(LoadStringW(module, id, value, 1024))};
}
std::wstring Label(HMENU menu, int position) {
    wchar_t value[1024]{};
    return {value, static_cast<size_t>(GetMenuStringW(menu, position, value, 1024, MF_BYPOSITION))};
}
struct Module {
    HMODULE value{};
    ~Module() { if (value) FreeLibrary(value); }
};
struct Window {
    HWND value{};
    ~Window() { if (value && IsWindow(value)) DestroyWindow(value); }
};
struct Menu {
    HMENU value{};
    ~Menu() { if (value) DestroyMenu(value); }
};
void VerifySingleBarPresentation(HWND bar) {
    // Record drawing directed at the destination DC. Background clearing and
    // per-button PNG drawing must happen offscreen, leaving only the final
    // SRCCOPY in this metafile, not a series of partially composed frames.
    const HDC dc = CreateEnhMetaFileW(nullptr,nullptr,nullptr,nullptr);
    Require(dc != nullptr,"cannot record toolbar presentation");
    SendMessageW(bar,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
    const HENHMETAFILE metafile = CloseEnhMetaFile(dc);
    Require(metafile != nullptr,"cannot finish toolbar presentation recording");
    struct Recording { int copies{}, intermediate{}; } recording;
    const BOOL enumerated = EnumEnhMetaFile(nullptr,metafile,
        [](HDC,HANDLETABLE*,const ENHMETARECORD* record,int,LPARAM data)->int {
            auto& result = *reinterpret_cast<Recording*>(data);
            switch (record->iType) {
            case EMR_BITBLT:
                if (reinterpret_cast<const EMRBITBLT*>(record)->dwRop == SRCCOPY) ++result.copies;
                else ++result.intermediate;
                break;
            case EMR_RECTANGLE: case EMR_STRETCHBLT: case EMR_STRETCHDIBITS:
            case EMR_ALPHABLEND: case EMR_TRANSPARENTBLT: case EMR_GDICOMMENT:
                ++result.intermediate; break;
            default: break;
            }
            return 1;
        },reinterpret_cast<LPVOID>(&recording),nullptr);
    DeleteEnhMetaFile(metafile);
    Require(enumerated && recording.copies == 1 && recording.intermediate == 0,
            "toolbar exposes background/per-button drawing instead of one complete frame");
}
void NativeHoverTests(ui::DesktopLyricsWindow& desktop) {
    Require(!(GetAsyncKeyState(VK_LBUTTON)&0x8000) && !(GetAsyncKeyState(VK_RBUTTON)&0x8000),
            "mouse is in use; retry the native hover test when idle");
    struct PaintCount { int count{}; RECT area{}; } paints;
    struct MoveCount { int count{}; std::vector<std::pair<UINT,LPARAM>> log; };
    struct HostState {
        POINT cursor{};
        HWND foreground=GetForegroundWindow(), bar{};
        ~HostState() {
            if (bar && IsWindow(bar)) RemoveWindowSubclass(bar,CountPaint,83);
            SetCursorPos(cursor.x,cursor.y);
            if (IsWindow(foreground)) SetForegroundWindow(foreground);
        }
        static LRESULT CALLBACK CountPaint(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
            if (message==WM_PAINT) {
                auto& paints=*reinterpret_cast<PaintCount*>(data);
                ++paints.count;
                GetUpdateRect(window,&paints.area,FALSE);
            }
            return DefSubclassProc(window,message,wp,lp);
        }
        static LRESULT CALLBACK CountMove(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
            const auto result=DefSubclassProc(window,message,wp,lp);
            auto& moves=*reinterpret_cast<MoveCount*>(data);
            if (message==WM_MOUSEMOVE) ++moves.count;
            if (message==WM_MOUSEMOVE || message==WM_MOUSELEAVE) moves.log.emplace_back(message,lp);
            return result;
        }
    } host;
    GetCursorPos(&host.cursor);
    const auto pump=[] {
        MSG msg{};
        while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        MsgWaitForMultipleObjects(0,nullptr,FALSE,3,QS_ALLINPUT);
        while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    };
    desktop.Show(true);
    const HWND bar=desktop.BarHandle(), control=desktop.ControlHandle();
    RECT bounds{}; GetWindowRect(control,&bounds);
    SetCursorPos(bounds.left+40,bounds.top+20);
    SendMessageW(control,WM_SETCURSOR,reinterpret_cast<WPARAM>(control),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
    int visited{};
    Require(SetWindowSubclass(bar,HostState::CountPaint,83,reinterpret_cast<DWORD_PTR>(&paints))!=FALSE,
            "cannot count native toolbar paints");
    host.bar=bar;
    for (HWND child=GetWindow(bar,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        RECT rect{}; GetWindowRect(child,&rect);
        if (rect.right-rect.left<3 || rect.bottom-rect.top<3) continue;
        const POINT point{(rect.left+rect.right)/2,(rect.top+rect.bottom)/2};
        MoveCount moves;
        Require(SetWindowSubclass(child,HostState::CountMove,84,reinterpret_cast<DWORD_PTR>(&moves))!=FALSE,
                "cannot observe native hover entry");
        struct MoveProbe {
            HWND child;
            ~MoveProbe() { if (IsWindow(child)) RemoveWindowSubclass(child,HostState::CountMove,84); }
        } probe{child};
        SetCursorPos(point.x,point.y);
        // SetCursorPos/WindowFromPoint do not imply WM_MOUSEMOVE has reached
        // the child. Wait for the production handler before taking the paint
        // baseline; otherwise the initial hot transition is counted as flicker.
        const auto deadline=GetTickCount64()+1000;
        while (!moves.count && GetTickCount64()<deadline) pump();
        Require(moves.count>0,"native hover entry was not delivered");
        UpdateWindow(bar); pump();
        Require(WindowFromPoint(point)==child,"native hover point does not reach the desktop button");
        const int initial=paints.count;
        for (int i=0;i<20;++i) {
            SetCursorPos(point.x-(i%2),point.y); pump(); UpdateWindow(bar);
        }
        if (paints.count!=initial) {
            POINT cursor{}; GetCursorPos(&cursor);
            std::cerr<<"hover repaint: command="<<GetDlgCtrlID(child)<<" count="<<paints.count-initial
                     <<" region="<<paints.area.left<<','<<paints.area.top<<','<<paints.area.right<<','<<paints.area.bottom
                     <<" cursor="<<cursor.x<<','<<cursor.y<<" child="<<(WindowFromPoint(cursor)==child)<<'\n';
            for (const auto& [message,lp]:moves.log)
                std::cerr<<" msg="<<message<<" point="<<static_cast<short>(LOWORD(lp))<<','<<static_cast<short>(HIWORD(lp));
            std::cerr<<'\n';
        }
        Require(paints.count==initial,"native movement inside one button repeatedly repaints the toolbar");
        Require(IsWindowVisible(bar)!=FALSE,"toolbar disappeared while hovering a button");
        ++visited;
    }
    Require(visited>=3,"native hover test did not exercise enough buttons");
    desktop.Show(false);
    std::cout << "native hover: " << visited << " controls, no repeated paints within a control\n";
}
struct ToolSearch { HWND bar{}, tooltip{}; };
BOOL CALLBACK FindTip(HWND window, LPARAM data) {
    auto& search = *reinterpret_cast<ToolSearch*>(data);
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    if (GetWindow(window, GW_OWNER) == search.bar && std::wstring_view(name) == TOOLTIPS_CLASSW) {
        search.tooltip = window; return FALSE;
    }
    return TRUE;
}
std::wstring QueryTip(HWND tooltip, HWND bar, UINT command) {
    const HWND control = GetDlgItem(bar, command);
    Require(control != nullptr, "desktop control missing");
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFO_V1_SIZE;
    tool.hwnd = bar; tool.uId = reinterpret_cast<UINT_PTR>(control);
    Require(SendMessageW(tooltip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)) != 0,
            "control is not registered with tooltip");
    Require((tool.uFlags & TTF_IDISHWND) && tool.lpszText == LPSTR_TEXTCALLBACKW,
            "tooltip is not a dynamic HWND callback");
    wchar_t text[1024]{};
    tool.lpszText = text;
    SendMessageW(tooltip, TTM_GETTEXTW, 1024, reinterpret_cast<LPARAM>(&tool));
    return text;
}
HMENU TrackSubmenu(HMENU menu) {
    if (GetMenuItemID(menu, 0) == 0x7ef4) return menu;
    for (int index = 0; index < GetMenuItemCount(menu); ++index)
        if (HMENU child = GetSubMenu(menu, index))
            if (HMENU found = TrackSubmenu(child)) return found;
    return nullptr;
}

void ToolbarTests(HMODULE resources) {
    const fs::path directory = fs::temp_directory_path() /
        (L"TTPlayer-Desktop-Toolbar-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(directory);
    ULONG_PTR token{};
    Gdiplus::GdiplusStartupInput startup;
    Require(Gdiplus::GdiplusStartup(&token, &startup, nullptr) == Gdiplus::Ok,
            "toolbar GDI+ startup failed");
    struct Runtime { ULONG_PTR token; ~Runtime() { Gdiplus::GdiplusShutdown(token); } } runtime{token};
    UINT count{}, length{};
    Gdiplus::GetImageEncodersSize(&count, &length);
    std::vector<unsigned char> codecs(length);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(codecs.data());
    Gdiplus::GetImageEncoders(count, length, encoders);
    CLSID png{};
    bool found{};
    for (UINT i = 0; i < count; ++i) if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
        png = encoders[i].Clsid; found = true; break;
    }
    Require(found, "toolbar PNG encoder missing");
    Gdiplus::Bitmap background(330, 32, PixelFormat32bppARGB);
    { Gdiplus::Graphics graphics(&background); graphics.Clear(Gdiplus::Color(255, 50, 63, 108)); }
    Require(background.Save((directory/L"background.png").c_str(), &png) == Gdiplus::Ok, "cannot write toolbar background");
    Gdiplus::Bitmap sprite(16, 4, PixelFormat32bppARGB);
    for (int y=0; y<4; ++y) for (int x=0; x<16; ++x) {
        const int frame=x/4;
        const std::array<Gdiplus::Color,4> states{Gdiplus::Color(128,255,255,255),
            Gdiplus::Color(128,0,255,0),Gdiplus::Color(128,255,0,0),Gdiplus::Color(128,0,0,255)};
        sprite.SetPixel(x, y, x%4<2 ? Gdiplus::Color(0,0,0,0) : states[frame]);
    }
    Require(sprite.Save((directory/L"sprite.png").c_str(), &png) == Gdiplus::Ok, "cannot write toolbar sprite");
    {
        std::ofstream xml(directory/L"Skin.xml", std::ios::binary);
        xml << R"(<skin version="2" name="Toolbar fixture"><player_window image="background.png"/>
<desklrc_bar image="background.png"><settings position="80,5,84,9" image="sprite.png"/>
<zoomin position="20,5,24,9" image="sprite.png"/><zoomout position="30,5,34,9" image="sprite.png"/>
</desklrc_bar></skin>)";
    }
    auto skin = skin::LegacySkin::Load(directory);
    Require(skin.DesktopLyricBar().zoom_in.image.IsGdiPlus() &&
            skin.DesktopLyricBar().zoom_out.bounds.left == 30, "optional toolbar XML not parsed");
    settings::DesktopLyricSettings settings;
    settings.font.lfHeight = -40;
    RECT persisted{100,100,700,166};
    Window owner{CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Toolbar fixture",
        WS_POPUP,0,0,330,32,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr)};
    Require(owner.value != nullptr,"cannot create toolbar owner");
    ui::DesktopLyricsWindow desktop;
    bool profile{};
    Require(desktop.Create(GetModuleHandleW(nullptr), owner.value, nullptr, resources,
        &settings, &persisted, [&](HMENU menu, POINT, HWND) {
            profile = GetMenuItemID(menu,0) == 0x80a2; return UINT(0);
        }), "cannot create toolbar test");
    desktop.SetSkin(&skin);
    const HWND bar = desktop.BarHandle();
    ShowWindow(bar,SW_SHOWNOACTIVATE);
    const HWND plus = GetDlgItem(bar, IDC_DESKTOP_LYRIC_ZOOM_IN);
    const auto visible = [](HWND w) { return (GetWindowLongPtrW(w,GWL_STYLE)&WS_VISIBLE)!=0; };
    Require(visible(plus) && !visible(GetDlgItem(bar,0x803e)), "omitted toolbar control left a ghost hit target");
    ToolSearch tip{bar};
    EnumThreadWindows(GetCurrentThreadId(),FindTip,reinterpret_cast<LPARAM>(&tip));
    Require(tip.tooltip && SendMessageW(tip.tooltip,TTM_GETTOOLCOUNT,0,0)==4,
            "toolbar should register only icon and three visible controls");
    Require(QueryTip(tip.tooltip,bar,IDC_DESKTOP_LYRIC_ZOOM_IN)==L"放大歌词" &&
            QueryTip(tip.tooltip,bar,IDC_DESKTOP_LYRIC_ZOOM_OUT)==L"缩小歌词", "zoom tooltip mismatch");

    struct Surface {
        HDC dc=CreateCompatibleDC(nullptr); HBITMAP bitmap{}; HGDIOBJ old{};
        Surface() {
            BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=330; info.bmiHeader.biHeight=-32;
            info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; void* pixels{};
            bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
            Require(dc&&bitmap,"toolbar surface allocation failed"); old=SelectObject(dc,bitmap);
        }
        ~Surface() { SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); }
    } surface;
    const auto paint = [&] { SendMessageW(bar,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(surface.dc),PRF_CLIENT); GdiFlush(); };
    VerifySingleBarPresentation(bar);
    paint();
    Require(GetPixel(surface.dc,20,5)==RGB(50,63,108), "transparent PNG painted black instead of background");
    const COLORREF normal=GetPixel(surface.dc,22,5);
    Require(GetRValue(normal)>140 && GetRValue(normal)<170 && GetBValue(normal)>170,
            "half-transparent PNG did not composite over toolbar");
    SendMessageW(plus,WM_MOUSEMOVE,0,MAKELPARAM(1,1)); paint();
    const COLORREF hot = GetPixel(surface.dc,22,5);
    Require(GetPixel(surface.dc,22,5)!=normal && GetPixel(surface.dc,20,5)==RGB(50,63,108),
            "hover frame/alpha was lost");
    RedrawWindow(bar,nullptr,nullptr,RDW_VALIDATE | RDW_ALLCHILDREN | RDW_NOERASE | RDW_NOINTERNALPAINT);
    Require(!GetUpdateRect(bar,nullptr,FALSE),"toolbar update rectangle was not validated");
    for (int i=0;i<100;++i) SendMessageW(plus,WM_MOUSEMOVE,0,MAKELPARAM(1+(i%2),1));
    Require(!GetUpdateRect(bar,nullptr,FALSE),"unchanged hover repeatedly invalidated the desktop toolbar");
    SendMessageW(plus,WM_MOUSELEAVE,0,0); paint();
    Require(GetPixel(surface.dc,22,5)==normal,"hover repaint accumulated alpha");
    ValidateRect(bar,nullptr);
    SendMessageW(plus,WM_MOUSELEAVE,0,0);
    Require(!GetUpdateRect(bar,nullptr,FALSE),"redundant mouse leave repainted the toolbar");
    SendMessageW(plus,WM_MOUSEMOVE,0,MAKELPARAM(1,1));
    RECT dirty{}; GetUpdateRect(bar,&dirty,FALSE);
    const RECT expected_dirty{20,5,24,9};
    Require(EqualRect(&dirty,&expected_dirty),"one button hover invalidated more than its rectangle");
    SendMessageW(plus,WM_MOUSELEAVE,0,0);
    const HWND minus = GetDlgItem(bar,IDC_DESKTOP_LYRIC_ZOOM_OUT);
    SendMessageW(minus,WM_MOUSEMOVE,0,MAKELPARAM(1,1));
    ValidateRect(bar,nullptr);
    SendMessageW(plus,WM_MOUSELEAVE,0,0); paint();
    Require(!GetUpdateRect(bar,nullptr,FALSE) && GetPixel(surface.dc,32,5)==hot,
            "late leave of previous button cleared or repainted the new hover");
    SendMessageW(minus,WM_MOUSELEAVE,0,0);
    SendMessageW(plus,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(1,1)); paint();
    const COLORREF pressed = GetPixel(surface.dc,22,5);
    Require(pressed != hot && pressed != normal,"pressed button frame missing");
    SendMessageW(plus,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(8,1)); paint();
    Require(GetCapture()==plus && GetPixel(surface.dc,22,5)==normal,
            "captured button stayed pressed outside its bounds");
    SendMessageW(plus,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(1,1)); paint();
    Require(GetPixel(surface.dc,22,5)==pressed,"captured button did not restore pressed frame on reentry");
    SendMessageW(plus,WM_CANCELMODE,0,0); paint();
    Require(GetCapture()!=plus && GetPixel(surface.dc,22,5)==hot,
            "cancelled button retained capture/pressed frame");
    SendMessageW(plus,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(1,1));
    SendMessageW(plus,WM_LBUTTONUP,0,MAKELPARAM(8,1)); paint();
    Require(settings.font.lfHeight==-40 && GetPixel(surface.dc,22,5)==normal,
            "release outside invoked zoom or left a hover frame");
    EnableWindow(plus,FALSE); paint();
    Require(GetPixel(surface.dc,22,5)!=normal,"disabled frame not repainted");
    EnableWindow(plus,TRUE); paint();
    Require(GetPixel(surface.dc,22,5)==normal,"enabled button retained disabled frame");
    for (int i=0;i<100;++i) {
        SendMessageW(plus,WM_MOUSEMOVE,0,MAKELPARAM(1,1)); paint();
        Require(GetPixel(surface.dc,22,5)==hot,"repeated hover accumulated PNG alpha");
        SendMessageW(plus,WM_MOUSELEAVE,0,0); paint();
        Require(GetPixel(surface.dc,22,5)==normal,"repeated leave lost background pixels");
    }
    NativeHoverTests(desktop);
    SendMessageW(bar,WM_COMMAND,IDC_DESKTOP_LYRIC_ZOOM_IN,0);
    Require(settings.font.lfHeight==-42 && settings.font_valid,"A+ did not change persistent font");
    SendMessageW(bar,WM_COMMAND,IDC_DESKTOP_LYRIC_ZOOM_OUT,0);
    Require(settings.font.lfHeight==-40,"A- did not restore font size");
    for(int i=0;i<50;i++) SendMessageW(bar,WM_COMMAND,IDC_DESKTOP_LYRIC_ZOOM_IN,0);
    Require(settings.font.lfHeight==-96,"zoom upper limit missing");
    for(int i=0;i<60;i++) SendMessageW(bar,WM_COMMAND,IDC_DESKTOP_LYRIC_ZOOM_OUT,0);
    Require(settings.font.lfHeight==-12,"zoom lower limit missing");
    SendMessageW(bar,WM_COMMAND,0x803f,0);
    Require(profile,"grid button did not open preset profiles");
    desktop.SetSkin(nullptr);
    Require(!visible(plus) && visible(GetDlgItem(bar,0x803e)) &&
            SendMessageW(tip.tooltip,TTM_GETTOOLCOUNT,0,0)==12,"legacy toolbar fallback changed");
    desktop.SetSkin(&skin);
    Require(visible(plus) && !visible(GetDlgItem(bar,0x803e)),"toolbar rebind visibility failed");
    std::cout << "desktop toolbar PNG alpha/hover, optional controls, preset mapping and font zoom passed\n";
}
} // namespace

namespace ttplayer::testing {
struct SkinRebindAccess {
    static inline UINT dispatched{};
    static inline int initialized_track_count{};
    static LRESULT CALLBACK Owner(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* player = reinterpret_cast<ui::PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            player = static_cast<ui::PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(player));
        }
        if (message == WM_COMMAND) {
            // Verify the returned menu ID reaches the main window without
            // opening a real decoder, playing audio or editing user data.
            dispatched = LOWORD(wp); return 0;
        }
        if (player && message == WM_INITMENUPOPUP) {
            const auto menu = reinterpret_cast<HMENU>(wp);
            const UINT first = GetMenuItemID(menu, 0);
            const auto result = player->HandleMessage(message, wp, lp);
            if (first == 0x7ef4 || first == 10000) initialized_track_count = GetMenuItemCount(menu);
            return result;
        }
        if (player && (message == WM_MEASUREITEM || message == WM_DRAWITEM))
            return player->HandleMessage(message, wp, lp);
        return DefWindowProcW(window, message, wp, lp);
    }
    static void Run(HMODULE resources) {
        settings::Settings settings;
        settings.general.send_title_to_msn = false;
        settings.general.tray_icon = false;
        settings.general.fade_windows = false;
        settings.lyric.auto_download = false;
        settings.playlist.tag_title_format = L"%T";
        ui::PlayerWindow p(settings);
        p.SetSkinResourceModule(resources);
        p.instance_ = GetModuleHandleW(nullptr);
        WNDCLASSEXW type{sizeof(type)};
        type.hInstance = p.instance_; type.lpfnWndProc = Owner;
        type.lpszClassName = L"TTPlayerDesktopMenuTest";
        Require(RegisterClassExW(&type) != 0, "cannot register fixture window");
        Window owner{CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Desktop menu test",
            WS_POPUP, 0, 0, 327, 141, nullptr, nullptr, p.instance_, &p)};
        Require(owner.value != nullptr, "cannot create fixture window");
        p.window_ = owner.value;
        p.playlists_.NewList(L"Test playlist");
        UINT selection{};
        int opens{};
        bool native_tracking{};
        std::function<void(HMENU)> verify;
        auto tracker = [&](HMENU menu, POINT point, HWND bar) {
            Require(bar == p.desktop_lyrics_.BarHandle(), "menu owner is not DeskLrcBar");
            Require(GetMenuItemID(menu, 0) == 0x7ef4, "resource track placeholder changed");
            // Exercise the same ordering as PlayerWindow's real tracker:
            // style the skeleton, then let the bar forward WM_INITMENUPOPUP.
            p.BeginPopupMenuStyle(menu, true);
            if (native_tracking) {
                // Real Win32 modal menu: no synthetic INIT notification. A
                // thread timer ends only our test popup without user input.
                const UINT_PTR timer = SetTimer(nullptr, 0, 150,
                    [](HWND, UINT, UINT_PTR, DWORD) { EndMenu(); });
                Require(timer != 0, "cannot install menu timeout");
                TrackPopupMenuEx(menu, TPM_RETURNCMD,
                                 point.x, point.y, bar, nullptr);
                KillTimer(nullptr, timer);
            } else {
                SendMessageW(bar, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0);
            }
            for (int index = 0; index < GetMenuItemCount(menu); ++index) {
                MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_FTYPE | MIIM_DATA;
                Require(GetMenuItemInfoW(menu, index, TRUE, &item) &&
                    (item.fType & MFT_OWNERDRAW) && p.FindPopupMenuItem(item.dwItemData),
                    "dynamic menu row lost owner-draw style");
            }
            p.EndPopupMenuStyle();
            if (verify) verify(menu);
            ++opens;
            return selection;
        };
        RECT saved{100, 150, 740, 216};
        try {
            Require(p.desktop_lyrics_.Create(p.instance_, owner.value, owner.value, resources,
                &p.settings_.desktop_lyric, &saved, tracker), "cannot create desktop lyrics");
            const HWND bar = p.desktop_lyrics_.BarHandle();
            ToolSearch search{bar};
            EnumThreadWindows(GetCurrentThreadId(), FindTip, reinterpret_cast<LPARAM>(&search));
            Require(search.tooltip && SendMessageW(search.tooltip, TTM_GETTOOLCOUNT, 0, 0) == 12,
                    "expected 12 desktop tooltip registrations");
            Require(SendMessageW(search.tooltip, TTM_GETMAXTIPWIDTH, 0, 0) == -1,
                    "desktop tooltip has non-native wrapping width");
            Require((GetWindowLongPtrW(search.tooltip, GWL_STYLE) & (TTS_ALWAYSTIP | TTS_NOPREFIX)) == 0,
                    "desktop tooltip has non-native style");
            for (const auto command : {0x7dd8U, 0x7d05U, 0x7d06U, 0x803eU, 0x803fU,
                                        0x8040U, 0x8038U, 8U}) {
                auto expected = Resource(resources, command);
                if (auto split = expected.find(L'\n'); split != std::wstring::npos)
                    expected.erase(0, split + 1);
                Require(!expected.empty() && QueryTip(search.tooltip, bar, command) == expected,
                        "static resource tip differs from 5.7.9");
            }
            for (const bool playing : {false, true, false}) {
                p.desktop_lyrics_.UpdatePlayback(std::chrono::milliseconds(0), playing);
                Require(QueryTip(search.tooltip, bar, 0x7d00) == (playing ? L"暂停" : L"播放"),
                        "play/pause tooltip did not follow playback state");
            }
            for (const int lines : {1, 2, 1}) {
                p.settings_.desktop_lyric.lines = lines;
                Require(QueryTip(search.tooltip, bar, 0x8d1) == (lines == 1 ? L"双行显示" : L"单行显示"),
                        "line tooltip did not describe the next action");
            }
            for (const bool enabled : {false, true, false}) {
                p.settings_.desktop_lyric.karaoke_mode = enabled;
                p.settings_.desktop_lyric.topmost = enabled;
                Require(QueryTip(search.tooltip, bar, 0x866) == (enabled ? L"非卡拉OK模式" : L"卡拉OK模式"),
                        "karaoke tooltip did not split resource state");
                Require(QueryTip(search.tooltip, bar, 0x8042) == (enabled ? L"取消总在最前" : L"总在最前"),
                        "topmost tooltip always advertised cancellation");
            }
            NMTTDISPINFOA ansi{};
            ansi.hdr = {search.tooltip, reinterpret_cast<UINT_PTR>(GetDlgItem(bar, 0x803e)), TTN_GETDISPINFOA};
            SendMessageW(bar, WM_NOTIFY, ansi.hdr.idFrom, reinterpret_cast<LPARAM>(&ansi));
            wchar_t converted[80]{};
            MultiByteToWideChar(CP_ACP, 0, ansi.lpszText, -1, converted, 80);
            Require(std::wstring_view(converted) == L"播放曲目", "ANSI tooltip notification failed");
            std::cout << "12 resource tips, dynamic actions, ANSI/Unicode and native tooltip style passed\n";

            verify = [&](HMENU menu) {
                Require(GetMenuItemCount(menu) == 1 && GetMenuItemID(menu, 0) == 0x7ef4 &&
                    Label(menu, 0) == Resource(resources, 0x7ef4) &&
                    (GetMenuState(menu, 0, MF_BYPOSITION) & MF_DISABLED), "empty list placeholder differs");
            };
            SendMessageW(bar, WM_COMMAND, 0x803e, 0);
            for (int index = 0; index < 12; ++index) {
                playlist::Track track;
                track.path = L"test.wav"; track.title = "Song " + std::to_string(index + 1);
                track.metadata.emplace_back("Title", track.title);
                track.duration_ms = index == 1 ? -1 : 61000;
                p.ActivePlaylist().Add(std::move(track));
            }
            p.current_ = 1; p.playing_playlist_index_ = p.playlists_.ActiveIndex();
            verify = [&](HMENU menu) {
                Require(GetMenuItemCount(menu) == 12 && GetMenuItemID(menu, 0) == 10000 &&
                    GetMenuItemID(menu, 11) == 10011, "track menu was not filled from active list");
                Require(Label(menu, 0) == L"1.  Song 1\t[1:01]" && Label(menu, 1) == L"2.  Song 2" &&
                    Label(menu, 11) == L"12. Song 12\t[1:01]", "track number/title/duration formatting differs");
                Require((GetMenuState(menu, 1, MF_BYPOSITION) & MF_CHECKED) &&
                    !(GetMenuState(menu, 0, MF_BYPOSITION) & MF_CHECKED), "playing item marker differs");
            };
            selection = 10001; dispatched = 0;
            SendMessageW(bar, WM_COMMAND, 0x803e, 0);
            Require(dispatched == 10001, "selected track was not routed to main player");
            // The main context menu now uses the same lazy entry point.
            Menu main{p.BuildContextMenu()};
            const HMENU tracks = TrackSubmenu(main.value);
            Require(tracks != nullptr, "main menu track placeholder missing");
            p.HandleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(tracks), 0);
            p.EndPopupMenuStyle();
            verify(tracks);
            p.settings_.playlist.title_number = false;
            p.playlists_.NewList(L"Other list");
            // A playing index in a different playlist must not check this row.
            playlist::Track other; other.path = L"other.wav"; other.title = "Other";
            other.metadata.emplace_back("Title", other.title);
            other.duration_ms = -1; p.ActivePlaylist().Add(std::move(other));
            verify = [&](HMENU menu) {
                Require(GetMenuItemCount(menu) == 1 && Label(menu, 0) == L"Other" &&
                    !(GetMenuState(menu, 0, MF_BYPOSITION) & MF_CHECKED), "menu did not refresh after list change");
            };
            selection = 0; SendMessageW(bar, WM_COMMAND, 0x803e, 0);
            Require(opens == 3, "desktop list button did not invoke tracker each time");

            // Let TrackPopupMenuEx itself drive the forwarded notification.
            native_tracking = true;
            SendMessageW(bar, WM_COMMAND, 0x803e, 0);
            Require(opens == 4, "real Win32 popup did not open");
            native_tracking = false;
            p.settings_.general.menu_bar_playlist = true;
            for (int index = 1; index < 80; ++index) {
                playlist::Track item; item.path = L"entry.wav";
                p.ActivePlaylist().Add(std::move(item));
            }
            // Check optional column breaks without exhausting the system's
            // desktop menu heap with tens of thousands of native items.
            Menu large{CreatePopupMenu()};
            p.PopulateTrackMenu(large.value);
            Require(GetMenuItemCount(large.value) == 80 && GetMenuItemID(large.value, 79) == 10079,
                    "track command IDs lost their playlist indices");
            RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            const int rows = std::max(20L, (work.bottom - work.top) / 22);
            Require((GetMenuState(large.value, rows, MF_BYPOSITION) & MF_MENUBARBREAK) &&
                !(GetMenuState(large.value, rows - 1, MF_BYPOSITION) & MF_MENUBARBREAK),
                "multi-column track menu did not follow the configured row limit");
            // Use the application's actual popup callback too, so adding
            // TPM_NONOTIFY there cannot pass just a synthetic message test.
            p.desktop_lyrics_.Destroy();
            p.lyric_window_ = owner.value;
            Require(p.CreateDesktopLyrics(), "production desktop callback fixture failed");
            initialized_track_count = 0;
            const UINT_PTR timer = SetTimer(nullptr, 0, 150,
                [](HWND, UINT, UINT_PTR, DWORD) { EndMenu(); });
            Require(timer != 0, "production menu timeout failed");
            SendMessageW(p.desktop_lyrics_.BarHandle(), WM_COMMAND, 0x803e, 0);
            KillTimer(nullptr, timer);
            Require(initialized_track_count == 80 && !p.context_menu_open_,
                    "production popup suppressed initialization or left menu state active");
            std::cout << "empty/populated/refreshed menus, shared initialization, styling and dispatch passed\n";
            std::cout << "real TrackPopupMenuEx, production callback and optional menu columns passed\n";
            p.desktop_lyrics_.Destroy(); p.window_ = p.lyric_window_ = nullptr;
        } catch (...) {
            p.EndPopupMenuStyle(); p.desktop_lyrics_.Destroy(); p.window_ = p.lyric_window_ = nullptr;
            throw;
        }
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2, "expected repository root");
        const auto path = fs::path(argv[1]) / L"ttpres.dll";
        if (!fs::exists(path)) { std::cout << "5.7.9 ttpres.dll fixture unavailable\n"; return 77; }
        Require(SUCCEEDED(OleInitialize(nullptr)), "OLE init failed");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&controls) != FALSE, "common controls init failed");
        Module resources{LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)};
        Require(resources.value != nullptr, "cannot load resource-only DLL");
        testing::SkinRebindAccess::Run(resources.value);
        ToolbarTests(resources.value);
        {
            // Also verify the native 5.7.9-era BMP toolbar, not just PNG fixtures.
            auto skin=skin::LegacySkin::Load(fs::path(argv[1])/L"reverse/semantic/TT2012.extracted");
            Window owner{CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"BMP toolbar fixture",WS_POPUP,
                0,0,231,25,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr)};
            settings::DesktopLyricSettings options; RECT bounds{100,100,700,166};
            ui::DesktopLyricsWindow desktop;
            Require(desktop.Create(GetModuleHandleW(nullptr),owner.value,nullptr,resources.value,&options,&bounds),
                    "BMP toolbar fixture creation failed");
            desktop.SetSkin(&skin);
            VerifySingleBarPresentation(desktop.BarHandle());
            NativeHoverTests(desktop);
        }
        OleUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "desktop lyric menu test: " << error.what() << '\n'; return 1;
    }
}
