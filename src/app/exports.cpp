#include "ttplayer/plugins/plugin_manager.h"

// TTPlayer.exe!004C51D3. ttp_clienc!6020619C resolves this by its undecorated
// name at DLL initialization; 6020147F rejects %s presets when it is absent.
// The 602014D4 call passes (wide path, STGM mode 0x1022, IStream**) stdcall.
// Expose the original callback rather than changing or patching the AddIn.
extern "C" HRESULT WINAPI TTPlayer_CreateStreamOnFile(
    LPCWSTR path, DWORD mode, IStream** output) noexcept {
    return ttplayer::plugins::CreateLegacyFileStream(path, mode, output);
}

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:CreateStreamOnFile=_TTPlayer_CreateStreamOnFile@12,@3")
#else
#pragma comment(linker, "/EXPORT:CreateStreamOnFile=TTPlayer_CreateStreamOnFile,@3")
#endif
