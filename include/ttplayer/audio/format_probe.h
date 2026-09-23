#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <windows.h>
#include <objidl.h>

namespace ttplayer::audio {

// 004C5757 / 004CBA11: a routing hint, never a replacement filename or proof
// that a decoder can read the entire file. Deliberately not a universal sniffer.
[[nodiscard]] std::wstring AudioExtensionHint(const std::filesystem::path& path);
[[nodiscard]] std::wstring RecoverAudioFormatHint(
    std::wstring_view extension, std::span<const unsigned char> header);
// Both overloads preserve the actual filename. The IStream overload preserves
// its caller's position, including on short reads and failed probes.
[[nodiscard]] HRESULT ProbeAudioFormatHint(
    const std::filesystem::path& path, std::wstring& extension);
[[nodiscard]] HRESULT ProbeAudioFormatHint(IStream* stream, std::wstring& extension);
[[nodiscard]] bool IsTerminalAudioOpenError(HRESULT result) noexcept;

} // namespace ttplayer::audio
