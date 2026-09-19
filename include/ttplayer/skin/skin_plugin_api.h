#pragma once
// TTPlayer skin provider ABI v1. Keep this header identical in the host SDK.
// UTF-16, fixed-width fields, WINAPI callbacks. No C++ ownership crosses DLLs.
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define TTP_SKIN_ABI 1u
#define TTP_SKIN_ENTRY "ttpGetSkinPlugin"
#define TTP_SKIN_COMMAND_MESSAGE L"TTPlayer.SkinPlugin.Command.v1"

enum TtpSkinCommand {
    TTP_SKIN_PLAY = 1, TTP_SKIN_PAUSE, TTP_SKIN_STOP, TTP_SKIN_PREVIOUS,
    TTP_SKIN_NEXT, TTP_SKIN_OPEN, TTP_SKIN_CLOSE, TTP_SKIN_MINIMIZE,
    TTP_SKIN_PLAYLIST, TTP_SKIN_EQUALIZER, TTP_SKIN_MENU, TTP_SKIN_OPTIONS,
    TTP_SKIN_VOLUME, TTP_SKIN_BALANCE, TTP_SKIN_SEEK, TTP_SKIN_MODE,
    TTP_SKIN_PLAY_ROW, TTP_SKIN_REMOVE_ROW, TTP_SKIN_EQ_ENABLE,
    TTP_SKIN_EQ_PRESETS, TTP_SKIN_TIME_MODE, TTP_SKIN_LYRICS,
    TTP_SKIN_SELECT_ROW, TTP_SKIN_TOGGLE_ROW, TTP_SKIN_EXTEND_ROW,
    TTP_SKIN_EXTEND_TOGGLE_ROW, TTP_SKIN_DELETE_SELECTED,
    TTP_SKIN_LIST_TOOLBAR, // value: native TTPlayer toolbar index 0..6
    TTP_SKIN_LIST_MENU, // value: row, or -1 for empty area
    TTP_SKIN_SELECT_ALL, TTP_SKIN_PROPERTIES, TTP_SKIN_ALWAYS_ON_TOP,
    TTP_SKIN_VISUAL_NEXT, TTP_SKIN_VISUAL_MENU, TTP_SKIN_EQ_BANDS,
    TTP_SKIN_MOVE_SELECTION, TTP_SKIN_COPY_SELECTION, // insertion row; current native selection
    TTP_SKIN_EQ_VALUE = 100 // + 0: preamp, + 1..10: frequency bands; value -12..12
};

typedef struct TtpSkinState {
    uint32_t size;
    int32_t playback; // 0 stopped, 1 opening, 2 playing, 3 paused, 4 failed
    int64_t position_ms, duration_ms;
    int32_t volume, balance; // 0..100, -100..100
    int32_t mode; // TTPlayer: single=0, repeat one=1, sequential=2, repeat all=3, random=4
    int32_t channels, sample_rate, bitrate;
    int32_t playlist_visible, equalizer_visible, eq_enabled, elapsed;
    int32_t eq[11];
    uint32_t track_count;
    int32_t playing_row;
    wchar_t title[512];
} TtpSkinState;

typedef struct TtpSkinTrack {
    uint32_t size;
    int32_t duration_ms;
    wchar_t title[512];
} TtpSkinTrack;

enum TtpSkinDragPhase { TTP_SKIN_DRAG_BEGIN = 1, TTP_SKIN_DRAG_MOVE, TTP_SKIN_DRAG_END };
enum TtpSkinDragEdges {
    TTP_SKIN_DRAG_WINDOW = 1,
    TTP_SKIN_DRAG_RIGHT = 0x10, TTP_SKIN_DRAG_BOTTOM = 0x20,
    TTP_SKIN_DRAG_LEFT = 0x40, TTP_SKIN_DRAG_TOP = 0x80
};
typedef struct TtpSkinDrag {
    uint32_t size, phase;
    HWND window;
    POINT point; // Current mouse message's client coordinates, not a queued cursor sample.
    uint32_t edges;
    SIZE minimum; // Provider's resize minimum; ignored for window movement.
} TtpSkinDrag;

typedef struct TtpSkinVisualColors {
    COLORREF background, top, middle, bottom, peak, scope;
} TtpSkinVisualColors;

typedef struct TtpSkinHost {
    uint32_t size, version;
    void* context;
    // Called synchronously on the UI thread. Host fills caller-owned buffers.
    BOOL (WINAPI *query)(void*, TtpSkinState*);
    BOOL (WINAPI *track)(void*, uint32_t, TtpSkinTrack*);
    // Must enqueue commands: callbacks may open menus and unload this skin later.
    void (WINAPI *command)(void*, uint32_t, int32_t);
    // Optional, size-negotiated v1 extension. Synchronous UI-thread geometry
    // only: never enqueue, dispatch playback commands, or unload the provider.
    // The host owns capture, native snapping and atomic attached-window moves.
    BOOL (WINAPI *drag)(void*, const TtpSkinDrag*);
    // Optional read-only selection flags: bit 0 selected, bit 1 caret.
    uint32_t (WINAPI *selection)(void*, uint32_t);
    // Render the native visualization into the supplied rectangle. The optional
    // colors apply to this paint only, without modifying user preferences.
    BOOL (WINAPI *visual)(void*, HDC, const RECT*, const TtpSkinVisualColors*);
} TtpSkinHost;
#define TTP_SKIN_HOST_V1_SIZE offsetof(TtpSkinHost, drag)

typedef struct TtpSkinWindows {
    uint32_t size;
    HWND player, playlist, equalizer;
} TtpSkinWindows;

typedef struct TtpSkinInfo {
    uint32_t size;
    wchar_t name[128], author[128];
} TtpSkinInfo;

typedef struct TtpSkinPlugin {
    uint32_t size, version;
    const wchar_t* name; // Provider-defined label for Options tabs and Skin submenus.
    // Probe is reentrant; never creates UI or mutates an active skin.
    HRESULT (WINAPI *probe)(const wchar_t*, TtpSkinInfo*);
    // Create fully validates the package without touching windows.
    HRESULT (WINAPI *create)(const wchar_t*, const TtpSkinHost*, void**);
    // Attach/detach/destroy/render are UI-thread calls. Detach restores HWND state.
    HRESULT (WINAPI *attach)(void*, const TtpSkinWindows*);
    void (WINAPI *detach)(void*);
    void (WINAPI *destroy)(void*);
    // Caller owns returned HBITMAP (DeleteObject); valid after instance destruction.
    HBITMAP (WINAPI *preview)(void*);
    void (WINAPI *shade)(void*);
    void (WINAPI *paint)(void*, HWND, HDC);
    BOOL (WINAPI *translate)(void*, const MSG*);
    // Provider-owned package namespace, relative to the executable's Skin
    // directory, and supported suffixes separated by semicolons. Required by
    // the generic host; legacy v1 callers may request only the prefix below.
    const wchar_t* skin_directory;
    const wchar_t* extensions; // e.g. L".wsz;.wal"; no wildcard/native suffixes

} TtpSkinPlugin;
#define TTP_SKIN_PLUGIN_V1_SIZE offsetof(TtpSkinPlugin, skin_directory)

typedef HRESULT (WINAPI *TtpGetSkinPlugin)(uint32_t, TtpSkinPlugin*);
