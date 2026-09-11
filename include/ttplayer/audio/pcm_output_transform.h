#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>

namespace ttplayer::audio {

// Output-side PCM contract recovered from FUN_004B0D1A.  Readers keep their
// native PCM format; this stage is shared by every output backend and applies
// the persisted Device/@ResampleRate, @SsrcMode, @OutputBits and @Dither
// settings after ReplayGain/EQ/surround processing.
struct PcmOutputTransformOptions {
    int output_bits{16};
    int resample_rate{};
    int ssrc_mode{1};
    int dither{};
    // CConvertDlg sends normalized IEEE float64 to AddIn encoders; only its
    // built-in Wave writer quantizes to the selected integer bit depth.
    bool floating_point{};
};

class PcmOutputTransform {
public:
    PcmOutputTransform();
    ~PcmOutputTransform();
    PcmOutputTransform(const PcmOutputTransform&) = delete;
    PcmOutputTransform& operator=(const PcmOutputTransform&) = delete;

    bool Open(const WAVEFORMATEX& input,
              const PcmOutputTransformOptions& options,
              HMODULE ttpcomm_module);
    bool Process(const std::vector<std::byte>& input,
                 std::vector<std::byte>& output,
                 bool end_of_stream = false);
    void Reset() noexcept;

    [[nodiscard]] WAVEFORMATEX OutputFormat() const noexcept;
    [[nodiscard]] bool UsesOrdinal102() const noexcept;
    [[nodiscard]] const std::wstring& Error() const noexcept;

private:
    struct Impl;
    Impl* impl_{};
};

} // namespace ttplayer::audio
