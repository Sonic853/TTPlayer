#pragma once
#include <windows.h>
#include <objidl.h>
#include <stdint.h>

/* Additive, read-only archive service. Strings are UTF-16. Entry names are
   borrowed only during the visitor call. Returned IStreams own their storage;
   Release them before unloading the DLL. No CRT/STL ownership crosses the ABI.
   Callbacks return FALSE to cancel and must not reenter this service. */
#define TTPCOMM_ARCHIVE_ABI 1u
#define TTPCOMM_ARCHIVE_RAR4 1u
#define TTPCOMM_ARCHIVE_RAR5 2u
#define TTPCOMM_ARCHIVE_REQUIRED 3u
#define TTPCOMM_ARCHIVE_DIRECTORY 1u
#define TTPCOMM_ARCHIVE_ENCRYPTED 2u
#define TTPCOMM_ARCHIVE_SOLID 4u
#define TTPCOMM_ARCHIVE_LINK 8u
#define TTPCOMM_ARCHIVE_SPLIT 16u
#define TTPCOMM_ARCHIVE_SPARSE 32u
/* Unknown only during enumeration / progressive gzip decode; Stat waits for
   verified completion and always returns the real length. */
#define TTPCOMM_ARCHIVE_UNKNOWN_SIZE 64u
#define TTPCOMM_ARCHIVE_PASSWORD ((HRESULT)0x8004A001L)
#define TTPCOMM_ARCHIVE_BAD_PASSWORD ((HRESULT)0x8004A002L)
#define TTPCOMM_ARCHIVE_MISSING_VOLUME ((HRESULT)0x8004A003L)
#define TTPCOMM_ARCHIVE_DICTIONARY_LIMIT ((HRESULT)0x8004A004L)
#define TTPCOMM_ARCHIVE_UNSUPPORTED ((HRESULT)0x8004A005L)

#pragma pack(push, 8)
typedef struct TtpCommArchiveOptions {
    uint32_t size;
    uint32_t dictionary_limit_kib; /* 0: 256 MiB; hard ceiling 512 MiB. */
    uint64_t member_limit_bytes;   /* 0: 8 GiB; applies before/during decode. */
    uint32_t memory_limit_bytes;   /* 0: 16 MiB; larger streams use delete-on-close files. */
    uint32_t entry_limit;          /* 0: 100000. */
    const wchar_t* password;       /* Optional; never persisted by the service. */
    BOOL (__cdecl *progress)(void* context, uint64_t completed, uint64_t total);
    void* context;
} TtpCommArchiveOptions;

typedef struct TtpCommArchiveEntry {
    uint32_t size, index, flags, dictionary_kib;
    uint64_t unpacked_bytes, packed_bytes;
    const wchar_t* name;
} TtpCommArchiveEntry;

typedef BOOL (__cdecl *TtpCommArchiveVisitor)(void*, const TtpCommArchiveEntry*);
typedef struct TtpCommArchiveApi {
    uint32_t size, abi_version, capabilities, reserved;
    HRESULT (__cdecl *enumerate)(const wchar_t*, const TtpCommArchiveOptions*,
                                TtpCommArchiveVisitor, void*);
    HRESULT (__cdecl *open_member)(const wchar_t*, const wchar_t*,
                                  const TtpCommArchiveOptions*, IStream**);
} TtpCommArchiveApi;
#pragma pack(pop)
typedef BOOL (__cdecl *TtpCommQueryArchiveFn)(uint32_t abi, TtpCommArchiveApi*);

/* ABI 2 is available at ttpcomm_query_archive_stream (ordinal 503), and at
   ttpcomm_query_archive with ABI 2. ABI 1 stays synchronous and unchanged.
   Each stream owns its cancellation callback. It must outlive that stream
   and its clones; only caller threads poll it, never the cached producer.
   Release all streams and call clear_cache before unloading the DLL. */
#define TTPCOMM_ARCHIVE_STREAM_ABI 2u
#define TTPCOMM_ARCHIVE_ZIP 4u
#define TTPCOMM_ARCHIVE_PROGRESSIVE 8u
#define TTPCOMM_ARCHIVE_STREAM_REQUIRED 15u
#define TTPCOMM_ARCHIVE_TAR 16u
#define TTPCOMM_ARCHIVE_GZIP 32u
#define TTPCOMM_ARCHIVE_7Z 64u
#define TTPCOMM_ARCHIVE_ZIP_UTF8 128u
#define TTPCOMM_ARCHIVE_ZIP64 256u
#define TTPCOMM_ARCHIVE_EXTENDED_REQUIRED 511u
/* For stream open_member, encrypted members are exposed only after complete
   CRC/authentication verification. A supplied password does not force an
   unencrypted member to finish decoding before its stream can be returned. */
