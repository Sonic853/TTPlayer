#pragma once
#include <stdint.h>
#include <windows.h>

// ABI 1: synchronous, read-only OSZ expansion. No STL objects, ownership or
// exceptions cross this boundary. Strings are UTF-16; bytes are unmodified.
#define TTP_OSU_ABI_VERSION 1u
#pragma pack(push, 8)
typedef struct TtpOsuMedia {
    const wchar_t* beatmap;
    const wchar_t* audio;
    const wchar_t* background;
} TtpOsuMedia;
typedef HRESULT (WINAPI* TtpOsuVisitMedia)(void*, const TtpOsuMedia*);
typedef HRESULT (WINAPI* TtpOsuVisitBytes)(void*, const unsigned char*, uint32_t);
typedef HRESULT (WINAPI* TtpOsuVisitName)(void*, const wchar_t*);
typedef struct TtpOsuHost {
    uint32_t size;
    void* context;
    // Generic archive I/O. The plugin requests enumeration of the OSZ itself;
    // names retain archive order and exclude directories/links.
    HRESULT (WINAPI* list_members)(void*, const wchar_t*, TtpOsuVisitName, void*);
    // The returned buffer is borrowed until the next read or API return.
    // Complete the member CRC check; never return more than 16 MiB.
    HRESULT (WINAPI* read_member)(void*, const wchar_t*, const wchar_t*, const unsigned char**, uint32_t*);
    BOOL (WINAPI* canceled)(void*);
    HRESULT (WINAPI* progress)(void*, uint64_t, uint64_t);
} TtpOsuHost;
typedef struct TtpOsuApi {
    uint32_t size;
    uint32_t abi;
    // Input is an OSZ archive path, never an individual .osu text file.
    // NULL/empty required_audio expands every
    // distinct audio; otherwise returns only that member's map/background.
    // Visitors run after a successful scan, never for an incomplete scan.
    HRESULT (WINAPI* resolve)(const TtpOsuHost*, const wchar_t*,
                            const wchar_t*, TtpOsuVisitMedia, void*);
    // S_FALSE: no valid referenced image. Host tries embedded art first.
    HRESULT (WINAPI* cover)(const TtpOsuHost*, const wchar_t*,
                          const wchar_t*, TtpOsuVisitBytes, void*);
} TtpOsuApi;
#pragma pack(pop)
typedef HRESULT (WINAPI* TtpOsuQuery)(uint32_t, TtpOsuApi*);
