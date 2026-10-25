#pragma once
#include <stdint.h>
#include <stddef.h>
#include <windows.h>
#include "abi.h"

// Independent of OSZ ABI 1. Caller initializes size; borrowed inputs are copied
// before return. No exceptions or allocator ownership cross the DLL boundary.
#define TTP_OSU_VISUAL_ABI 3u
#define TTP_OSU_VISUAL_ENTRY "ttp_osu_query_visual"
#pragma pack(push, 8)
enum TtpOsuVisualFlags {
    TTP_OSU_SPECTRUM=1, TTP_OSU_STARS=2, TTP_OSU_FLASH=4,
    TTP_OSU_PARALLAX=8, TTP_OSU_LOGO=16
};
typedef struct TtpOsuVisualOptions {
    uint32_t size, flags, seed;
    COLORREF accent, background;
    double fallback_bpm;
} TtpOsuVisualOptions;
typedef struct TtpOsuVisualFrame {
    uint32_t size, playback; // 0 stopped, 1 opening, 2 playing, 3 paused, 4 failed
    uint64_t now_ms, timeline, revision;
    double position_ms;
    uint32_t sample_rate, count;
    const int16_t* left;
    const int16_t* right; // null means mono
    float volume; // 0..1; apply once to peak envelope, not to FFT
    float pointer_x, pointer_y; // normalized client coordinates 0..1
    uint32_t pointer_inside;
    uint32_t pcm_cursor_offset; // actual playback cursor within the PCM window
} TtpOsuVisualFrame;
typedef struct TtpOsuVisualImage {
    uint32_t size, width, height, stride;
    const uint8_t* bgra; // premultiplied BGRA; copied synchronously
} TtpOsuVisualImage;
typedef struct TtpOsuVisualStats {
    uint32_t size, particles, bursts, fft_count, timing_points;
    uint32_t kiai, backend; // backend: 0 not rendered, 1 CPU, 2 D3D9
    float logo_scale, left_flash, right_flash;
    uint64_t revision;
} TtpOsuVisualStats;
typedef struct TtpOsuVisualTransitionState {
    uint32_t size, phase, complete, duration_ms;
} TtpOsuVisualTransitionState;
typedef struct TtpOsuVisualApi {
    uint32_t size, abi;
    HRESULT (WINAPI* create)(void**);
    void (WINAPI* destroy)(void*);
    HRESULT (WINAPI* configure)(void*, const TtpOsuVisualOptions*);
    // UTF-8 .osu bytes; null/zero selects ordinary-audio timing. Atomic replace.
    HRESULT (WINAPI* timing)(void*, const uint8_t*, uint32_t);
    // Uses existing read-only OSZ callbacks. Explicit audio selects a stable
    // first matching beatmap; absent timing clears the previous track's timing.
    HRESULT (WINAPI* archive_timing)(void*, const TtpOsuHost*, const wchar_t*, const wchar_t*);
    HRESULT (WINAPI* image)(void*, const TtpOsuVisualImage*); // null clears
    HRESULT (WINAPI* update)(void*, const TtpOsuVisualFrame*);
    HRESULT (WINAPI* paint)(void*, HDC, const RECT*);
    // UI/window thread only. D3D9 is loaded on demand. Failure permits CPU paint.
    HRESULT (WINAPI* present)(void*, HWND);
    // UI/window thread only, before HWND destruction/reparent or DLL unload.
    void (WINAPI* detach)(void*);
    HRESULT (WINAPI* stats)(void*, TtpOsuVisualStats*);
    // ABI 2: 0 embedded TTPlayer, 1 embedded osu!, 2 custom PBGRA image.
    // Built-ins require null; custom input is copied before return. Failure
    // preserves the current logo. Aspect ratio is retained for custom images.
    HRESULT (WINAPI* logo)(void*, uint32_t, const TtpOsuVisualImage*);
    // ABI 3: 0 cancels, 1 starts the original welcome, 2 starts departure.
    // now_ms uses the same steady millisecond clock as update(), independent
    // of audio position and playback state. Repeating an active phase does not
    // restart it. Departure may interrupt welcome; its final frame stays black.
    HRESULT (WINAPI* transition)(void*, uint32_t, uint64_t now_ms);
    // Advancing this clock also permits transitions while audio is stopped.
    // Completed welcome reports phase 0; completed departure remains phase 2.
    HRESULT (WINAPI* transition_state)(void*, uint64_t now_ms, TtpOsuVisualTransitionState*);
} TtpOsuVisualApi;
#define TTP_OSU_VISUAL_ABI1_SIZE ((uint32_t)offsetof(TtpOsuVisualApi, logo))
#define TTP_OSU_VISUAL_ABI2_SIZE ((uint32_t)offsetof(TtpOsuVisualApi, transition))
#pragma pack(pop)
typedef HRESULT (WINAPI* TtpOsuQueryVisual)(uint32_t, TtpOsuVisualApi*);
