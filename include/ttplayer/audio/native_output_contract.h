#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <guiddef.h>

namespace ttplayer::audio {

enum class OutputBackend { wave_out = 0, direct_sound = 1, kernel_streaming = 2,
                           asio = 3, unknown = -1 };

struct OutputDeviceKey {
    OutputBackend backend{OutputBackend::unknown};
    GUID identifier{};
    std::uint16_t ordinal{};
    bool default_direct_sound{};
};

// FUN_004918B1 / FUN_004918FE: Data1.high selects the backend only when
// the remaining twelve bytes are zero. Data1.low is a catalog ordinal.
// An empty setting retains the existing wave mapper default.
[[nodiscard]] std::optional<OutputDeviceKey>
ParseOutputDeviceKey(std::wstring_view text);

struct AsioChannelLayout {
    int container_bits{};
    int valid_bits{};
    bool floating_point{};
    bool big_endian{};
};

// FUN_004E14BD's exact supported sample-type table. This validates a
// driver's channel layout; it does not create or start an ASIO driver.
[[nodiscard]] std::optional<AsioChannelLayout>
ResolveAsioChannelLayout(int source_bits, int driver_sample_type) noexcept;

struct AsioBufferPlan {
    std::uint32_t frames_per_buffer{};
    std::uint32_t queued_buffers{};
    std::uint32_t queued_source_bytes{};
};

// FUN_004E1610 rounds the requested byte capacity to the preferred driver
// buffer size, with at least four periods. Invalid/overflowing driver
// results must be rejected before allocation or createBuffers.
[[nodiscard]] std::optional<AsioBufferPlan>
PlanAsioBuffers(std::uint32_t requested_source_bytes,
                std::uint16_t source_block_align,
                std::int32_t preferred_driver_frames) noexcept;

} // namespace ttplayer::audio
