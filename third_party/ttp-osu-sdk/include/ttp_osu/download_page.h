#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

// Independent, optional ABI. The plugin owns the page and URL recognition;
// the host owns the browser, HTTP transport, playlist and download queue.
#define TTP_OSU_DOWNLOAD_PAGE_ABI 2u
#define TTP_OSU_DOWNLOAD_PAGE_ENTRY "ttp_osu_download_page_query"
#pragma pack(push, 8)
typedef struct TtpOsuDownloadPage {
    uint32_t size;
    const wchar_t* name; // borrowed until DLL unload
    const wchar_t* url;
} TtpOsuDownloadPage;
typedef struct TtpOsuDownloadPageApi {
    uint32_t size, abi;
    HRESULT (WINAPI* page)(TtpOsuDownloadPage*);
    // S_OK: recognized .osz download, filename written to caller-owned buffer.
    // S_FALSE: unrelated page/link (including all preview audio).
    HRESULT (WINAPI* recognize)(const wchar_t* page_url, const wchar_t* target_url,
        wchar_t* filename, uint32_t capacity);
    // ABI 2: document-start adapter and strict audio URL recognition.
    const wchar_t* (WINAPI* script)(); // borrowed until DLL unload
    HRESULT (WINAPI* recognize_audio)(const wchar_t* page_url, const wchar_t* audio_url);
} TtpOsuDownloadPageApi;
#define TTP_OSU_DOWNLOAD_PAGE_ABI1_SIZE offsetof(TtpOsuDownloadPageApi, script)
#pragma pack(pop)
typedef HRESULT (WINAPI* TtpOsuQueryDownloadPage)(uint32_t, TtpOsuDownloadPageApi*);
