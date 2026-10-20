#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ttplayer::core {
std::string WideToUtf8(std::wstring_view value);
std::wstring Utf8ToWide(std::string_view value);
std::string ReadUtf8Text(const std::filesystem::path& path);
// Write all bytes to a unique sibling and flush/close before committing.
// Existing file metadata is preserved. Failed writes never truncate the
// target; a failed replacement restores its backup or retains it for recovery.
// This also accepts legacy-code-page text; no transcoding is performed.
void WriteFileAtomically(const std::filesystem::path& path, std::string_view bytes);
void WriteUtf8Text(const std::filesystem::path& path, std::string_view text);
}
