#pragma once
#include <windows.h>
#include <mmreg.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace ttpcomm::pcm {
// Explicit domains prevent a factor-of-two regression between the playback
// chain (+/-0.5) and ReplayGain / external IEEE float PCM (+/-1).
enum class Domain { half, full };
struct Encoding {
    bool supported{};
    bool floating_point{};
    WORD valid_bits{};
    DWORD channel_mask{};
};
inline Encoding Describe(const WAVEFORMATEX& format, bool allowPadding = false) noexcept {
    Encoding result;
    if (!format.nChannels || !format.nSamplesPerSec || !format.wBitsPerSample) return result;
    const unsigned packed = ((format.wBitsPerSample + 7u) / 8u) * format.nChannels;
    if (packed > format.nBlockAlign || (!allowPadding && packed != format.nBlockAlign)) return result;
    WORD tag = format.wFormatTag;
    result.valid_bits = format.wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return result;
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        const BYTE tail[]{0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        if (extended.SubFormat.Data2 || extended.SubFormat.Data3 != 0x10 ||
            std::memcmp(tail, extended.SubFormat.Data4, sizeof(tail)) ||
            (extended.SubFormat.Data1 != WAVE_FORMAT_PCM && extended.SubFormat.Data1 != WAVE_FORMAT_IEEE_FLOAT)) return result;
        tag = static_cast<WORD>(extended.SubFormat.Data1);
        result.channel_mask = extended.dwChannelMask;
        result.valid_bits = extended.Samples.wValidBitsPerSample ? extended.Samples.wValidBitsPerSample : format.wBitsPerSample;
    }
    result.floating_point = tag == WAVE_FORMAT_IEEE_FLOAT;
    result.supported = result.floating_point
        ? (format.wBitsPerSample == 32 || format.wBitsPerSample == 64) && result.valid_bits == format.wBitsPerSample
        : tag == WAVE_FORMAT_PCM && (format.wBitsPerSample == 8 || format.wBitsPerSample == 16 ||
            format.wBitsPerSample == 24 || format.wBitsPerSample == 32) && result.valid_bits &&
            result.valid_bits <= format.wBitsPerSample && (format.wBitsPerSample != 8 || result.valid_bits == 8);
    return result;
}
inline double DecodeSample(const uint8_t* value, WORD bits, bool floating,
                            WORD validBits, Domain domain) noexcept {
    if (floating) {
        double number;
        if (bits == 32) { float f; std::memcpy(&f, value, 4); number = f; }
        else std::memcpy(&number, value, 8);
        return std::isfinite(number) ? number * (domain == Domain::half ? 0.5 : 1.0) : 0.0;
    }
    if (bits == 8) return (int(*value) - 128) / (domain == Domain::half ? 256.0 : 128.0);
    uint64_t raw{};
    for (unsigned i = 0; i < unsigned(bits) / 8u; ++i) raw |= uint64_t(value[i]) << (i * 8);
    const uint64_t sign = uint64_t{1} << (bits - 1);
    int64_t number = static_cast<int64_t>((raw ^ sign) - sign);
    if (validBits < bits) number >>= bits - validBits;
    return double(number) / std::ldexp(1.0, validBits - (domain == Domain::full ? 1 : 0));
}
inline bool Decode(const void* bytes, size_t size, const WAVEFORMATEX& format,
                    std::vector<double>& samples, Domain domain, bool allowPadding = false) {
    const auto encoding = Describe(format, allowPadding);
    if (!encoding.supported || (size && !bytes) || size % format.nBlockAlign) return false;
    const size_t frames = size / format.nBlockAlign;
    if (frames > samples.max_size() / format.nChannels) return false;
    samples.resize(frames * format.nChannels);
    const auto* input = static_cast<const uint8_t*>(bytes);
    const unsigned width = format.wBitsPerSample / 8;
    for (size_t i = 0; i < frames; ++i)
        for (unsigned c = 0; c < format.nChannels; ++c)
            samples[i * format.nChannels + c] = DecodeSample(input + i * format.nBlockAlign + c * width,
                format.wBitsPerSample, encoding.floating_point, encoding.valid_bits, domain);
    return true;
}
// Legacy effect-chain encoder. Dither/noise shaping is a separate final-output
// operation; do not substitute this rounding rule for the dither quantizer.
template<class Byte>
bool EncodeHalf(const std::vector<double>& samples, const WAVEFORMATEX& format,
                std::vector<Byte>& bytes) {
    static_assert(sizeof(Byte) == 1, "PCM output is a byte buffer");
    const auto encoding = Describe(format);
    if (!encoding.supported || encoding.valid_bits != format.wBitsPerSample ||
        samples.size() % format.nChannels) return false;
    const unsigned width = format.wBitsPerSample / 8;
    if (samples.size() > bytes.max_size() / width) return false;
    bytes.resize(samples.size() * width);
    auto* output = reinterpret_cast<uint8_t*>(bytes.data());
    for (size_t i = 0; i < samples.size(); ++i) {
        auto* value = output + i * width;
        const double sample = std::isfinite(samples[i]) ? samples[i] : 0.0;
        if (encoding.floating_point) {
            const double number = std::clamp(sample * 2.0, -1.0, 1.0);
            if (width == 4) { const float f = static_cast<float>(number); std::memcpy(value, &f, 4); }
            else std::memcpy(value, &number, 8);
        } else if (width == 1) {
            value[0] = static_cast<uint8_t>(std::clamp<long long>(std::llround(
                std::clamp(sample, -0.5, 127.0 / 256.0) * 256.0 + 128.0), 0, 255));
        } else {
            const double scale = std::ldexp(1.0, format.wBitsPerSample);
            const int64_t number = std::llround(std::clamp(sample, -0.5, 0.5 - 1.0 / scale) * scale);
            for (unsigned b = 0; b < width; ++b) value[b] = static_cast<uint8_t>(uint64_t(number) >> (b * 8));
        }
    }
    return true;
}
inline void ApplyGainHalf(std::vector<double>& samples, double gain) noexcept {
    if (gain == 1.0) return;
    for (auto& sample : samples) {
        double value = sample * gain;
        if (value > 0.5) value = (std::tan((value - 0.5) * 2.0) + 1.0) * 0.5;
        else if (value < -0.5) value = std::tan((value + 0.5) * 2.0) * 0.5 - 0.5;
        sample = value;
    }
}
}
