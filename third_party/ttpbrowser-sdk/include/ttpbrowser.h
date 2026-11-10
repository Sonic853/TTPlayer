#pragma once
// Versioned, optional in-process ABI. No STL objects or CRT-owned allocations
// cross this boundary. Borrowed strings/bitmaps are copied during each call.
#include <windows.h>
#include <objidl.h>
#include <oaidl.h>
#include <stdint.h>
#include <stddef.h>

#define TTP_BROWSER_ABI_VERSION 1u
#define TTP_BROWSER_VIEW_TOGGLE 0u
#define TTP_BROWSER_VIEW_MUSIC 1u
#define TTP_BROWSER_VIEW_DOWNLOADS 2u
#define TTP_BROWSER_VIEW_HIDE 3u
#define TTP_BROWSER_STATE_VISIBLE 1u
#define TTP_BROWSER_STATE_AUTOSTART 2u
#define TTP_BROWSER_STATE_DOWNLOADS 4u
#define TTP_BROWSER_STATE_WEBVIEW2 8u
#define TTP_BROWSER_STATE_IE 16u
#define TTP_BROWSER_MENU_OPTIONS 0x0905u

enum ttp_browser_control_id {
    TTP_BROWSER_CLOSE, TTP_BROWSER_MUSIC, TTP_BROWSER_DOWNLOADS,
    TTP_BROWSER_BACK, TTP_BROWSER_FORWARD, TTP_BROWSER_REFRESH,
    TTP_BROWSER_STARTUP, TTP_BROWSER_LINK, TTP_BROWSER_CONTROL_COUNT
};
struct ttp_browser_track {
    const wchar_t* source;
    const wchar_t* artist;
    const wchar_t* title;
    const wchar_t* tid;
};
struct ttp_browser_state {
    uint32_t size;
    RECT bounds;
    RECT alternate_bounds;
    uint32_t flags;
};
struct ttp_browser_control {
    RECT bounds;
    HBITMAP image;
    SIZE image_size;
    uint32_t frames;
    COLORREF color;
    LOGFONTW font;
    int checkbox_interval;
};
struct ttp_browser_skin {
    uint32_t size;
    BOOL valid;
    RECT position;
    RECT content;
    HBITMAP background;
    SIZE background_size;
    COLORREF transparent_color;
    struct ttp_browser_control controls[TTP_BROWSER_CONTROL_COUNT];
    RECT resize_rect;
    BOOL resize_tile;
};
struct ttp_browser_host {
    uint32_t size;
    uint32_t version;
    void* context;
    HWND owner;
    HMODULE resources;
    // UI-thread callback. Strings are valid only for the duration of the call.
    HRESULT (WINAPI* submit_tracks)(void*, const struct ttp_browser_track*, uint32_t,
                                   int mode0, int mode1, int mode2);
    HRESULT (WINAPI* query_info)(void*, uint32_t kind, uint32_t selector, BSTR*);
    // Worker callback. Host snapshots network options under its own lock and
    // duplicates cancellation_event before retaining it in a stream callback.
    HRESULT (WINAPI* open_stream)(void*, const wchar_t*, HANDLE cancellation_event,
                                 IStream**, BOOL* can_persist);
    void (WINAPI* state_changed)(void*, const struct ttp_browser_state*);
    HRESULT (WINAPI* setup_pack)(void*, const wchar_t*, const wchar_t*, const wchar_t*);
    // Optional, size-gated UI-thread extension. The host adjusts a proposed
    // screen rectangle using its current snap settings and registered windows.
    // hit_test is HTCAPTION or an HTLEFT..HTBOTTOMRIGHT resize code. S_OK
    // accepts the result; any other result leaves the proposed rectangle alone.
    // The DLL subsequently enforces its skin's resize limits.
    HRESULT (WINAPI* adjust_window_rect)(void*, HWND, uint32_t hit_test, RECT*);
    // Optional, size-gated UI-thread extension. The lyric result is a BSTR
    // owned by the caller (SysFreeString). S_FALSE means no known association;
    // callers must not substitute a guessed same-name lyric file.
    HRESULT (WINAPI* resolve_associated_lyric)(void*, const wchar_t* media_path,
                                              BSTR* lyric_path);
    // Optional: 1 resolves stable sources; 2 also preserves origin Authorization.
    uint32_t media_source_version;
    // Optional UI-thread shortcut to Options / Network / Music / Media sources.
    HRESULT (WINAPI* open_media_settings)(void*);
    // Optional UI-thread visibility transition. S_OK lets the host complete
    // an asynchronous fade; other results use immediate ShowWindow fallback.
    HRESULT (WINAPI* set_window_visible)(void*, HWND, BOOL visible);
    // Optional UI-thread popup tracking with the host's common menu renderer.
    // The menu is borrowed for this call. S_OK includes cancellation (command 0);
    // E_NOTIMPL requests native tracking. Menu notifications go to host.owner.
    HRESULT (WINAPI* track_popup_menu)(void*, HWND, HMENU, POINT, UINT* command);
    // Optional UI-thread shortcut to Options / Network / Music.
    HRESULT (WINAPI* open_options)(void*);
    // Optional component-provided page. S_FALSE means absent.
    // BSTR outputs belong to the caller; no legacy player bridge is exposed.
    HRESULT (WINAPI* get_download_page)(void*, BSTR* name, BSTR* url);
    HRESULT (WINAPI* recognize_page_download)(void*, const wchar_t* page_url,
        const wchar_t* target_url, BSTR* filename);
    HRESULT (WINAPI* get_page_script)(void*, BSTR* script);
    HRESULT (WINAPI* recognize_page_audio)(void*, const wchar_t* page_url, const wchar_t* audio_url);
};
// Version 1 hosts predating the optional window-drag callback remain supported.
#define TTP_BROWSER_HOST_V1_SIZE offsetof(struct ttp_browser_host, adjust_window_rect)
struct ttp_browser_config {
    uint32_t size;
    const wchar_t* runtime_directory;
    const wchar_t* state_file;
    const wchar_t* download_directory;
    const wchar_t* home_url;
    int maximum_downloads;
    BOOL artist_folders;
    BOOL replace_files;
    BOOL topmost;
    BYTE opacity;
    struct ttp_browser_state initial_state;
    const wchar_t* cache_directory;
    // 0 = WebView2 when available, then IE; 1 = IE; 2 = require WebView2.
    uint32_t engine;
    BOOL accept_recommendations;
    // Optional size-gated extension; older hosts retain GPU acceleration.
    BOOL disable_gpu;
};
#define TTP_BROWSER_CONFIG_V1_SIZE offsetof(struct ttp_browser_config, disable_gpu)
struct ttp_browser_api {
    uint32_t size;
    uint32_t version;
    HRESULT (WINAPI* create)(const struct ttp_browser_host*, const struct ttp_browser_config*, void**);
    void (WINAPI* destroy)(void*);
    HRESULT (WINAPI* configure)(void*, const struct ttp_browser_config*);
    HRESULT (WINAPI* set_skin)(void*, const struct ttp_browser_skin*);
    HRESULT (WINAPI* show)(void*, uint32_t view);
    HWND (WINAPI* window)(void*);
    BOOL (WINAPI* translate_message)(void*, MSG*);
    HRESULT (WINAPI* get_state)(void*, struct ttp_browser_state*);
    HRESULT (WINAPI* enqueue)(void*, const struct ttp_browser_track*, uint64_t* task_id);
    HRESULT (WINAPI* prefetch)(void*, const wchar_t*);
    // Returns the legacy external object, with AddRef. Used by host diagnostics
    // and contract tests; the browser itself installs an origin-scoped wrapper.
    // Retained objects reject calls after destroy. Release all returned COM
    // references before FreeLibrary; the caller owns the DLL load reference.
    HRESULT (WINAPI* external_dispatch)(void*, IDispatch**);
    // Optional worker-thread API. Caller must keep the instance alive until
    // the call returns. Credentials never enter the persistent source URL.
    HRESULT (WINAPI* resolve_source)(void*, const wchar_t*,
        BOOL (WINAPI* canceled)(void*), void* cancel_context, BSTR* resolved_url);
    // Optional native child page, owned by the DLL and destroyed with parent.
    HRESULT (WINAPI* create_settings_page)(void*, HWND parent, HWND* page);
    HRESULT (WINAPI* commit_settings_page)(void*);
    // Optional authenticated resolution. Both BSTRs belong to the caller.
    // Authorization is transient, origin-scoped, and must never be persisted.
    HRESULT (WINAPI* resolve_source_ex)(void*, const wchar_t*,
        BOOL (WINAPI* canceled)(void*), void* cancel_context, BSTR* resolved_url, BSTR* authorization);
};
#define TTP_BROWSER_API_V1_SIZE offsetof(struct ttp_browser_api, resolve_source)
typedef const struct ttp_browser_api* (WINAPI* ttp_browser_get_api_fn)(uint32_t);
#ifdef __cplusplus
extern "C" {
#endif
const struct ttp_browser_api* WINAPI ttp_browser_get_api(uint32_t version);
#ifdef __cplusplus
}
#endif
