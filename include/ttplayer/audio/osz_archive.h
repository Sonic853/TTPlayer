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

inline constexpr HRESULT kOszPluginMissing = static_cast<HRESULT>(0x80047101U);
inline constexpr HRESULT kOszPluginIncompatible = static_cast<HRESULT>(0x80047102U);

struct OszMedia {
    std::wstring beatmap;
    std::wstring audio;
    std::wstring background;
};

[[nodiscard]] bool IsOszArchive(const std::filesystem::path& path);
// Capabilities are probed once per process. Install/remove add-ins while the
// player is closed; a legacy OSZ-only DLL need not expose the visual ABI.
[[nodiscard]] bool HasOszSupport() noexcept;
[[nodiscard]] bool HasOsuVisualSupport() noexcept;
[[nodiscard]] bool HasOsuLogoSupport() noexcept;
// Return each referenced audio member once, in first beatmap-reference order.
// Each audio keeps its first available background; paths stay inside the archive.
// required_audio limits cover lookup to one member and permits an early return
// once that member's background is found. Import scans every beatmap instead.
[[nodiscard]] std::vector<OszMedia> ResolveOszMedia(
    const std::filesystem::path& archive,
    HMODULE module, const std::function<bool()>& canceled = {},
    const std::function<void(uint64_t, uint64_t)>& progress = {},
    std::wstring_view required_audio = {});

[[nodiscard]] std::vector<unsigned char> ReadOszCover(
    const std::filesystem::path& archive,
    HMODULE module, const std::function<bool()>& canceled,
    std::wstring_view required_audio);

} // namespace ttplayer::audio
