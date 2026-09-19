#pragma once
#include <stdint.h>
#include <wchar.h>

// Shared, versioned C ABI. No CRT allocations or C++ objects cross this boundary.
#define TTP_I18N_ABI_VERSION 1u
#ifdef _WIN32
#define TTP_I18N_CALL __cdecl
#else
#define TTP_I18N_CALL
#endif

typedef struct TtpI18nApi {
    uint32_t size;
    uint32_t version;
    void* (TTP_I18N_CALL *open)(const wchar_t* directory, const wchar_t* language);
    void (TTP_I18N_CALL *close)(void* catalog);
    // UTF-8 keys; NULL context means absent, "" means explicit empty msgctxt.
    // UTF-16 output on Windows. Returns required wchar_t count,
    // including the terminator, or zero on a miss. Query with output=NULL.
    // Resource contexts (ttpres|exe/string|menu|dialog/...) may also match a
    // unique singular entry by context when the loaded resource language differs.
    // The host validates the result against the actual resource format. Other
    // contexts retain exact gettext matching. Older v1 providers can simply miss.
    uint32_t (TTP_I18N_CALL *lookup)(const void* catalog, const char* context,
        const char* singular, const char* plural, uint64_t count,
        wchar_t* output, uint32_t capacity);
} TtpI18nApi;

typedef const TtpI18nApi* (TTP_I18N_CALL *TtpI18nGetApiFn)(uint32_t version);
