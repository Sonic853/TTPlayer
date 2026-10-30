#pragma once
#include <ttp_osu/abi.h>
#include <objidl.h>

// Independent import ABI. The game is always given a verified physical copy.
// Calls are synchronous: run on a worker. Borrowed strings last until callback
// return. No STL, exceptions, ownership or Realm handles cross this boundary.
#define TTP_OSU_IMPORT_ABI 1u
#define TTP_OSU_IMPORT_ENTRY "ttp_osu_import_query"
#pragma pack(push, 8)
enum TtpOsuImportFlags {
    TTP_OSU_IMPORT_KEEP_SOURCE=1,
    TTP_OSU_IMPORT_SOURCE_REFERENCED=2
};
enum TtpOsuImportPhase {
    TTP_OSU_IMPORT_COPY=1, TTP_OSU_IMPORT_CHECK_COPY=2,
    TTP_OSU_IMPORT_MANIFEST=3, TTP_OSU_IMPORT_VERIFY=4,
    TTP_OSU_IMPORT_WAIT=5
};
enum TtpOsuImportState {
    TTP_OSU_IMPORT_CONFIRMED=1, TTP_OSU_IMPORT_PENDING=2
};
enum TtpOsuImportRetention {
    TTP_OSU_IMPORT_RETAINED=1, TTP_OSU_IMPORT_REFERENCED=2,
    TTP_OSU_IMPORT_RECYCLED=3, TTP_OSU_IMPORT_CLEANUP_FAILED=4
};
typedef struct TtpOsuImportRequest {
    uint32_t size, source; // 1 stable, 2 lazer
    const wchar_t* executable;
    const wchar_t* archive;
    uint32_t flags;
    uint32_t wait_ms; // 0: submit without waiting; at most 120000
} TtpOsuImportRequest;
typedef struct TtpOsuImportHost {
    uint32_t size;
    void* context;
    BOOL (WINAPI* canceled)(void*);
    HRESULT (WINAPI* progress)(void*,uint32_t,uint64_t,uint64_t,const wchar_t*);
    HRESULT (WINAPI* list_members)(void*,const wchar_t*,TtpOsuVisitName,void*);
    // Returns one owned IStream reference. Reads must validate member CRC at EOF.
    HRESULT (WINAPI* open_member)(void*,const wchar_t*,const wchar_t*,IStream**);
} TtpOsuImportHost;
typedef struct TtpOsuImportResult {
    uint32_t size, state, retention;
    BOOL already_present;
    HRESULT detail; // latest verification/cleanup error, not launch success
    const wchar_t* task_directory; // durable recovery record; empty if unnecessary
} TtpOsuImportResult;
typedef HRESULT (WINAPI* TtpOsuImported)(void*,const TtpOsuImportResult*);
typedef struct TtpOsuImportApi {
    uint32_t size, abi;
    HRESULT (WINAPI* run)(const TtpOsuImportHost*,const TtpOsuImportRequest*,TtpOsuImported,void*);
} TtpOsuImportApi;
#pragma pack(pop)
typedef HRESULT (WINAPI* TtpOsuQueryImport)(uint32_t,TtpOsuImportApi*);
