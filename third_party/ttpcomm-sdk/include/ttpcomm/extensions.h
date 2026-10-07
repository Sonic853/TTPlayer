#pragma once
#include <stdint.h>

/* Optional extension. The original 67 exports and 0x50700 version stay intact.
   Consumers query by name; absence means use the original ABI. No ownership
   crosses this boundary. Capabilities describe this DLL, not reader plugins. */
#define TTPCOMM_EXTENSION_ABI 1u
#define TTPCOMM_CAP_PARALLEL_REPLAYGAIN 0x00000001u
typedef struct TtpCommExtensionInfo {
    uint32_t size;
    uint32_t abi_version;
    uint32_t legacy_version;
    uint32_t capabilities;
} TtpCommExtensionInfo;
typedef int (__cdecl *TtpCommQueryExtensionFn)(uint32_t, TtpCommExtensionInfo*);

