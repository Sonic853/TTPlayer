#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ttpcomm/audio_client.h>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ttplayer::audio {
enum class PcmPlaybackMode { compatible = 0, high_precision = 1, bit_exact = 2 };

// Source/endpoint contracts preserve valid bits and channel order. No helper
// silently resamples, drops channels or duplicates mono.
bool DescribeHiresPcm(const WAVEFORMATEX&, TtpCommPcmFormat&) noexcept;
WAVEFORMATEXTENSIBLE MakeHiresWave(const TtpCommPcmFormat&, bool extended = true) noexcept;
bool CanPreserveHiresPcm(const TtpCommPcmFormat& source,
                        const TtpCommPcmFormat& target) noexcept;
std::vector<WAVEFORMATEXTENSIBLE> HiresOutputCandidates(
    const WAVEFORMATEX& source, PcmPlaybackMode mode);
double ReadHiresSample(const std::byte*, const TtpCommPcmFormat&) noexcept;
bool FiniteHiresPacket(std::span<const std::byte>, const TtpCommPcmFormat&) noexcept;

// The API2 encoder performs the only integer quantization, after gains. The
// module is borrowed and must outlive this object.
class HiresPcmEncoder {
public:
    ~HiresPcmEncoder();
    HiresPcmEncoder() = default;
    HiresPcmEncoder(const HiresPcmEncoder&) = delete;
    HiresPcmEncoder& operator=(const HiresPcmEncoder&) = delete;
    bool Open(HMODULE, const TtpCommPcmFormat&, const TtpCommPcmFormat&,
              PcmPlaybackMode, size_t maximum_frames, std::wstring&, int dither = 0,
              uint32_t dither_seed = 0);
    bool Convert(std::span<const std::byte>, float left, float right,
                 std::span<std::byte> destination) noexcept;
    bool EncodeHalf(std::span<const double>, std::span<std::byte>) noexcept;
    void Reset() noexcept;
private:
    TtpCommAudioApi api_{};
    void* encoder_{};
    TtpCommPcmFormat source_{}, target_{};
    PcmPlaybackMode mode_{PcmPlaybackMode::compatible};
    std::vector<double> scratch_, gains_;
};
} // namespace ttplayer::audio
