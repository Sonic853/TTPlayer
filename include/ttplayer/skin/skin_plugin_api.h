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
    TTP_SKIN_CONTENT_FULLSCREEN, // low byte: content mode; next byte: visual type
    TTP_SKIN_CONTENT_MENU, // enqueue a provider-defined menu on the content surface
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

// Read-only native analysis snapshot for provider-owned spectrum styles.
// Magnitudes are the first 256 bins of the host's existing 512-sample FFT.
typedef struct TtpSkinSpectrumFrame {
    uint32_t size, type, playback, count;
    uint64_t generation, revision;
    int16_t magnitudes[256];
} TtpSkinSpectrumFrame;

enum TtpSkinContentMode {
    TTP_SKIN_CONTENT_LYRICS = 1, TTP_SKIN_CONTENT_VISUAL, TTP_SKIN_CONTENT_COMBINED
};

// Provider geometry/state; bounds use content-window client coordinates.
typedef struct TtpSkinContent {
    uint32_t size;
    HWND window;
    RECT bounds;
    uint32_t mode, visual_type;
} TtpSkinContent;
typedef struct TtpSkinLyricColors {
    uint32_t size;
    COLORREF text, highlight, background;
} TtpSkinLyricColors;
// Host-created content widgets remain visible inside a provider-owned frame.
#define TTP_SKIN_CONTENT_CHILD L"TTPlayer.SkinPlugin.ContentChild.v1"

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
    // Optional UI-thread command label, including the host's configured hotkey.
    // FALSE means the provider should use its own description. Caller owns text.
    BOOL (WINAPI *tip)(void*, uint32_t, int32_t, wchar_t*, uint32_t);
    // Optional synchronous geometry change (e.g. shade/unshade). Keep the
    // source's top-left fixed and carry windows docked below its old bottom.
    // Client size is in physical pixels. FALSE requests a single-window fallback.
    BOOL (WINAPI *resize)(void*, HWND, SIZE);
    BOOL (WINAPI *spectrum)(void*, TtpSkinSpectrumFrame*);
    // Optional bounded content canvas. Reuses the host lyric document and
    // fullscreen visual renderer; never changes the main player's visual mode.
    BOOL (WINAPI *content)(void*, HDC, const RECT*, uint32_t mode, uint32_t visual_type);
    // Optional synchronous content input/layout. TRUE consumes the message.
    // Coordinates remain those of window; capture belongs to that HWND.
    // Never open modal UI or unload the provider in this callback.
    BOOL (WINAPI *content_input)(void*, const TtpSkinContent*, const MSG*, LRESULT*);
} TtpSkinHost;
#define TTP_SKIN_HOST_V1_SIZE offsetof(TtpSkinHost, drag)

typedef struct TtpSkinWindows {
    uint32_t size;
    HWND player, playlist, equalizer;
    HWND lyrics; // Optional content surface; native window remains the fallback.
} TtpSkinWindows;
#define TTP_SKIN_WINDOWS_V1_SIZE offsetof(TtpSkinWindows, lyrics)

typedef struct TtpSkinInfo {
    uint32_t size;
    wchar_t name[128], author[128];
} TtpSkinInfo;

typedef struct TtpSkinLayout {
    uint32_t size;
    RECT windows[3]; // Screen rectangles: player, playlist, equalizer. Empty = default.
    wchar_t state[256]; // Provider-owned versioned text; host stores it without parsing.
} TtpSkinLayout;

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
    // Optional UI-thread snapshot. restore=TRUE stages state before attach;
    // FALSE captures live or last non-minimized state. No filesystem access.
    HRESULT (WINAPI *layout)(void*, TtpSkinLayout*, BOOL restore);
    // Optional ownership query after attach, including added content surfaces.
    BOOL (WINAPI *handles)(void*, HWND);
    // Optional menu factory/dispatch. command=0 transfers an HMENU to the host;
    // otherwise executes its selected ID. The host owns the modal menu loop,
    // so providers never remain on the stack while a skin can be unloaded.
    HMENU (WINAPI *menu)(void*, HWND, uint32_t command);
    // Query content geometry/state; apply=TRUE changes mode/type only.
    BOOL (WINAPI *content_state)(void*, TtpSkinContent*, BOOL apply);
    // Optional package defaults. window=nullptr queries before attach; the
    // host overlays saved/user colours and uses those for both UI and paint.
    BOOL (WINAPI *lyric_colors)(void*, HWND, TtpSkinLyricColors*);
    // Optional default LOGFONT height for window lyrics (negative = glyph
    // pixels, zero = host default). Query before attach, then overlay the
    // user's saved lyric font; never override it during painting.
    int32_t (WINAPI *lyric_font_height)(void*);
} TtpSkinPlugin;
#define TTP_SKIN_PLUGIN_V1_SIZE offsetof(TtpSkinPlugin, skin_directory)
#define TTP_SKIN_PLUGIN_DECLARATION_SIZE offsetof(TtpSkinPlugin, layout)

typedef HRESULT (WINAPI *TtpGetSkinPlugin)(uint32_t, TtpSkinPlugin*);
