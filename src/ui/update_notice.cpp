#include "update_notice.h"
#include "ttplayer/ui/wtl_runtime.h"
#include "ttplayer/platform/windows_features.h"
#include <atlhost.h>
#include <shellapi.h>
#include <memory>

namespace ttplayer::ui {
namespace {
constexpr UINT kCallback=WM_APP+11;
struct Notice {
    HWND owner{},window{};
    HFONT font{},bold{};
    HBRUSH header{CreateSolidBrush(RGB(135,194,218))};
    bool native{},icon{},owned{};
    ATL::CAxWindow html;
    ~Notice(){if(font) DeleteObject(font);if(bold) DeleteObject(bold);if(header) DeleteObject(header);}
    void Options() {PostMessageW(owner,0x7f4,1047,1);DestroyWindow(window);}
    void Create() {
        if(native) {
            NOTIFYICONDATAW info{};info.cbSize=sizeof(info);info.hWnd=window;info.uID=1;
            info.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;info.hIcon=LoadIconW(nullptr,IDI_INFORMATION);info.uCallbackMessage=kCallback;
            wcscpy_s(info.szTip,L"千千静听 - 软件更新");
            icon=Shell_NotifyIconW(NIM_ADD,&info)!=FALSE;
            if(icon) {
                info.uFlags=NIF_INFO;info.dwInfoFlags=NIIF_INFO;
                wcscpy_s(info.szInfoTitle,L"千千静听 - 最新版本");
                wcscpy_s(info.szInfo,L"发现新版本，请点击通知前往选项更新。");
                if(Shell_NotifyIconW(NIM_MODIFY,&info)) {SetTimer(window,1,60000,nullptr);return;}
                Shell_NotifyIconW(NIM_DELETE,&info);icon=false;
            }
            native=false;
        }
        // 00455308: topmost, toolwindow, no activation. 00455C99: HTML area
        // 360 x 240, outer padding 12 x 10, anchored eight pixels from right.
        HDC dc=GetDC(window);const int dpi=GetDeviceCaps(dc,LOGPIXELSY);ReleaseDC(window,dc);
        font=CreateFontW(-MulDiv(9,dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,DEFAULT_QUALITY,0,L"宋体");
        bold=CreateFontW(-MulDiv(9,dpi,72),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,DEFAULT_QUALITY,0,L"宋体");
        RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        const int w=MulDiv(384,dpi,96),h=MulDiv(260,dpi,96),bar=MulDiv(30,dpi,96);
        SetWindowPos(window,HWND_TOPMOST,work.right-w-8,work.bottom-h,w,h,SWP_NOACTIVATE);
        auto options=CreateWindowW(L"STATIC",L"选项",WS_CHILD|WS_VISIBLE|SS_NOTIFY,w-MulDiv(58,dpi,96),MulDiv(9,dpi,96),MulDiv(28,dpi,96),MulDiv(18,dpi,96),window,HMENU(123),GetModuleHandleW(nullptr),nullptr);
        auto close=CreateWindowW(L"BUTTON",L"×",WS_CHILD|WS_VISIBLE|BS_FLAT,w-MulDiv(24,dpi,96),MulDiv(8,dpi,96),MulDiv(16,dpi,96),MulDiv(16,dpi,96),window,HMENU(IDCANCEL),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(options,WM_SETFONT,WPARAM(font),0);SendMessageW(close,WM_SETFONT,WPARAM(font),0);
        bool rendered=false;
        if(SUCCEEDED(EnsureWtlRuntime()) && ATL::AtlAxWinInit()) {
            RECT content{2,bar,w-2,h-2};
            if(html.Create(window,content,nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN))
                // ATL's MSHTML: persistence stream uses the system code page
                // on old IE. ASCII character references render identically on
                // XP and modern systems without an ACP/UTF-8 mismatch.
                rendered=SUCCEEDED(html.CreateControl(L"MSHTML:<html><body style='margin:12px;background:white;font:12px SimSun'>&#35831;&#28857;&#20987;&#36873;&#39033;&#21069;&#24448;&#26356;&#26032;</body></html>"));
        }
        if(!rendered) CreateWindowW(L"STATIC",L"请点击选项前往更新",WS_CHILD|WS_VISIBLE,12,bar+12,w-24,h-bar-24,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        // Original HTML notices use an effectively indefinite lifetime
        // (00462D2A passes 0x7fffffff), closed by the options/close controls.
        ShowWindow(window,SW_SHOWNOACTIVATE);
    }
    static LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<Notice*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE) {self=static_cast<Notice*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,LONG_PTR(self));}
        if(self) {
            if(msg==WM_CREATE) {self->Create();return 0;}
            if(msg==kCallback) {if(LOWORD(lp)==NIN_BALLOONUSERCLICK) self->Options();else if(LOWORD(lp)==NIN_BALLOONTIMEOUT) DestroyWindow(hwnd);return 0;}
            if(msg==WM_TIMER || msg==WM_CLOSE) {DestroyWindow(hwnd);return 0;}
            if(msg==WM_COMMAND) {if(LOWORD(wp)==123) self->Options();else if(LOWORD(wp)==IDCANCEL) DestroyWindow(hwnd);return 0;}
            if(msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
            if(msg==WM_CTLCOLORSTATIC) {
                const bool link=GetDlgCtrlID(HWND(lp))==123;
                SetTextColor(HDC(wp),link?RGB(0,0,255):RGB(0,0,0));SetBkMode(HDC(wp),TRANSPARENT);
                return LRESULT(link?self->header:GetStockObject(WHITE_BRUSH));
            }
            if(msg==WM_SETCURSOR && GetDlgCtrlID(HWND(wp))==123) {SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
            if(msg==WM_PAINT) {
                PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);RECT r{};GetClientRect(hwnd,&r);
                HBRUSH brush=CreateSolidBrush(RGB(135,194,218));FillRect(dc,&r,brush);DeleteObject(brush);
                const auto old=SelectObject(dc,self->bold);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(8,64,112));
                const int dpi=GetDeviceCaps(dc,LOGPIXELSY),unit=MulDiv(16,dpi,96);
                DrawIconEx(dc,MulDiv(12,dpi,96),MulDiv(8,dpi,96),LoadIconW(nullptr,IDI_INFORMATION),unit,unit,0,nullptr,DI_NORMAL);
                RECT title{MulDiv(38,dpi,96),0,r.right-MulDiv(62,dpi,96),MulDiv(30,dpi,96)};
                DrawTextW(dc,L"千千静听 - 最新版本",-1,&title,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
                SelectObject(dc,old);EndPaint(hwnd,&paint);return 0;
            }
            if(msg==WM_DESTROY) {
                if(self->icon){NOTIFYICONDATAW info{};info.cbSize=sizeof(info);info.hWnd=hwnd;info.uID=1;Shell_NotifyIconW(NIM_DELETE,&info);}
                if(self->html.IsWindow()) self->html.DestroyWindow();return 0;
            }
            if(msg==WM_NCDESTROY) {SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);if(self->owned) delete self;return DefWindowProcW(hwnd,msg,wp,lp);}
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};
}
HWND ShowUpdateNotice(HWND owner) {
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Notice::Proc;
    wc.lpszClassName=L"TTPlayerUpdateNotice";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
    auto state=std::make_unique<Notice>();state->owner=owner;state->native=platform::CurrentWindowsFeatures().version.AtLeast(6,2);
    const auto window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"千千静听 - 最新版本",
        WS_POPUP|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,0,0,owner,nullptr,wc.hInstance,state.get());
    if(window) {state->owned=true;state.release();}return window;
}
}
