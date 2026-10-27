#pragma once
#include <stdint.h>
#include <windows.h>

// Read-only installation metadata, independent of the archive/library/visual
// interfaces. UTF-16 strings are borrowed only during a synchronous callback.
#define TTP_OSU_INSTALLATION_ABI 1u
#define TTP_OSU_INSTALLATION_ENTRY "ttp_osu_installation_query"
#pragma pack(push,8)
enum TtpOsuExecutableKind { TTP_OSU_EXECUTABLE_NONE=0,
    TTP_OSU_EXECUTABLE_STABLE=1, TTP_OSU_EXECUTABLE_LAZER=2 };
typedef struct TtpOsuInstallationRequest {
    uint32_t size, source; // 1 = stable, 2 = lazer
    // NULL discovers the default executable. Empty explicitly disables it.
    const wchar_t* executable;
    // Optional lazer roaming-data root; NULL uses the current user's folder.
    const wchar_t* roaming_root;
    uint32_t flags; // ABI 1: zero
} TtpOsuInstallationRequest;
typedef struct TtpOsuInstallation {
    uint32_t size, source, executable_kind;
    const wchar_t* executable;
    // Stable Songs (including BeatmapDirectory), or lazer files directory.
    // Discovery retains offline paths and never creates/modifies game files.
    const wchar_t* library_directory;
} TtpOsuInstallation;
typedef HRESULT (WINAPI* TtpOsuVisitInstallation)(void*,const TtpOsuInstallation*);
typedef struct TtpOsuInstallationApi {
    uint32_t size, abi;
    // S_OK: recognized PE/version identity; S_FALSE: missing/unrecognized.
    // Neither operation loads or runs the supplied executable.
    HRESULT (WINAPI* identify)(const wchar_t*,uint32_t* kind);
    HRESULT (WINAPI* discover)(const TtpOsuInstallationRequest*,TtpOsuVisitInstallation,void*);
} TtpOsuInstallationApi;
#pragma pack(pop)
typedef HRESULT (WINAPI* TtpOsuQueryInstallation)(uint32_t,TtpOsuInstallationApi*);
