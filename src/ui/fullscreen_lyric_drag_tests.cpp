#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "../app/resource_ids.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <dwmapi.h>

namespace fs = std::filesystem;
using namespace ttplayer;
using namespace ttplayer::ui::detail;

namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Temp {
    fs::path path = fs::temp_directory_path() / (L"TTPlayer-fullscreen-drag-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Temp() { Require(fs::create_directory(path),"cannot create test directory"); }
    ~Temp() { std::error_code error; fs::remove_all(path,error); }
};
struct Window {
    HWND value{};
    ~Window() { if (value && IsWindow(value)) DestroyWindow(value); }
};
void XmlTests(const fs::path& folder) {
    const auto path = folder / L"settings.xml";
    Require(settings::Settings{}.lyric.fullscreen_drag_lyric, "new settings default must permit dragging");
    for (bool normal : {false,true}) {
        { std::ofstream stream(path); stream << "<ttplayer><Lyric DragLyric=\"" << normal << "\"/></ttplayer>"; }
        const auto old = settings::LoadLegacyXml(path);
        Require(old.lyric.drag_lyric == normal && old.lyric.fullscreen_drag_lyric == normal,
                "old shared drag preference not preserved");
        for (bool fullscreen : {false,true}) {
            auto input = old;
            input.lyric.fullscreen_drag_lyric = fullscreen;
            settings::SaveWindowState(path,input);
            const auto saved = settings::LoadLegacyXml(path);
            Require(saved.lyric.drag_lyric == normal && saved.lyric.fullscreen_drag_lyric == fullscreen,
                    "independent DragLyric/DragLyricFS values did not round-trip");
        }
    }
}
} // namespace

