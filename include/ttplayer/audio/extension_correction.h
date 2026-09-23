#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ttplayer::audio {

struct MediaFileStamp {
    std::uint64_t size{}, modified{}, file_id{};
    DWORD volume{};
    bool operator==(const MediaFileStamp&) const = default;
};

struct ExtensionCorrection {
    std::filesystem::path source, target;
    std::wstring format;
    MediaFileStamp stamp;
};

// Call only after a decoder/metadata reader has successfully opened this file.
// Unlike routing hints, this verifies the container/frame header and accepts
// legitimate aliases. Unknown containers produce no suggestion.
std::optional<ExtensionCorrection> FindExtensionCorrection(
    const std::filesystem::path& path, int subtrack = 0);
std::optional<MediaFileStamp> ReadMediaFileStamp(const std::filesystem::path& path);

enum class ExtensionCollision { fail, number, overwrite };
struct ExtensionRenameResult {
    std::filesystem::path target;
    DWORD error{};
};
// The caller must obtain confirmation first. An overwrite also requires the
// destination stamp displayed in that confirmation; stale choices fail closed.
ExtensionRenameResult CorrectFileExtension(const ExtensionCorrection& correction,
    ExtensionCollision collision, const std::optional<MediaFileStamp>& destination = {});
std::wstring MediaFileSizeText(std::uint64_t bytes);

} // namespace ttplayer::audio
