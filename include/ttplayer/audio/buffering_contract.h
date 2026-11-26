#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <windows.h>
#include <mmsystem.h>

namespace ttplayer::audio {

// 004EA0EA: the built-in PCM reader advertises an aligned 1/24 second.
[[nodiscard]] constexpr std::uint32_t PcmReaderPacketBytes(const WAVEFORMATEX& format) noexcept {
    const auto align = format.nBlockAlign;
    return align ? std::max<DWORD>(1, format.nAvgBytesPerSec / 24 / align) * align : 0;
}

struct DirectSoundBufferPlan {
    std::uint32_t reader_bytes;   // decoded source format, before effects/SSRC
    std::uint32_t packet_bytes;   // processing packet in the output format
    std::uint32_t logical_bytes;
    std::uint32_t reserve_bytes;
    std::uint32_t prefill_bytes;
    std::uint32_t low_water_bytes;
    std::uint32_t refill_bytes;

    [[nodiscard]] constexpr std::uint32_t RingBytes() const noexcept {
        return logical_bytes + reserve_bytes;
    }
    [[nodiscard]] constexpr bool NeedsPrefill(std::uint64_t ready, bool eof) const noexcept {
        return !eof && ready < prefill_bytes;
    }
    [[nodiscard]] constexpr bool BelowLowWater(std::uint64_t ready, bool eof) const noexcept {
        return !eof && ready < low_water_bytes;
    }
    [[nodiscard]] constexpr bool Refilled(std::uint64_t ready, bool eof) const noexcept {
        return eof || ready > refill_bytes; // 004ACAED: strictly above the floored 80%
    }
};

[[nodiscard]] constexpr std::optional<DirectSoundBufferPlan> PlanDirectSoundBuffer(
    const WAVEFORMATEX& source, std::uint32_t reader_bytes, const WAVEFORMATEX& output,
    int duration_ms, bool url) noexcept {
    if (!source.nBlockAlign || !source.nSamplesPerSec || !reader_bytes ||
        !output.nBlockAlign || !output.nSamplesPerSec || !output.nAvgBytesPerSec)
        return {};
    const auto ceil_div = [](std::uint64_t bytes, std::uint64_t unit) {
        return (bytes + unit - 1) / unit;
    };
    const auto frames = ceil_div(reader_bytes, source.nBlockAlign);
    const auto native = frames * source.nBlockAlign;
    if (native > 64U * 1024U * 1024U) return {}; // legacy reader's maximum request
    auto output_frames = frames;
    if (output.nSamplesPerSec != source.nSamplesPerSec) {
        // 004B101E..004B1046: truncate((frames + 0.5 + 1) * outRate / inRate).
        output_frames = std::max<std::uint64_t>(1, (2 * frames + 3) * output.nSamplesPerSec /
                                                     (2ULL * source.nSamplesPerSec));
    }
    if (output_frames > UINT32_MAX / output.nBlockAlign) return {};
    const auto packet = output_frames * output.nBlockAlign;
    const std::uint64_t rate = output.nAvgBytesPerSec;
    const auto ms = std::clamp(duration_ms, url ? 3000 : 500, 10000);
    // 004AB3D9..004AB432: MulDiv rounds to nearest, then round up in groups of four packets.
    const auto target = (rate * ms + 500) / 1000;
    const auto group = 4 * packet;
    const auto logical = std::max<std::uint64_t>(1, ceil_div(target, group)) * group;
    // 004C3170 / 004C3856: this packet-aligned guard is physically present but never writable capacity.
    const auto reserve = ceil_div((rate * 250 + 500) / 1000, packet) * packet;
    if (logical + reserve > UINT32_MAX) return {};
    return DirectSoundBufferPlan{
        static_cast<std::uint32_t>(native), static_cast<std::uint32_t>(packet),
        static_cast<std::uint32_t>(logical), static_cast<std::uint32_t>(reserve),
        static_cast<std::uint32_t>(std::min(logical, rate) / 2),
        static_cast<std::uint32_t>(std::max(logical / 100, rate / 10)),
        static_cast<std::uint32_t>(logical * 8 / 10)};
}

// 004AC605 refills to 80%, but the original queue contains many small reader
// packets. A bounded queue of large packets must leave room for the partly
// played packet that cannot be recycled while the device is stopped. Units
// may be frames or bytes, provided capacity, packet and requested agree.
[[nodiscard]] constexpr std::uint64_t ReachableRefillWatermark(
    std::uint64_t capacity, std::uint64_t packet,
    std::uint64_t requested) noexcept {
    if (!capacity) return 0;
    const auto reachable = capacity > packet ? capacity - packet : capacity;
    return std::max<std::uint64_t>(1, std::min(requested, reachable));
}

} // namespace ttplayer::audio
