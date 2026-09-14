#include "lyric_upload_window.h"
#include "../app/resource_ids.h"
#include <commctrl.h>
#include <exdisp.h>
#include <exdispid.h>
#include <mshtml.h>
#include <ocidl.h>
#include <oleidl.h>
#include <wrl/client.h>
#include <winhttp.h>
#include <algorithm>
#include <new>

namespace ttplayer::ui {
using Microsoft::WRL::ComPtr;
namespace {
struct Bstr {
    BSTR value{};
    explicit Bstr(std::wstring_view text) : value(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
    ~Bstr() { SysFreeString(value); }
};
std::wstring Text(UINT id) {
    wchar_t text[512]{};
    LoadStringW(GetModuleHandleW(nullptr), id, text, static_cast<int>(std::size(text)));
    return text;
}

// A small SDK-only OLE container replaces the original AtlAxWin71 wrapper.
// No ATL redistributable, external EXE, or installed IE desktop application
// is launched. COM references and event connections belong to this window.
class UploadWindow final : public IOleClientSite, public IOleInPlaceSite,
                           public IOleInPlaceFrame, public IDispatch {
public:
    HWND window{}, status{}, progress{};
    ComPtr<IOleObject> object;
    ComPtr<IWebBrowser2> browser;
    ComPtr<IConnectionPoint> connection;
    DWORD cookie{};
    ULONG references{1};
    LyricUploadData data;
    std::wstring page;
    bool closed{}, filled{};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IOleClientSite) *result = static_cast<IOleClientSite*>(this);
        else if (iid == IID_IOleWindow || iid == IID_IOleInPlaceSite) *result = static_cast<IOleInPlaceSite*>(this);
        else if (iid == IID_IOleInPlaceUIWindow || iid == IID_IOleInPlaceFrame) *result = static_cast<IOleInPlaceFrame*>(this);
        else if (iid == IID_IDispatch || iid == DIID_DWebBrowserEvents2) *result = static_cast<IDispatch*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE SaveObject() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMoniker(DWORD, DWORD, IMoniker** p) override { if (p) *p = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetContainer(IOleContainer** p) override { if (!p) return E_POINTER; *p = nullptr; return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE ShowObject() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShowWindow(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestNewObjectLayout() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND* p) override { if (!p) return E_POINTER; *p = window; return S_OK; }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CanInPlaceActivate() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnInPlaceActivate() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUIActivate() override { return S_OK; }
    RECT ContentRect() const {
        RECT bounds{}, bar{}; GetClientRect(window, &bounds); GetWindowRect(status, &bar);
        bounds.bottom = std::max(bounds.top, bounds.bottom - (bar.bottom - bar.top));
        return bounds;
    }
    HRESULT STDMETHODCALLTYPE GetWindowContext(IOleInPlaceFrame** frame, IOleInPlaceUIWindow** doc,
            LPRECT pos, LPRECT clip, LPOLEINPLACEFRAMEINFO info) override {
        if (!frame || !doc || !pos || !clip || !info) return E_POINTER;
        *frame = this; AddRef(); *doc = nullptr; *pos = *clip = ContentRect();
        info->cb = sizeof(*info); info->fMDIApp = FALSE; info->hwndFrame = window;
        info->haccel = nullptr; info->cAccelEntries = 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Scroll(SIZE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnUIDeactivate(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnInPlaceDeactivate() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DiscardUndoState() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeactivateAndUndo() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnPosRectChange(LPCRECT rect) override {
        ComPtr<IOleInPlaceObject> inplace;
        return object && SUCCEEDED(object.As(&inplace)) ? inplace->SetObjectRects(rect, rect) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetBorder(LPRECT rect) override { if (!rect) return E_POINTER; *rect = ContentRect(); return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestBorderSpace(LPCBORDERWIDTHS) override { return INPLACE_E_NOTOOLSPACE; }
    HRESULT STDMETHODCALLTYPE SetBorderSpace(LPCBORDERWIDTHS) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetActiveObject(IOleInPlaceActiveObject*, LPCOLESTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE InsertMenus(HMENU, LPOLEMENUGROUPWIDTHS) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMenu(HMENU, HOLEMENU, HWND) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RemoveMenus(HMENU) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetStatusText(LPCOLESTR text) override {
        if (!closed && !filled) SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(LPMSG, WORD) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override { if (!n) return E_POINTER; *n = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    void Status(UINT id) { SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(Text(id).c_str())); }
    bool IsTopDocument(IDispatch* dispatch) {
        ComPtr<IUnknown> sender, top;
        return dispatch && SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&sender))) && browser &&
            SUCCEEDED(browser.As(&top)) && sender == top;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS* args,
            VARIANT*, EXCEPINFO*, UINT*) override {
        if (closed || !args) return S_OK;
        if (id == DISPID_DOCUMENTCOMPLETE && args->cArgs == 2 &&
            args->rgvarg[1].vt == VT_DISPATCH && IsTopDocument(args->rgvarg[1].pdispVal)) {
            KillTimer(window, 1); ShowWindow(progress, SW_HIDE);
            BSTR address{};
            browser->get_LocationURL(&address);
            // Do not disclose the document to a redirected/unrelated site or
            // a frame; only the requested upload page receives the snapshot.
            const bool expected = address && _wcsicmp(address, page.c_str()) == 0;
            SysFreeString(address);
            ComPtr<IDispatch> document;
            if (!filled && expected && SUCCEEDED(browser->get_Document(&document)))
                filled = PopulateLyricUploadDocument(document.Get(), data);
            Status(filled ? IDS_LYRIC_UPLOAD_READY : IDS_LYRIC_UPLOAD_UNAVAILABLE);
        } else if (id == DISPID_PROGRESSCHANGE && args->cArgs == 2 &&
                   args->rgvarg[0].vt == VT_I4 && args->rgvarg[1].vt == VT_I4) {
            const auto maximum = args->rgvarg[0].lVal, current = args->rgvarg[1].lVal;
            ShowWindow(progress, current >= 0 && maximum > 0 ? SW_SHOW : SW_HIDE);
            SendMessageW(progress, PBM_SETRANGE32, 0, std::max(1L, maximum));
            SendMessageW(progress, PBM_SETPOS, std::max(0L, current), 0);
        } else if (id == DISPID_NAVIGATEERROR && args->cArgs == 5 &&
                   args->rgvarg[4].vt == VT_DISPATCH && IsTopDocument(args->rgvarg[4].pdispVal)) {
            KillTimer(window, 1); ShowWindow(progress, SW_HIDE); Status(IDS_LYRIC_UPLOAD_UNAVAILABLE);
        } else if (id == DISPID_NEWWINDOW3 || id == DISPID_NEWWINDOW2) {
            // No unsolicited popups from the historical endpoint.
            const UINT cancel = id == DISPID_NEWWINDOW3 ? 3 : 0;
            if (args->cArgs > cancel && args->rgvarg[cancel].vt == (VT_BYREF | VT_BOOL))
                *args->rgvarg[cancel].pboolVal = VARIANT_TRUE;
        }
        return S_OK;
    }
    void Layout() {
        SendMessageW(status, WM_SIZE, 0, 0);
        RECT bounds{}; GetClientRect(status, &bounds);
        const int parts[]{std::max(0L, bounds.right - 120), bounds.right};
        SendMessageW(status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
        MoveWindow(progress, std::max(0L, bounds.right - 116), 2, 100, std::max(0L, bounds.bottom - 4), TRUE);
        const auto content = ContentRect(); OnPosRectChange(&content);
    }
    bool Initialize() {
        status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
        progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | PBS_SMOOTH,
            0, 0, 0, 0, status, reinterpret_cast<HMENU>(2), GetModuleHandleW(nullptr), nullptr);
        if (!status || !progress) return false;
        Layout(); Status(IDS_LYRIC_UPLOAD_LOADING);
        HRESULT hr = CoCreateInstance(CLSID_WebBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&object));
        if (FAILED(hr) || FAILED(object->SetClientSite(this)) || FAILED(object.As(&browser))) return false;
        OleSetContainedObject(object.Get(), TRUE);
        ComPtr<IConnectionPointContainer> events;
        if (FAILED(browser.As(&events)) || FAILED(events->FindConnectionPoint(DIID_DWebBrowserEvents2, &connection)) ||
            FAILED(connection->Advise(static_cast<IDispatch*>(this), &cookie))) return false;
        const auto bounds = ContentRect();
        if (FAILED(object->DoVerb(OLEIVERB_SHOW, nullptr, this, 0, window, &bounds))) return false;
        browser->put_Silent(VARIANT_TRUE);
        VARIANT url{}, headers{}, empty{};
        url.vt = headers.vt = VT_BSTR;
        url.bstrVal = SysAllocString(page.c_str());
        // 0044895B uses the containing directory (without final slash).
        const auto slash = page.rfind(L'/');
        const auto referer = L"Referer: " + page.substr(0, slash) + L"\r\n";
        headers.bstrVal = SysAllocString(referer.c_str());
        hr = url.bstrVal && headers.bstrVal ? browser->Navigate2(&url, &empty, &empty, &empty, &headers) : E_OUTOFMEMORY;
        VariantClear(&url); VariantClear(&headers);
        if (SUCCEEDED(hr)) SetTimer(window, 1, 30000, nullptr);
        return SUCCEEDED(hr);
    }
    void Close() {
        if (closed) return;
        closed = true; KillTimer(window, 1);
        if (connection && cookie) connection->Unadvise(cookie);
        cookie = 0; connection.Reset();
        if (browser) browser->Stop();
        if (object) { object->Close(OLECLOSE_NOSAVE); object->SetClientSite(nullptr); }
        browser.Reset(); object.Reset();
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<UploadWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<UploadWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = hwnd; self->AddRef();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (msg == WM_CREATE) { if (!self->Initialize()) self->Status(IDS_LYRIC_UPLOAD_UNAVAILABLE); return 0; }
            if (msg == WM_SIZE) { self->Layout(); return 0; }
            if (msg == WM_TIMER && wp == 1) {
                KillTimer(hwnd, 1); if (self->browser) self->browser->Stop();
                ShowWindow(self->progress, SW_HIDE); self->Status(IDS_LYRIC_UPLOAD_UNAVAILABLE); return 0;
            }
            if (msg == WM_DESTROY) { self->Close(); return 0; }
            if (msg == WM_NCDESTROY) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); self->window = nullptr; self->Release();
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};
}

bool PopulateLyricUploadDocument(IDispatch* dispatch, const LyricUploadData& data) {
    ComPtr<IHTMLDocument2> document;
    ComPtr<IHTMLElementCollection> elements;
    if (!dispatch || FAILED(dispatch->QueryInterface(IID_PPV_ARGS(&document))) ||
        FAILED(document->get_all(&elements)) || !elements) return false;
    ComPtr<IHTMLInputElement> inputs[3];
    ComPtr<IHTMLTextAreaElement> area;
    const wchar_t* names[]{L"artist", L"title", L"album", L"lyrics"};
    // First resolve ALL fields, so an error page cannot get a partial fill.
    for (size_t i = 0; i < std::size(names); ++i) {
        Bstr name(names[i]); VARIANT key{}, index{}; key.vt = VT_BSTR; key.bstrVal = name.value;
        index.vt = VT_I4; index.lVal = 0;
        ComPtr<IDispatch> item;
        if (!name.value || FAILED(elements->item(key, index, &item)) || !item) return false;
        if (i < 3) { if (FAILED(item.As(&inputs[i]))) return false; }
        else if (FAILED(item.As(&area))) return false;
    }
    const std::wstring* values[]{&data.artist, &data.title, &data.album};
    for (size_t i = 0; i < 3; ++i) { Bstr value(*values[i]); if (!value.value || FAILED(inputs[i]->put_value(value.value))) return false; }
    Bstr text(data.lyrics);
    return text.value && SUCCEEDED(area->put_value(text.value));
}

std::wstring LyricUploadUrl(std::wstring_view search_url) {
    if (std::any_of(search_url.begin(), search_url.end(), [](wchar_t c) { return c <= L' ' || c == 0x7f; })) return {};
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(search_url.data(), static_cast<DWORD>(search_url.size()), 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength) return {};
    const auto authority = search_url.find(L"://");
    if (authority == search_url.npos) return {};
    const auto end = search_url.find_first_of(L"/?#", authority + 3);
    return std::wstring(search_url.substr(0, end)) + L"/dll/lrcup.php";
}

HWND ShowLyricUploadWindow(HWND owner, std::wstring_view caption, const LyricUploadData& data, std::wstring_view page) {
    auto* self = new (std::nothrow) UploadWindow;
    if (!self) return nullptr;
    self->data = data; self->page = page;
    WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc = UploadWindow::Proc;
    cls.hInstance = GetModuleHandleW(nullptr); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(128)); cls.hIconSm = cls.hIcon;
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); cls.lpszClassName = kLyricUploadWindowClass;
    RegisterClassExW(&cls);
    RECT bounds{0, 0, 500, 480}; // 0044895B's client area.
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const auto title = std::wstring(caption);
    HWND result = CreateWindowExW(0, cls.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        owner, nullptr, cls.hInstance, self);
    if (result) { ShowWindow(result, SW_SHOWNORMAL); UpdateWindow(result); }
    self->Release();
    return result;
}

bool TranslateLyricUploadMessage(const MSG& message) {
    if (message.message < WM_KEYFIRST || message.message > WM_KEYLAST) return false;
    const HWND root = GetAncestor(message.hwnd, GA_ROOT);
    wchar_t name[80]{}; GetClassNameW(root, name, static_cast<int>(std::size(name)));
    if (wcscmp(name, kLyricUploadWindowClass)) return false;
    auto* self = reinterpret_cast<UploadWindow*>(GetWindowLongPtrW(root, GWLP_USERDATA));
    ComPtr<IOleInPlaceActiveObject> active;
    if (!self || !self->browser || FAILED(self->browser.As(&active))) return false;
    MSG copy = message; return active->TranslateAccelerator(&copy) == S_OK;
}
}
