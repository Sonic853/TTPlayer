#pragma once
#include <ttp_osu/library.h>

// Independent ABI: exports complete beatmap sets, with original member bytes
// and relative filenames. No game files or databases are modified.
#define TTP_OSU_EXPORT_ABI 1u
#define TTP_OSU_EXPORT_ENTRY "ttp_osu_export_query"
#pragma pack(push, 8)
enum TtpOsuExportFlags {
    TTP_OSU_EXPORT_SINGLE_FILE=1, // destination is an .osz, exactly one set
    TTP_OSU_EXPORT_REPLACE=2      // single-file only; caller confirmed overwrite
};
enum TtpOsuExportPhase { TTP_OSU_EXPORT_PLAN=5, TTP_OSU_EXPORT_WRITE=6 };
typedef struct TtpOsuExportRequest {
    uint32_t size, source;
    const wchar_t* root; // same root used for library enumeration
    const wchar_t* const* identities; // TtpOsuLibraryTrack.identity, nonempty
    uint32_t count, flags;
    const wchar_t* destination; // otherwise an existing output directory
} TtpOsuExportRequest;
typedef HRESULT (WINAPI* TtpOsuExported)(void*,const wchar_t* path);
typedef struct TtpOsuExportApi {
    uint32_t size, abi;
    // Synchronous, cancelable through host. Dedupe by set. Each archive is
    // committed atomically; completed callbacks remain valid if a later set
    // fails/cancels. Batch destinations never overwrite: append (2), (3), ...
    HRESULT (WINAPI* run)(const TtpOsuLibraryHost*,const TtpOsuExportRequest*,TtpOsuExported,void*);
} TtpOsuExportApi;
#pragma pack(pop)
typedef HRESULT (WINAPI* TtpOsuQueryExport)(uint32_t,TtpOsuExportApi*);
