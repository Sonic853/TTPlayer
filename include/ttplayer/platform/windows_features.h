#pragma once

#include <cstddef>

namespace ttplayer::platform {
struct WindowsVersion {
    unsigned long major{}, minor{}, build{};
    [[nodiscard]] constexpr bool AtLeast(unsigned long required_major,
                                         unsigned long required_minor = 0) const noexcept {
        return major > required_major || (major == required_major && minor >= required_minor);
    }
};

// These are product policies, not substitutes for checking COM/API availability.
// A failed version query conservatively selects the old-system memory policy
// and avoids entering WinRT. RtlGetVersion is unaffected by the EXE manifest.
struct WindowsFeatures {
    WindowsVersion version;
    [[nodiscard]] constexpr bool SystemMediaControls() const noexcept {
        return version.AtLeast(10);
    }
    [[nodiscard]] constexpr bool ReducedMemory() const noexcept {
        return !version.AtLeast(6, 2);
    }
    [[nodiscard]] constexpr size_t RandomRoundLimit() const noexcept {
        return ReducedMemory() ? 0 : 5000;
    }
    [[nodiscard]] constexpr size_t MetadataCacheEntries() const noexcept {
        return ReducedMemory() ? 128 : 512;
    }
    [[nodiscard]] constexpr size_t MetadataCacheBytes() const noexcept {
        return (ReducedMemory() ? 4U : 16U) * 1024U * 1024U;
    }
};

[[nodiscard]] const WindowsFeatures& CurrentWindowsFeatures() noexcept;
} // namespace ttplayer::platform
