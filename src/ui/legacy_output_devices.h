#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <guiddef.h>

namespace ttplayer::audio {
struct AsioDriverCapabilities;
}

namespace ttplayer::ui::detail {

// A presentation-only snapshot of the two legacy output backends that are
// discovered outside winmm/dsound.  The player keeps the original four-way
// backend numbering so bitmap 0x167 and the 16-byte persisted keys remain
// compatible with FUN_004991F1.
struct LegacyOutputDevice {
    int backend{}; // 0 waveOut, 1 DirectSound, 2 KS, 3 ASIO
    unsigned int wave_device_id{static_cast<unsigned int>(-1)};
    std::wstring key;
    std::wstring name;
    std::wstring module;
    GUID class_id{};
    bool has_class_id{};
    std::array<std::wstring, 4> details;
};

enum class NativeOutputResolutionError {
    none, invalid_key, wrong_backend, missing_device, ambiguous_device,
    invalid_descriptor
};

struct NativeOutputResolution {
    std::optional<LegacyOutputDevice> device;
    NativeOutputResolutionError error{NativeOutputResolutionError::none};
};

// Resolve against the same accepted catalog used by options. The persisted
// ordinal is part of entry.key, not its display-sorted row number. This
// operation does not load a driver and does not imply playback support.
[[nodiscard]] NativeOutputResolution ResolveLegacyNativeOutputDevice(
    std::wstring_view key, std::span<const LegacyOutputDevice> catalog);

[[nodiscard]] std::vector<LegacyOutputDevice>
EnumerateKernelStreamingOutputDevices();

[[nodiscard]] std::vector<LegacyOutputDevice>
EnumerateAsioOutputDevices();

// The first two FUN_004991F1 catalogue stages are safe fallbacks when the
// isolated KS/ASIO helper is unavailable: Wave Mapper + waveOut first, then
// DirectSound default + physical endpoints.
[[nodiscard]] std::vector<LegacyOutputDevice>
EnumerateWaveAndDirectSoundOutputDevices();

// Populate the four detail rows for one already-discovered descriptor.
// FUN_004991F1/FUN_004E1C87 only publish ASIO names/CLSIDs while building the
// combo box; FUN_0049989D reaches FUN_004E201C/FUN_004E207B only after that
// row is selected. Keep this separate so one bad in-process driver cannot
// suppress the complete output-device catalogue.
[[nodiscard]] bool PopulateLegacyOutputDeviceDetails(
    LegacyOutputDevice& device, std::wstring* diagnostic = nullptr);

// FUN_0049989D's four ASIO value strings. Kept pure so formatting remains
// testable on hosts without a registered ASIO driver.
[[nodiscard]] std::array<std::wstring, 4> FormatAsioOutputDeviceDetails(
    const audio::AsioDriverCapabilities& capabilities);

[[nodiscard]] std::vector<LegacyOutputDevice>
EnumerateLegacyOutputDevices();

[[nodiscard]] bool WriteLegacyOutputDeviceProbe(
    const std::filesystem::path& output,
    const std::vector<LegacyOutputDevice>& devices);

[[nodiscard]] std::optional<std::vector<LegacyOutputDevice>>
ReadLegacyOutputDeviceProbe(const std::filesystem::path& input);

} // namespace ttplayer::ui::detail
