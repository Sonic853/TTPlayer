#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <filesystem>

namespace ttplayer::plugins::detail {

// This is a final candidate, not a promise that the machine has a codec for
// the format. Initialize negotiates with the installed synchronous ACM driver.
bool SupportsBuiltinAcmDecoder(const WAVEFORMATEX& format) noexcept;
HRESULT CreateBuiltinAcmDecoder(void** decoder, std::filesystem::path& module_path) noexcept;
HRESULT FinishBuiltinAcmDecoder(void* decoder) noexcept;

} // namespace ttplayer::plugins::detail
