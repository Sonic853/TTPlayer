#pragma once
#include <windows.h>
#include <oaidl.h>
namespace ttplayer::audio {
HRESULT GetOsuDownloadPage(BSTR* name,BSTR* url) noexcept;
HRESULT RecognizeOsuDownload(const wchar_t* page,const wchar_t* target,BSTR* filename) noexcept;
HRESULT GetOsuPageScript(BSTR* script) noexcept;
HRESULT RecognizeOsuPageAudio(const wchar_t* page,const wchar_t* target) noexcept;
}
