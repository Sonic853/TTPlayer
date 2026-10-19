#pragma once

#include <objidl.h>

namespace ttplayer::audio { struct PlaybackOptions; }

namespace ttplayer::plugins {

// TTPlayer 5.7.9's six-slot sound buffer, used by the original ASF/RM AddIns.
inline constexpr GUID kLegacySoundBufferInterface{
    0x59eff8b9, 0x938c, 0x4a26, {0x82, 0xf2, 0x95, 0xcb, 0x84, 0xcd, 0xc8, 0x37}};
struct LegacyHostSoundBuffer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetLength(DWORD length) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCapacity(DWORD* capacity) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetData(BYTE** data, DWORD* length) = 0;
};

// The original IStream extension appends these three methods after Clone.
inline constexpr GUID kLegacyInetStreamInterface{
    0x83e2bbbf, 0x4fec, 0x4e00, {0xa6, 0x2c, 0x62, 0x61, 0xfe, 0x2b, 0xa2, 0xd4}};
struct LegacyHostInetStream : IStream {
    virtual HRESULT STDMETHODCALLTYPE ReadEx(void* data, ULONG size, ULONG* read,
                                              HANDLE cancel, DWORD timeout_ms) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCancelEvent(HANDLE cancel) = 0;
    virtual HRESULT STDMETHODCALLTYPE SeekEx(LARGE_INTEGER distance, DWORD origin,
                                              ULARGE_INTEGER* position,
                                              HANDLE cancel, DWORD timeout_ms) = 0;
};

HRESULT CreateLegacySoundBuffer(void** output, DWORD capacity) noexcept;
HRESULT GetLegacySoundCodecName(const GUID* subtype, LPWSTR* output) noexcept;
HRESULT CreateLegacyInetStream(LPCWSTR url, DWORD mode, IStream** output,
                              HANDLE cancel, DWORD timeout_ms) noexcept;

// The exported callback has no settings argument. Match the original global
// network profile while retaining a separate cancellation lease for its caller.
void ConfigureLegacyHostNetwork(const audio::PlaybackOptions& options);

} // namespace ttplayer::plugins
