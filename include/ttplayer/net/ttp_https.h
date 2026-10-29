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

/* ABI 4 adds structured HTTP GET without changing ABI 1/2/3 structures.
   Header values are ASCII without control characters, at most 512 bytes.
   Transport success returns HTTP errors (404/429/503 etc.) in http_status. */
#define TTP_HTTPS_HTTP_ABI_VERSION 4u
typedef struct ttp_https_http_request {
    uint32_t size;
    ttp_https_request request;
    const char *user_agent;
    const char *accept;
} ttp_https_http_request;
typedef struct ttp_https_http_response {
    uint32_t size;
    ttp_https_response response;
    uint32_t http_status;
    const char *retry_after;
} ttp_https_http_response;
typedef struct ttp_https_api_v4 {
    ttp_https_api_v3 base;
    int (__cdecl *get_http)(const ttp_https_http_request *, ttp_https_http_response *, char *, size_t);
    void (__cdecl *release_http)(ttp_https_http_response *);
} ttp_https_api_v4;

/* ABI 5: a single HTTPS exchange. Redirects are returned, never followed, so
   the caller can scope cookies to each destination. No global cookie store. */
#define TTP_HTTPS_EXCHANGE_ABI_VERSION 5u
typedef struct ttp_https_exchange_request {
    uint32_t size;
    ttp_https_request request;
    const char *cookie; /* Optional printable ASCII Cookie value, <= 16384 bytes. */
} ttp_https_exchange_request;
typedef struct ttp_https_exchange_response {
    uint32_t size;
    ttp_https_response response;
    uint32_t http_status;
    const char *location;
    const char *const *set_cookies;
    uint32_t cookie_count;
} ttp_https_exchange_response;
typedef struct ttp_https_api_v5 {
    ttp_https_api_v4 base;
    int (__cdecl *exchange)(const ttp_https_exchange_request *, ttp_https_exchange_response *, char *, size_t);
    void (__cdecl *release_exchange)(ttp_https_exchange_response *);
} ttp_https_api_v5;

/* ABI 6 preserves every earlier table and structure. One exchange, with
   caller-controlled headers and response metadata needed for full-file checks.
   ALL reads bounded bodies for all statuses; SUCCESS reads only 2xx bodies.
   HTTP statuses that cannot have a body always complete at the header boundary.
   Optional header values are printable ASCII, <= 512 bytes (Referer <= 8192).
   NULL UA/Accept retain the defaults; NULL/empty Referer omits that header. */
#define TTP_HTTPS_EXCHANGE_EX_ABI_VERSION 6u
#define TTP_HTTPS_BODY_ALL 0u
#define TTP_HTTPS_BODY_SUCCESS 1u
typedef struct ttp_https_exchange_ex_request {
    uint32_t size;
    ttp_https_exchange_request exchange;
    const char *user_agent;
    const char *accept;
    const char *referer;
    uint32_t body_policy;
} ttp_https_exchange_ex_request;
typedef struct ttp_https_exchange_ex_response {
    uint32_t size;
    ttp_https_exchange_response exchange;
    const char *content_range; /* Raw Content-Range, or empty. Same ownership. */
} ttp_https_exchange_ex_response;
typedef struct ttp_https_api_v6 {
    ttp_https_api_v5 base;
    int (__cdecl *exchange_ex)(const ttp_https_exchange_ex_request *, ttp_https_exchange_ex_response *, char *, size_t);
    void (__cdecl *release_exchange_ex)(ttp_https_exchange_ex_response *);
} ttp_https_api_v6;

/* ABI 7: pull-based HTTP/HTTPS media transport. One response per handle;
   redirects are returned to the caller. No body/file size or lifetime limit.
   open/read run on a caller-owned worker; close must not race another call.
   Request strings and CA bytes are copied. The cancellation callback/context
   must remain alive through close. Headers live until close. A successful
   read with zero bytes is EOF; short positive reads are NOT EOF. */
#define TTP_HTTPS_STREAM_ABI_VERSION 7u
#define TTP_HTTPS_TIMEOUT (-3)
#define TTP_HTTPS_TRUNCATED (-4)
typedef struct ttp_https_stream ttp_https_stream;
typedef struct ttp_https_stream_request {
    uint32_t size;
    ttp_https_request request;
    uint64_t offset;
    const char *if_range; /* Optional strong ETag or Last-Modified, <= 512 bytes. */
    uint32_t timeout_ms; /* Per open/read idle budget, 100..60000; 0 -> 15000. */
    int32_t icy_metadata;
    /* Optional explicit PAC configuration for a caller with its own settings.
       NULL and -1 use current-user IE settings. No registry writes. */
    const wchar_t *auto_config_url;
    int32_t auto_detect;
} ttp_https_stream_request;
typedef struct ttp_https_api_v7 {
    ttp_https_api_v6 base;
    int (__cdecl *stream_open)(const ttp_https_stream_request *, ttp_https_stream **,
        uint32_t *http_status, char *error, size_t error_size);
    const char *(__cdecl *stream_header)(ttp_https_stream *, const char *lowercase_name);
    int (__cdecl *stream_read)(ttp_https_stream *, void *, uint32_t capacity,
        uint32_t *read, char *error, size_t error_size);
    void (__cdecl *stream_close)(ttp_https_stream *);
} ttp_https_api_v7;

/* ABI 8: single HTTP/HTTPS GET or POST, with explicit origin headers.
   Unlike ABI 7, this does not send Range or ICY headers. No redirects, cookies,
   decompression or origin authentication retries. POST bodies are <= 1 MiB;
   the body is sent synchronously during open and may be released on return.
   Optional headers are printable ASCII; Authorization <= 16384 bytes, the
   others <= 512. NULL method means GET; GET must have no body. Read/header/close
   use the ABI 7 functions. Response limits belong to the caller.
   USE_WINHTTP is only returned before sending an origin request, when native
   proxy handling is required. TLS/HTTP/read failures never request fallback. */
#define TTP_HTTPS_REQUEST_ABI_VERSION 8u
typedef struct ttp_https_stream_request_ex {
    uint32_t size;
    ttp_https_stream_request stream;
    const char *method;
    const char *user_agent;
    const char *accept;
    const char *content_type;
    const char *authorization;
    const unsigned char *body;
    uint32_t body_size;
} ttp_https_stream_request_ex;
typedef struct ttp_https_api_v8 {
    ttp_https_api_v7 base;
    int (__cdecl *stream_open_ex)(const ttp_https_stream_request_ex *, ttp_https_stream **,
        uint32_t *http_status, char *error, size_t error_size);
} ttp_https_api_v8;

/* ABI 9 adds PROPFIND with Depth 0/1 and authenticated ranged GET.
   ABI 7/8 contracts are unchanged. No automatic redirects or origin retries. */
#define TTP_HTTPS_WEBDAV_ABI_VERSION 9u
#define TTP_HTTPS_REQUEST_RANGE 1u
typedef struct ttp_https_stream_request_ex2 {
    uint32_t size;
    ttp_https_stream_request_ex request;
    const char *depth;
    uint32_t flags;
} ttp_https_stream_request_ex2;
typedef struct ttp_https_api_v9 {
    ttp_https_api_v8 base;
    int (__cdecl *stream_open_ex2)(const ttp_https_stream_request_ex2 *, ttp_https_stream **,
        uint32_t *http_status, char *error, size_t error_size);
} ttp_https_api_v9;

/* All pointers in the response remain valid until release(). No C++ objects,
   exceptions or CRT ownership cross the ABI. Independent calls may run in
   parallel. Keep the DLL loaded until every call and response is finished. */
#ifdef __cplusplus
}
#endif
#endif
