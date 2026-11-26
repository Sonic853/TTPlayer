#pragma once

#include <windows.h>
#include <objidl.h>

namespace ttplayer::plugins {

inline constexpr int kDefaultFileBufferBytes = 16 * 1024;

// 004C4136 returns this secondary interface on the same local file IStream.
inline constexpr GUID kFileBufferInterface{
    0x1717a4a7, 0xa3dc, 0x416b, {0xa2, 0x97, 0x72, 0x16, 0x74, 0x9b, 0xd7, 0xbf}};
struct LegacyFileBuffer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCapacity(DWORD* bytes) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCapacity(DWORD bytes) = 0;
};

// 004C51D3, also exported by the host EXE for old AddIns using temporary WAVs.
HRESULT CreateLegacyFileStream(const wchar_t* path, DWORD mode, IStream** output) noexcept;

// Built-in readers use the same optional interface as Sound AddIns. Streams
// backed by a download or an archive member own their own buffering.
HRESULT SetLegacyFileBuffer(IStream* stream, int bytes) noexcept;

} // namespace ttplayer::plugins
