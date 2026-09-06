#include "ttplayer/audio/native_output_contract.h"

#include <algorithm>
#include <limits>
#include <string>
#include <windows.h>
#include <objbase.h>

namespace ttplayer::audio {

std::optional<OutputDeviceKey> ParseOutputDeviceKey(std::wstring_view text) {
    OutputDeviceKey result;
    if (text.empty()) {
        result.backend = OutputBackend::wave_out;
        return result;
    }
    if (text.find(L'\0') != std::wstring_view::npos ||
        FAILED(CLSIDFromString(std::wstring(text).c_str(), &result.identifier)))
        return std::nullopt;

    const auto& key = result.identifier;
    const bool zero_tail = key.Data2 == 0 && key.Data3 == 0 &&
        std::all_of(std::begin(key.Data4), std::end(key.Data4),
                    [](BYTE value) { return value == 0; });
    if (!zero_tail) {
        result.backend = OutputBackend::direct_sound;
        constexpr GUID default_key{0xdef00000, 0x9c6d, 0x47ed,
            {0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03}};
        result.default_direct_sound = IsEqualGUID(key, default_key) != FALSE;
        return result;
    }
    result.ordinal = LOWORD(key.Data1);
    switch (HIWORD(key.Data1)) {
    case 0: result.backend = OutputBackend::wave_out; break;
    case 2: result.backend = OutputBackend::kernel_streaming; break;
    case 3: result.backend = OutputBackend::asio; break;
    default: result.backend = OutputBackend::unknown; break;
    }
    return result;
}

std::optional<AsioChannelLayout> ResolveAsioChannelLayout(
    int source_bits, int driver_sample_type) noexcept {
    if (source_bits != 16 && source_bits != 24 && source_bits != 32)
        return std::nullopt;
    AsioChannelLayout result;
    switch (driver_sample_type) {
    case 0: result = {16, 16, false, true}; break;
    case 1: result = {24, 24, false, true}; break;
    case 2: result = {32, 32, false, true}; break;
    case 3: result = {32, 32, true, true}; break;
    case 4: result = {64, 64, true, true}; break;
    case 16: result = {16, 16, false, false}; break;
    case 17: result = {24, 24, false, false}; break;
    case 18: result = {32, 32, false, false}; break;
    case 19: result = {32, 32, true, false}; break;
    case 20: result = {64, 64, true, false}; break;
    case 24: result = {32, 16, false, false}; break;
    case 25: result = {32, 18, false, false}; break;
    case 26: result = {32, 20, false, false}; break;
    case 27: result = {32, 24, false, false}; break;
    default: return std::nullopt;
    }
    return result;
}

std::optional<AsioBufferPlan> PlanAsioBuffers(
    std::uint32_t requested_source_bytes, std::uint16_t source_block_align,
    std::int32_t preferred_driver_frames) noexcept {
    if (source_block_align == 0 || preferred_driver_frames <= 0)
        return std::nullopt;
    const auto frames = static_cast<std::uint32_t>(preferred_driver_frames);
    const std::uint64_t count = std::max<std::uint64_t>(4,
        (std::uint64_t{frames} / 2 +
         requested_source_bytes / source_block_align) / frames);
    const std::uint64_t bytes = count * frames * source_block_align;
    if (bytes > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return AsioBufferPlan{frames, static_cast<std::uint32_t>(count),
                          static_cast<std::uint32_t>(bytes)};
}

} // namespace ttplayer::audio
