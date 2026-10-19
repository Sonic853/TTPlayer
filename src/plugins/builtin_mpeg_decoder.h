#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <filesystem>

namespace ttplayer::plugins::detail {
bool SupportsBuiltinMpegDecoder(const WAVEFORMATEX& format) noexcept;
HRESULT CreateBuiltinMpegDecoder(void** decoder, std::filesystem::path& module_path) noexcept;
// S_FALSE means this is a foreign decoder with the original ten-slot ABI.
HRESULT FinishBuiltinMpegDecoder(void* decoder) noexcept;
}
