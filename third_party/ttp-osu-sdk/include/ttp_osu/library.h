#pragma once
#include <stdint.h>
#include <windows.h>

// Independent, synchronous, read-only installed-song enumeration.
// ABI strings are UTF-16 and borrowed only for the duration of a callback.
// No STL objects, exceptions, memory ownership or database handles cross ABI.
#define TTP_OSU_LIBRARY_ABI 1u
#define TTP_OSU_LIBRARY_ENTRY "ttp_osu_library_query"
#pragma pack(push, 8)
enum TtpOsuLibrarySource { TTP_OSU_LIBRARY_STABLE=1, TTP_OSU_LIBRARY_LAZER=2 };
enum TtpOsuLibraryPhase { TTP_OSU_LIBRARY_DISCOVER=1, TTP_OSU_LIBRARY_SCAN=2,
    TTP_OSU_LIBRARY_READ=3, TTP_OSU_LIBRARY_DELIVER=4 };
typedef struct TtpOsuLibraryRequest {
    uint32_t size, source;
    // stable installation/Songs directory, or lazer data directory containing
    // client.realm and files. NULL/empty asks the DLL to discover the location.
    const wchar_t* root;
    uint32_t flags; // ABI 1: zero
} TtpOsuLibraryRequest;
typedef struct TtpOsuLibraryHost {
    uint32_t size;
    void* context;
    BOOL (WINAPI* canceled)(void*);
    // total=0 means unknown during discovery. Returning failure cancels scan.
    HRESULT (WINAPI* progress)(void*,uint32_t phase,uint64_t done,uint64_t total,const wchar_t* current);
} TtpOsuLibraryHost;
typedef struct TtpOsuLibraryTrack {
    uint32_t size, source;
    const wchar_t* identity;
    const wchar_t* audio;       // absolute physical path, not a temporary copy
    const wchar_t* beatmap;     // representative .osu (possibly hashed lazer file)
    const wchar_t* background;  // absolute image path; empty when unavailable
    const wchar_t* title;       // Unicode title preferred, then ordinary title
    const wchar_t* artist;      // Unicode artist preferred
    const wchar_t* album;       // beatmap Source; may be empty
    const wchar_t* creator;
    uint64_t duration_ms;       // zero when unknown; host may read audio tags
    uint64_t file_size;
    int32_t beatmap_set_id;     // -1 when not published/known
} TtpOsuLibraryTrack;
typedef HRESULT (WINAPI* TtpOsuVisitLibraryTrack)(void*,const TtpOsuLibraryTrack*);
typedef struct TtpOsuLibraryApi {
    uint32_t size, abi;
    // Scan completes before visiting records. The host must stage callbacks
    // and atomically replace its dedicated playlist only after S_OK: a host
    // cancellation or visitor failure during delivery can stop at any record.
    HRESULT (WINAPI* enumerate)(const TtpOsuLibraryHost*,const TtpOsuLibraryRequest*,
        TtpOsuVisitLibraryTrack,void*);
} TtpOsuLibraryApi;
#pragma pack(pop)
typedef HRESULT (WINAPI* TtpOsuQueryLibrary)(uint32_t,TtpOsuLibraryApi*);
