#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include <windows.h>
#include <mmreg.h>

namespace ttplayer::audio {

struct Mp3TagPolicy {
    std::uint32_t read_priority{0x04080000U};
    std::uint32_t write_type{5U};
    std::uint32_t id3v2_encoding{};
    bool id3v2_padding{true};
};

struct BuiltinMetadataEntry {
    std::wstring name;
    std::wstring value;
};

struct BuiltinFileInfo {
    DWORD capabilities{};
    WAVEFORMATEX format{};
    DWORD duration_ms{};
    DWORD encoded_bits_per_second{};
    std::wstring codec;
    std::vector<BuiltinMetadataEntry> metadata;
    std::vector<unsigned char> cover;
};

struct BuiltinTagWriteField {
    std::string name;
    std::wstring value;
};

struct BuiltinTagWriteResult {
    HRESULT status{E_FAIL};
    std::vector<HRESULT> fields;
    HRESULT cover_status{S_FALSE};
};

enum class BuiltinCoverAction : std::uint32_t {
    unchanged = 0,
    replace = 1,
    remove = 2,
};

// The 5.7.9 executable contains an MP3 reader/tag writer in the main image
// (004D9AC8/004D9B56), rather than in AddIn\ttp_mp3.dll.  These functions
// provide that built-in boundary to the isolated file-info helper.  WAV and
// Shell metadata fallbacks are read-only and report no tag-write capability.
[[nodiscard]] HRESULT ReadBuiltinFileInfo(
    const std::filesystem::path& path, const Mp3TagPolicy& policy,
    BuiltinFileInfo& result) noexcept;

[[nodiscard]] BuiltinTagWriteResult WriteBuiltinFileInfo(
    const std::filesystem::path& path, const Mp3TagPolicy& policy,
    std::span<const BuiltinTagWriteField> fields,
    BuiltinCoverAction cover_action = BuiltinCoverAction::unchanged,
    std::span<const unsigned char> cover = {}) noexcept;

} // namespace ttplayer::audio