namespace ttplayer::testing {
// Existing AudioEngine/PlayerWindow friend seam; no sound device, song files,
// user config or real full-screen takeover is needed for these HWND messages.
struct ProgressSeekAccess {
    static inline HWND backing_window{};
    static inline COLORREF backing_color = RGB(80,120,160);
    static inline int background_clicks{}, popup_count{};
    static LRESULT CALLBACK Control(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_INITMENUPOPUP) ++popup_count;
        if (window == backing_window) {
            if (message == WM_LBUTTONUP) ++background_clicks;
            if (message == WM_PAINT) {
                PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window,&paint);
                RECT bounds{}; GetClientRect(window,&bounds);
                const HBRUSH brush = CreateSolidBrush(backing_color);
                FillRect(dc,&bounds,brush); DeleteObject(brush); EndPaint(window,&paint); return 0;
            }
        }
        auto* p = reinterpret_cast<ui::PlayerWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (p)
            return p->HandleLyricControlMessage(window,message,wp,lp);
        return DefWindowProcW(window,message,wp,lp);
    }
    static void Pump(DWORD duration = 40) {
        const auto end = GetTickCount64() + duration;
        do {
            MSG msg{};
            while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            MsgWaitForMultipleObjects(0,nullptr,FALSE,5,QS_ALLINPUT);
        } while (GetTickCount64() < end);
    }
    static void Button(DWORD flags) {
        INPUT event{}; event.type = INPUT_MOUSE; event.mi.dwFlags = flags;
        Require(SendInput(1,&event,sizeof(event)) == 1,"host SendInput failed");
        Pump();
    }
    static void CheckInputSurface(ui::PlayerWindow& p) {
        Require(!(GetAsyncKeyState(VK_LBUTTON) & 0x8000) && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000),
                "host mouse is in use; retry the input test when idle");
        struct RestoreCursor {
            POINT position{};
            HWND foreground = GetForegroundWindow();
            RestoreCursor() { GetCursorPos(&position); }
            ~RestoreCursor() {
                if (GetCapture()) { ReleaseCapture(); INPUT input{}; input.type=INPUT_MOUSE;
                    input.mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(1,&input,sizeof(input)); }
                SetCursorPos(position.x,position.y);
                if (IsWindow(foreground)) SetForegroundWindow(foreground);
            }
        } cursor;
        std::vector<RECT> work_areas;
        EnumDisplayMonitors(nullptr,nullptr,
            [](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
                MONITORINFO info{sizeof(info)};
                if (GetMonitorInfoW(monitor,&info)) reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcWork);
                return TRUE;
            },reinterpret_cast<LPARAM>(&work_areas));
        Require(!work_areas.empty(),"no desktop monitor available");
        Window backing{CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"TTPlayerFullscreenDragTest",L"Fullscreen input regression",WS_POPUP,
            0,0,440,220,nullptr,nullptr,p.instance_,nullptr)};
        Require(backing.value != nullptr,"native backing window unavailable");
        backing_window = backing.value; background_clicks = 0;
        p.mini_mode_ = false; p.fullscreen_mode_ = 3;
        p.settings_.lyric.fullscreen_transparent = true;
        p.settings_.lyric.fullscreen_drag_lyric = false;
        p.settings_.lyric.fullscreen_scroll_mode = 0;
        p.settings_.lyric.fullscreen_background_color = RGB(0,0,0);
        ResetClock(p,audio::PlaybackState::playing);
        const int left = work_areas.front().left + 50, top = work_areas.front().top + 50;
        SetWindowPos(backing.value,HWND_TOPMOST,left,top,440,220,SWP_NOACTIVATE | SWP_SHOWWINDOW);
        p.DetachLyricControl({left+20,top+20,left+420,top+200},HWND_TOPMOST);
        Pump(); DwmFlush();
        const auto blank = Point(p,false);
        POINT screen = blank; ClientToScreen(p.lyric_control_,&screen);
        Require(WindowFromPoint(screen) == backing.value,
                "fixture did not reproduce colour-key mouse fall-through");
        p.settings_.lyric.fullscreen_drag_lyric = true;
        p.UpdateFullScreenLyricInput(); Pump(); DwmFlush();
        Require(p.fullscreen_lyric_input_ && WindowFromPoint(screen) == p.fullscreen_lyric_input_,
                "transparent input region did not intercept the blank lyric area");
        const HWND input = p.fullscreen_lyric_input_;
        Require((GetWindowLongPtrW(input,GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0 &&
                (GetWindowLongPtrW(input,GWL_EXSTYLE) & WS_EX_LAYERED) == 0,
                "input surface has visible colour-key/alpha rendering");
        // Actual desktop pixels must match a changing surface underneath,
        // proving the proxy adds neither a tint nor a stale black rectangle.
        for (COLORREF color : {RGB(80,120,160),RGB(160,90,30)}) {
            backing_color = color;
            RedrawWindow(backing.value,nullptr,nullptr,RDW_INVALIDATE | RDW_UPDATENOW);
            Pump(); DwmFlush();
            const HDC dc = GetDC(nullptr); const COLORREF actual = GetPixel(dc,screen.x,screen.y); ReleaseDC(nullptr,dc);
            Require(actual == color,"input surface changed or obscured background pixels");
        }
        // Genuine OS-dispatched blank-area drag, not a synthetic SendMessage
        // to LyricCtrl (which bypasses the transparency hit test).
        for (const int mode : {1,3}) for (const int scroll : {0,1})
            for (const bool text : {false,true}) {
                p.fullscreen_mode_ = mode;
                p.settings_.lyric.fullscreen_scroll_mode = scroll;
                const auto state = text ? audio::PlaybackState::paused : audio::PlaybackState::playing;
                ResetClock(p,state);
                POINT begin = Point(p,text); ClientToScreen(p.lyric_control_,&begin);
                Require(WindowFromPoint(begin) == input,"lyric input HWND no longer owns the test point");
                SetCursorPos(begin.x,begin.y); Pump();
                Button(MOUSEEVENTF_LEFTDOWN);
                Require(p.lyric_line_dragging_ && GetCapture() == p.lyric_control_,"native lyric click did not start drag");
                SetCursorPos(begin.x-(scroll ? 18 : 0),begin.y-(scroll ? 0 : 18)); Pump();
                Require(p.lyric_line_drag_offset_ == -18,"native mouse move did not reach the captured lyric control");
                const HDC dc = GetDC(p.lyric_control_);
                const auto font = SelectObject(dc,p.lyric_font_);
                const auto target = p.LyricDragTime(dc,-18);
                SelectObject(dc,font); ReleaseDC(p.lyric_control_,dc);
                Require(target.has_value(),"native drag target unavailable");
                Button(MOUSEEVENTF_LEFTUP);
                VerifySeek(p,*target,state);
            }
        Require(background_clicks == 0,"blank-area drag clicked the underlying visual/desktop");
        // Right click in that same blank area must reach the lyrics menu.
        popup_count = 0; SetCursorPos(screen.x,screen.y); Pump();
        const auto timer = SetTimer(nullptr,0,180,[](HWND,UINT,UINT_PTR,DWORD){ EndMenu(); });
        Require(timer != 0,"menu auto-dismiss timer unavailable");
        Button(MOUSEEVENTF_RIGHTDOWN); Button(MOUSEEVENTF_RIGHTUP); KillTimer(nullptr,timer);
        Require(popup_count > 0,"blank-area right click missed the lyric menu");
        // Move/resize the same detached control on every attached monitor.
        // Its input HWND must follow, rather than retaining primary coordinates.
        for (const RECT work : work_areas) {
            const int x = work.left+50, y = work.top+50;
            SetWindowPos(backing.value,HWND_TOPMOST,x,y,480,240,SWP_NOACTIVATE);
            SetWindowPos(p.lyric_control_,HWND_TOPMOST,x+20,y+20,420,200,SWP_NOACTIVATE);
            Pump();
            RECT client{}; GetClientRect(p.lyric_control_,&client);
            MapWindowPoints(p.lyric_control_,nullptr,reinterpret_cast<POINT*>(&client),2);
            RECT area{}; GetWindowRect(input,&area);
            Require(p.fullscreen_lyric_input_ == input && EqualRect(&client,&area),"input surface did not follow lyric geometry");
            const POINT inside{client.left+2,client.top+2};
            Require(WindowFromPoint(inside) == input,"moved lyric region has stale input coordinates");
            const POINT outside{client.right+2,client.bottom+2};
            Require(WindowFromPoint(outside) != input,"input region exceeds lyric bounds");
        }
        // Do not intercept another application above the lyric stack.
        SetWindowPos(backing.value,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        RECT lyric_bounds{}; GetWindowRect(p.lyric_control_,&lyric_bounds);
        const POINT inside{lyric_bounds.left+2,lyric_bounds.top+2};
        p.UpdateFullScreenLyricInput(); Pump();
        Require(WindowFromPoint(inside) == backing.value,"input proxy raised itself above an unrelated foreground surface");
        p.settings_.lyric.fullscreen_drag_lyric = false; p.UpdateFullScreenLyricInput();
        Require(!p.fullscreen_lyric_input_ && !IsWindow(input),"disabled option left a mouse-blocking window");
        p.settings_.lyric.fullscreen_drag_lyric = true; p.UpdateFullScreenLyricInput();
        Require(p.fullscreen_lyric_input_ != nullptr,"input surface failed to reenable");
        ShowWindow(p.lyric_control_,SW_HIDE);
        Require(!p.fullscreen_lyric_input_,"hidden lyrics retained an invisible mouse blocker");
        ShowWindow(p.lyric_control_,SW_SHOWNOACTIVATE); p.UpdateFullScreenLyricInput();
        Require(p.fullscreen_lyric_input_ != nullptr,"shown lyrics lost their input region");
        p.settings_.lyric.fullscreen_transparent = false; p.ApplyFullScreenLyricTransparency();
        Require(!p.fullscreen_lyric_input_,"opaque lyrics retained an unnecessary overlay");
        p.settings_.lyric.fullscreen_transparent = true; p.ApplyFullScreenLyricTransparency();
        Require(p.fullscreen_lyric_input_ != nullptr,"transparent lyrics lost their input region");
        p.fullscreen_mode_ = 0; p.RestoreLyricControl();
        Require(!p.fullscreen_lyric_input_ && !p.fullscreen_lyric_detached_,"leaving fullscreen retained input surface");

        // Exercise the restored, real child HWND as a normal/mini lyric
        // control. No fullscreen proxy may be needed for text hit testing.
        SetWindowPos(p.window_,HWND_TOPMOST,left,top,440,220,SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(p.lyric_control_,nullptr,20,20,400,180,SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Pump();
        for (bool mini : {false,true}) for (int scroll : {0,1}) for (bool text : {false,true}) {
            p.mini_mode_ = mini;
            p.settings_.lyric.scroll_mode = p.settings_.lyric.mini_scroll_mode = scroll;
            const auto state = text ? audio::PlaybackState::paused : audio::PlaybackState::playing;
            ResetClock(p,state);
            const auto begin = Point(p,text);
            POINT location = begin; ClientToScreen(p.lyric_control_,&location);
            InvalidateRect(p.lyric_control_,nullptr,FALSE); Pump();
            Require(!p.fullscreen_lyric_input_ && WindowFromPoint(location) == p.lyric_control_,
                    "normal/mini lyric test point is not owned by the restored child");
            SetCursorPos(location.x,location.y); Pump();
            SendMessageW(p.lyric_control_,WM_SETCURSOR,reinterpret_cast<WPARAM>(p.lyric_control_),
                         MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
            Require(GetCursor() == LoadCursorW(nullptr,IDC_ARROW),"timed lyrics still present a false link cursor");
            p.settings_.lyric.drag_lyric = false; p.settings_.lyric.fullscreen_drag_lyric = true;
            Button(MOUSEEVENTF_LEFTDOWN); Button(MOUSEEVENTF_LEFTUP);
            Require(!p.lyric_line_dragging_ && GetCapture() != p.lyric_control_ && p.audio_.seek_request_ms_ < 0,
                    "disabled normal/mini drag still captured native text input");
            p.settings_.lyric.drag_lyric = true; p.settings_.lyric.fullscreen_drag_lyric = false;
            // A stationary click must not seek; then start a real drag.
            Button(MOUSEEVENTF_LEFTDOWN); Button(MOUSEEVENTF_LEFTUP);
            Require(p.audio_.seek_request_ms_ < 0,"native stationary lyric click sought audio");
            Button(MOUSEEVENTF_LEFTDOWN);
            Require(p.lyric_line_dragging_ && GetCapture() == p.lyric_control_,
                    "native normal/mini lyric text or blank click did not start drag");
            Require(GetCursor() == LoadCursorW(nullptr,scroll ? IDC_SIZEWE : IDC_SIZENS),
                    "windowed lyric drag did not set its directional cursor");
            SetCursorPos(location.x-(scroll ? 18 : 0),location.y-(scroll ? 0 : 18)); Pump();
            Require(p.lyric_line_drag_offset_ == -18 && p.audio_.seek_request_ms_ < 0,
                    "native windowed lyric preview sought early or used the wrong axis");
            const HDC dc = GetDC(p.lyric_control_);
            const auto font = SelectObject(dc,p.lyric_font_);
            const auto target = p.LyricDragTime(dc,-18);
            SelectObject(dc,font); ReleaseDC(p.lyric_control_,dc);
            Require(target.has_value(),"native windowed lyric target unavailable");
            Button(MOUSEEVENTF_LEFTUP);
            Require(!p.lyric_line_dragging_ && GetCapture() != p.lyric_control_,
                    "native windowed lyric drag retained capture after release");
            VerifySeek(p,*target,state);
        }
        ShowWindow(p.window_,SW_HIDE);
        backing_window = nullptr;
        std::cout << "real SendInput blank-area drag/menu, unchanged desktop pixels, " << work_areas.size()
                  << " monitor(s), resize/z-order and hide/disable/restore cleanup passed\n"
                     "native normal/mini text and blank drags, cursors, both axes, playing/paused,\n"
                     "stationary clicks and independent toggles passed\n";
    }
    static void ResetClock(ui::PlayerWindow& p, audio::PlaybackState state) {
        auto& engine = p.audio_;
        std::scoped_lock lock(engine.mutex_);
        engine.CancelSeekLocked(); engine.position_ms_ = 23000; engine.duration_ms_ = 60000;
        engine.state_ = state; engine.backend_ = audio::AudioEngine::Backend::wave_out;
        engine.options_.sound_fade_mode = 0xffff;
        engine.options_.fade_duration = {5000,5000,5000};
    }
    static POINT Point(ui::PlayerWindow& p, bool text) {
        for (int y = 2; y < 178; y += 3)
            for (int x = 2; x < 398; x += 3)
                if (p.LyricTextHitTest(p.lyric_control_,{x,y}) == text) return {x,y};
        throw std::runtime_error("cannot find requested lyric hit-test region");
    }
    static void Mouse(ui::PlayerWindow& p, UINT message, POINT point) {
        SendMessageW(p.lyric_control_,message,message == WM_LBUTTONUP ? 0 : MK_LBUTTON,
                     MAKELPARAM(point.x,point.y));
    }
    static void VerifySeek(ui::PlayerWindow& p, std::chrono::milliseconds target, audio::PlaybackState state) {
        auto& engine = p.audio_;
        Require(engine.Position() == target && engine.State() == state,
                "lyric seek lost its target or changed playback state");
        { std::scoped_lock lock(engine.mutex_);
          Require(engine.seek_request_ms_ == target.count() && !engine.fade_pending_ && engine.transition_gain_ == 1.0F,
                  "fullscreen drag faded or failed to queue a direct seek"); }
        const auto request = engine.TakeSeekRequest();
        engine.position_ms_ = 23500;
        Require(engine.Position() == target,"fullscreen lyric seek jumped back to old progress");
        engine.CompleteSeek(request,target.count());
        Require(engine.Position() == target,"decoder acknowledgement lost drag target");
    }
    static void Run(HMODULE resources, const fs::path& folder) {
        settings::Settings settings;
        settings.source_path = folder / L"unused.xml";
        settings.general.tray_icon = settings.general.fade_windows = settings.general.send_title_to_msn = false;
        settings.lyric.auto_download = false;
        ui::PlayerWindow p(settings);
        p.instance_ = GetModuleHandleW(nullptr); p.SetSkinResourceModule(resources);
        Require(p.LoadSkinResource(resources),"5.7.9 lyric skin unavailable");
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpszClassName = L"TTPlayerFullscreenDragTest"; cls.hInstance = p.instance_; cls.lpfnWndProc = Control;
        Require(RegisterClassExW(&cls) != 0,"test window registration failed");
        Window owner{CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,
            0,0,400,180,nullptr,nullptr,p.instance_,nullptr)};
        Window lyric{CreateWindowExW(0,cls.lpszClassName,L"",WS_CHILD,
            0,0,400,180,owner.value,nullptr,p.instance_,nullptr)};
        Require(owner.value && lyric.value,"test windows unavailable");
        p.window_ = owner.value; p.lyric_control_ = lyric.value;
        SetWindowLongPtrW(lyric.value,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&p));
        p.lyrics_ = lyrics::ParseLrc("[00:00.00]alpha\n[00:10.00]bravo\n[00:20.00]charlie\n"
                                   "[00:30.00]delta\n[00:40.00]echo\n[00:50.00]foxtrot\n");
        p.lyric_font_ = CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Arial");
        Window page{CreateDialogParamW(resources,MAKEINTRESOURCEW(263),owner.value,
            [](HWND,UINT,WPARAM,LPARAM)->INT_PTR { return FALSE; },0)};
        Require(page.value != nullptr,"fullscreen options fixture unavailable");
        SetPropW(page.value,L"TTPlayer.Options.Template",reinterpret_cast<HANDLE>(263));
        p.InitializeOptionsPage(page.value,263);
        const auto toggle = [&](bool enabled) {
            CheckDlgButton(page.value,IDC_FULLSCREEN_LYRIC_DRAG,enabled ? BST_CHECKED : BST_UNCHECKED);
            p.HandleOptionsPageDialog(page.value,WM_COMMAND,MAKEWPARAM(IDC_FULLSCREEN_LYRIC_DRAG,BN_CLICKED),0);
        };
        try {
            p.settings_.lyric.drag_lyric = false;
            for (const int mode : {1,3}) for (const int scroll : {0,1})
                for (const auto state : {audio::PlaybackState::playing,audio::PlaybackState::paused}) {
                    p.fullscreen_mode_ = mode; p.fullscreen_lyric_detached_ = true;
                    p.settings_.lyric.fullscreen_scroll_mode = scroll;
                    ResetClock(p,state);
                    const auto start = Point(p,true);
                    const POINT end{start.x - (scroll ? 18 : 0), start.y - (scroll ? 0 : 18)};
                    toggle(false);
                    Mouse(p,WM_LBUTTONDOWN,start); Mouse(p,WM_MOUSEMOVE,end); Mouse(p,WM_LBUTTONUP,end);
                    Require(!p.lyric_line_dragging_ && GetCapture() != lyric.value && p.audio_.seek_request_ms_ < 0,
                            "disabled fullscreen dragging still captured or sought");
                    toggle(true);
                    Mouse(p,WM_LBUTTONDOWN,start);
                    Require(p.lyric_line_dragging_ && GetCapture() == lyric.value,
                            "fullscreen lyric text cannot initiate drag");
                    Mouse(p,WM_MOUSEMOVE,end);
                    Require(p.lyric_line_drag_offset_ == -18 && p.audio_.seek_request_ms_ < 0,
                            "drag preview sought before button release or used wrong axis");
                    const HDC dc = GetDC(lyric.value);
                    const auto previous = SelectObject(dc,p.lyric_font_);
                    const auto target = p.LyricDragTime(dc,-18);
                    SelectObject(dc,previous); ReleaseDC(lyric.value,dc);
                    Require(target && target->count() != 23000,"test drag did not change position");
                    Mouse(p,WM_LBUTTONUP,end);
                    Require(!p.lyric_line_dragging_ && GetCapture() != lyric.value,"drag release retained capture");
                    VerifySeek(p,*target,state);
                }
            // Disabling while a gesture is pending cancels without seeking.
            ResetClock(p,audio::PlaybackState::playing);
            auto start = Point(p,true);
            toggle(true); Mouse(p,WM_LBUTTONDOWN,start); toggle(false);
            Mouse(p,WM_LBUTTONUP,{start.x-20,start.y});
            Require(!p.lyric_line_dragging_ && GetCapture() != lyric.value && p.audio_.seek_request_ms_ < 0,
                    "disabling fullscreen drag committed a pending gesture");
            toggle(true);
            for (int cancellation = 0; cancellation < 3; ++cancellation) {
                Mouse(p,WM_LBUTTONDOWN,start);
                if (cancellation == 0) SendMessageW(lyric.value,WM_CANCELMODE,0,0);
                else if (cancellation == 1) SendMessageW(lyric.value,WM_KEYDOWN,VK_ESCAPE,0);
                else ReleaseCapture();
                Mouse(p,WM_LBUTTONUP,{start.x-20,start.y});
                Require(!p.lyric_line_dragging_ && GetCapture() != lyric.value && p.audio_.seek_request_ms_ < 0,
                        "cancelled fullscreen drag sought or retained capture");
            }
            // A click, without movement, must not seek.
            Mouse(p,WM_LBUTTONDOWN,start); Mouse(p,WM_LBUTTONUP,start);
            Require(p.audio_.seek_request_ms_ < 0,"stationary click sought audio");
            p.fullscreen_lyric_detached_ = false; p.fullscreen_mode_ = 0;
            for (bool mini : {false,true}) for (int scroll : {0,1})
                for (const auto state : {audio::PlaybackState::playing,audio::PlaybackState::paused})
                    for (bool on_text : {false,true}) {
                p.mini_mode_ = mini;
                p.settings_.lyric.scroll_mode = p.settings_.lyric.mini_scroll_mode = scroll;
                ResetClock(p,state);
                const auto begin = Point(p,on_text);
                const POINT end{begin.x-(scroll ? 18 : 0),begin.y-(scroll ? 0 : 18)};
                p.settings_.lyric.drag_lyric = false; toggle(true);
                Mouse(p,WM_LBUTTONDOWN,begin); Mouse(p,WM_MOUSEMOVE,end); Mouse(p,WM_LBUTTONUP,end);
                Require(!p.lyric_line_dragging_ && GetCapture() != lyric.value && p.audio_.seek_request_ms_ < 0,
                        "fullscreen option enabled normal/mini dragging");
                p.settings_.lyric.drag_lyric = true; toggle(false);
                Mouse(p,WM_LBUTTONDOWN,begin);
                Require(p.lyric_line_dragging_ && GetCapture() == lyric.value,
                        "normal/mini lyric text or blank area cannot initiate drag");
                Mouse(p,WM_MOUSEMOVE,end);
                Require(p.lyric_line_drag_offset_ == -18 && p.audio_.seek_request_ms_ < 0,
                        "windowed lyric drag did not preview the correct axis");
                const HDC dc = GetDC(lyric.value);
                const auto font = SelectObject(dc,p.lyric_font_);
                const auto target = p.LyricDragTime(dc,-18);
                SelectObject(dc,font); ReleaseDC(lyric.value,dc);
                Require(target.has_value(),"windowed lyric drag target unavailable");
                Mouse(p,WM_LBUTTONUP,end);
                Require(!p.lyric_line_dragging_ && GetCapture() != lyric.value,
                        "windowed lyric release retained capture");
                VerifySeek(p,*target,state);
            }
            CheckInputSurface(p);
            SetWindowLongPtrW(lyric.value,GWLP_USERDATA,0);
            p.window_ = p.lyric_control_ = nullptr;
        } catch (...) {
            if (GetCapture() == lyric.value) ReleaseCapture();
            SetWindowLongPtrW(lyric.value,GWLP_USERDATA,0);
            p.fullscreen_lyric_detached_ = false; p.fullscreen_mode_ = 0;
            p.window_ = p.lyric_control_ = nullptr;
            throw;
        }
        std::cout << "full/same-screen text dragging, vertical/horizontal axes, playing/paused direct seeks,\n"
                     "no clock rollback, disable/cancel/capture loss, and normal/mini independence passed\n";
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2,"expected original repository root");
        SetProcessDPIAware();
        Require(SUCCEEDED(OleInitialize(nullptr)),"OLE initialization failed");
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&common) != FALSE,"common controls unavailable");
        Temp temp; XmlTests(temp.path);
        const HMODULE resources = LoadLibraryExW((fs::path(argv[1])/L"ttpres.dll").c_str(),nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Require(resources != nullptr,"5.7.9 resources unavailable");
        testing::ProgressSeekAccess::Run(resources,temp.path);
        FreeLibrary(resources); OleUninitialize(); return 0;
    } catch (const std::exception& error) {
        std::cerr << "fullscreen lyric drag test: " << error.what() << '\n'; return 1;
    }
}
