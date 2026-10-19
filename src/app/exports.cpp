#include "ttplayer/plugins/plugin_manager.h"
#include "ttplayer/plugins/legacy_host_exports.h"
#include "ttplayer/audio/builtin_file_info.h"

// TTPlayer.exe!004C51D3. ttp_clienc!6020619C resolves this by its undecorated
// name at DLL initialization; 6020147F rejects %s presets when it is absent.
// The 602014D4 call passes (wide path, STGM mode 0x1022, IStream**) stdcall.
// Expose the original callback rather than changing or patching the AddIn.
extern "C" HRESULT WINAPI TTPlayer_CreateStreamOnFile(
    LPCWSTR path, DWORD mode, IStream** output) noexcept {
    return ttplayer::plugins::CreateLegacyFileStream(path, mode, output);
}

extern "C" HRESULT WINAPI TTPlayer_CreateStdContent(
    IStream* stream, DWORD write_type, IUnknown** content,
    ULONGLONG* audio_bytes) noexcept {
    return ttplayer::audio::CreateStandardContent(stream, write_type, content, audio_bytes);
}

// Original 004C5171 / 004C5269 / 004C4FD1, including their stdcall arity.
extern "C" HRESULT WINAPI TTPlayer_CreateSoundBuffer(void** output, DWORD capacity) noexcept {
    return ttplayer::plugins::CreateLegacySoundBuffer(output, capacity);
}

extern "C" HRESULT WINAPI TTPlayer_CreateStreamOnInet(
    LPCWSTR url, DWORD mode, IStream** output, HANDLE cancel, DWORD timeout_ms) noexcept {
    return ttplayer::plugins::CreateLegacyInetStream(url, mode, output, cancel, timeout_ms);
}

extern "C" HRESULT WINAPI TTPlayer_GetSoundCodecName(const GUID* subtype, LPWSTR* output) noexcept {
    return ttplayer::plugins::GetLegacySoundCodecName(subtype, output);
}

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:CreateSoundBuffer=_TTPlayer_CreateSoundBuffer@8,@1")
#pragma comment(linker, "/EXPORT:CreateStdContent=_TTPlayer_CreateStdContent@16,@2")
#pragma comment(linker, "/EXPORT:CreateStreamOnFile=_TTPlayer_CreateStreamOnFile@12,@3")
#pragma comment(linker, "/EXPORT:CreateStreamOnInet=_TTPlayer_CreateStreamOnInet@20,@4")
#pragma comment(linker, "/EXPORT:GetSoundCodecName=_TTPlayer_GetSoundCodecName@8,@5")
#else
#pragma comment(linker, "/EXPORT:CreateSoundBuffer=TTPlayer_CreateSoundBuffer,@1")
#pragma comment(linker, "/EXPORT:CreateStdContent=TTPlayer_CreateStdContent,@2")
#pragma comment(linker, "/EXPORT:CreateStreamOnFile=TTPlayer_CreateStreamOnFile,@3")
#pragma comment(linker, "/EXPORT:CreateStreamOnInet=TTPlayer_CreateStreamOnInet,@4")
#pragma comment(linker, "/EXPORT:GetSoundCodecName=TTPlayer_GetSoundCodecName,@5")
#endif
