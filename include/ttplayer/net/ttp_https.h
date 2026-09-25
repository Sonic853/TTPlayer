#ifndef TTP_HTTPS_H
#define TTP_HTTPS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TTP_HTTPS_ABI_VERSION 2u
#define TTP_HTTPS_OK 0
#define TTP_HTTPS_USE_WINHTTP 1 /* Proxy requires the caller's native transport. */
#define TTP_HTTPS_ERROR (-1)
#define TTP_HTTPS_CANCELED (-2)
#define TTP_HTTPS_TLS12 0x0303u
#define TTP_HTTPS_TLS13 0x0304u

typedef int (__cdecl *ttp_https_cancel_fn)(void *context);
typedef struct ttp_https_request {
    uint32_t size;
    const wchar_t *url;
    int32_t proxy_type; /* 0 direct, 1 IE/PAC, >1 explicit HTTP/SOCKS proxy. */
    const wchar_t *proxy_server;
    int32_t proxy_port; /* 0 default. */
    int32_t proxy_has_credentials; /* ABI 1 compatibility; ABI 2 uses username. */
    ttp_https_cancel_fn canceled; /* Must not throw; nonzero cancels. */
    void *cancel_context;
    /* Optional explicit private CA, never a verification bypass. NULL uses the
       built-in Mozilla roots. The player always leaves these fields zero. */
    const unsigned char *ca_pem;
    size_t ca_pem_size; /* Includes the terminating NUL. */
    uint32_t min_tls_version; /* 0 -> TLS 1.2. */
    uint32_t max_tls_version; /* 0 -> TLS 1.3. */
    /* ABI 2: used only for explicit proxies, never sent to the HTTPS origin.
       Server accepts host[:port], http://, socks4://, socks4a:// or socks5://.
       SOCKS5 (also socks5h://) resolves destination names at the proxy.
       A positive proxy_port overrides the server's port. */
    const wchar_t *proxy_username;
    const wchar_t *proxy_password;
} ttp_https_request;

typedef struct ttp_https_response {
    uint32_t size;
    const unsigned char *body;
    size_t body_size;
    const char *title_header; /* Raw tt-title (UTF-16LE hex), or empty string. */
    const char *url_header; /* Raw tt-url, or empty string. */
    uint32_t tls_version;
    uint32_t verify_flags;
    int32_t tls_error;
    char ciphersuite[96];
    void *owner; /* Opaque DLL allocation. Only release() may free it. */
} ttp_https_response;

typedef struct ttp_https_api {
    uint32_t size;
    uint32_t abi_version;
    const char *library_version;
    const char *ca_bundle_version;
    /* Synchronous HTTPS GET, bounded to 2 MiB; call on a worker thread.
       Initialize response to zero and set size. Release it after every call.
       An error after TLS starts must not be treated as a request to fall back. */
    int (__cdecl *get)(const ttp_https_request *, ttp_https_response *, char *error, size_t error_size);
    void (__cdecl *release)(ttp_https_response *);
} ttp_https_api;
typedef const ttp_https_api *(__cdecl *ttp_https_get_api_fn)(uint32_t version);
const ttp_https_api *__cdecl ttp_https_get_api(uint32_t version);

/* Optional streaming extension. ABI 1/2 and the 2 MiB get() contract are
   unchanged. Query version 3, validate base.size, then cast to this table.
   The sink runs synchronously on the calling worker. Return 0 on write error;
   no exceptions may cross the ABI. total is 0 for chunked/unknown lengths. */
#define TTP_HTTPS_DOWNLOAD_ABI_VERSION 3u
typedef int (__cdecl *ttp_https_write_fn)(void *context, const unsigned char *bytes,
    size_t size, uint64_t received, uint64_t total);
typedef struct ttp_https_download_request {
    uint32_t size;
    ttp_https_request request;
    uint64_t max_size; /* 1 .. 256 MiB; caller must also verify its expected size. */
    ttp_https_write_fn write;
    void *write_context;
} ttp_https_download_request;
typedef struct ttp_https_api_v3 {
    ttp_https_api base;
    int (__cdecl *download)(const ttp_https_download_request *, char *error, size_t error_size);
} ttp_https_api_v3;

/* All pointers in the response remain valid until release(). No C++ objects,
   exceptions or CRT ownership cross the ABI. Independent calls may run in
   parallel. Keep the DLL loaded until every call and response is finished. */
#ifdef __cplusplus
}
#endif
#endif
