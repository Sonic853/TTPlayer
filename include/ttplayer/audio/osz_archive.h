#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace ttplayer::audio {

struct OsuMediaReferences {
    std::wstring audio;
    std::wstring background;
};

struct OszMedia {
    std::wstring beatmap;
    std::wstring audio;
    std::wstring background;
};

[[nodiscard]] bool IsOszArchive(const std::filesystem::path& path);
// UTF-8 .osu text: only General/AudioFilename and Events/background are media.
// Hit samples, storyboards and videos are deliberately not playback entries.
[[nodiscard]] std::optional<OsuMediaReferences> ParseOsuMedia(
    std::span<const unsigned char> bytes);
// Return each referenced audio member once, in first beatmap-reference order.
// Each audio keeps its first available background; paths stay inside the archive.
// required_audio limits cover lookup to one member and permits an early return
// once that member's background is found. Import scans every beatmap instead.
[[nodiscard]] std::vector<OszMedia> ResolveOszMedia(
    const std::filesystem::path& archive, std::span<const std::wstring> names,
    HMODULE module, const std::function<bool()>& canceled = {},
    const std::function<void(uint64_t, uint64_t)>& progress = {},
    std::wstring_view required_audio = {});

} // namespace ttplayer::audio