#define TTPCOMM_ARCHIVE_CAP_PASSWORDS 512u
#define TTPCOMM_ARCHIVE_CAP_SIGNATURE 1024u
#define TTPCOMM_ARCHIVE_PASSWORD_REQUIRED 2047u
#define TTPCOMM_ARCHIVE_CAP_EXTRA_CODECS 2048u /* 7z BZip2 / Zstandard */
#define TTPCOMM_ARCHIVE_IMPORT_REQUIRED 4095u
#define TTPCOMM_ARCHIVE_CAP_MULTIVOLUME 4096u /* ZIP disk offsets; numeric ZIP/7z volumes */
#define TTPCOMM_ARCHIVE_VOLUMES_REQUIRED 8191u
#define TTPCOMM_ARCHIVE_CAP_VOLUME_RESOLVER 8192u
#define TTPCOMM_ARCHIVE_GROUP_REQUIRED 16383u
#define TTPCOMM_ARCHIVE_CACHE_CURRENT 1u
#pragma pack(push, 8)
typedef struct TtpCommArchiveStreamOptions {
    uint32_t size, flags;
    TtpCommArchiveOptions limits; /* progress/context unused; use canceled. */
    BOOL (__cdecl *canceled)(void* context);
    void* context;
    uint32_t timeout_ms; /* 0: 30000 per wait. */
} TtpCommArchiveStreamOptions;
typedef struct TtpCommArchiveStreamStatus {
    uint32_t size, complete;
    uint64_t completed_bytes, total_bytes, task_id;
    HRESULT result; /* E_PENDING until verified; failure is terminal. */
} TtpCommArchiveStreamStatus;
typedef struct TtpCommArchiveStreamApi {
    uint32_t size, abi_version, capabilities, reserved;
    HRESULT (__cdecl *enumerate)(const wchar_t*, const TtpCommArchiveOptions*,
                                TtpCommArchiveVisitor, void*);
    HRESULT (__cdecl *open_member)(const wchar_t*, const wchar_t*,
                                  const TtpCommArchiveStreamOptions*, IStream**);
    HRESULT (__cdecl *status)(IStream*, TtpCommArchiveStreamStatus*);
    void (__cdecl *clear_cache)(void); /* Active streams remain valid. */
} TtpCommArchiveStreamApi;
#pragma pack(pop)
typedef BOOL (__cdecl *TtpCommQueryArchiveStreamFn)(uint32_t, TtpCommArchiveStreamApi*);

/* ABI 3 uses the same query export with a separate table. ABI 1/2 sizes stay
   unchanged. Resolve/visitor callbacks run synchronously on the calling thread,
   before a stream producer starts. No resolver/context is retained by streams.
   Missing-volume replacement is read-only and still subject to codec checks.
   S_OK supplies a NUL-terminated absolute path; S_FALSE declines; failures abort.
   Volume visitors see the actual ordered files, only after complete discovery. */
#define TTPCOMM_ARCHIVE_GROUP_ABI 3u
#pragma pack(push, 8)
typedef struct TtpCommArchiveVolumeOptions {
    uint32_t size;
    HRESULT (__cdecl *resolve)(void* context, const wchar_t* archive,
        const wchar_t* expected, wchar_t* replacement, uint32_t capacity);
    void* context;
} TtpCommArchiveVolumeOptions;
typedef BOOL (__cdecl *TtpCommArchiveVolumeVisitor)(void*, const wchar_t* actual);
typedef struct TtpCommArchiveGroupApi {
    uint32_t size, abi_version, capabilities, reserved;
    HRESULT (__cdecl *enumerate)(const wchar_t*, const TtpCommArchiveOptions*,
        const TtpCommArchiveVolumeOptions*, TtpCommArchiveVisitor, void*);
    HRESULT (__cdecl *open_member)(const wchar_t*, const wchar_t*,
        const TtpCommArchiveStreamOptions*, const TtpCommArchiveVolumeOptions*, IStream**);
    HRESULT (__cdecl *volumes)(const wchar_t*, const TtpCommArchiveOptions*,
        const TtpCommArchiveVolumeOptions*, TtpCommArchiveVolumeVisitor, void*);
    HRESULT (__cdecl *status)(IStream*, TtpCommArchiveStreamStatus*);
    void (__cdecl *clear_cache)(void);
} TtpCommArchiveGroupApi;
#pragma pack(pop)
typedef BOOL (__cdecl *TtpCommQueryArchiveGroupFn)(uint32_t, TtpCommArchiveGroupApi*);

#if defined(__cplusplus) && !defined(CINTERFACE)
/* The status service queries this interface on IStream. A stream decorator
   must use its own controlling IUnknown and retain callback contexts until
   every exposed interface is released. This is the existing ABI 2 status
   GUID and vtable; no function-table layout or export has changed. */
static const IID IID_TtpCommArchiveStreamStatus =
    {0x4d4a30fe,0x8b93,0x44d7,{0xa6,0x72,0x73,0x4d,0x6f,0x4b,0xc5,0x36}};
struct TtpCommArchiveStreamStatusInterface : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Snapshot(TtpCommArchiveStreamStatus*) = 0;
};
#endif
