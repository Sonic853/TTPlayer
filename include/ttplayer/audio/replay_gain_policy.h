#pragma once

#include <cmath>
#include <optional>

namespace ttplayer::audio {

// DAT_0054790C is passed to FUN_004B107E when the playback processor is
// constructed.  The live analyzer consumes the already-decoded PCM; it is
// unrelated to DAT_00547910/SkipScanGain, which is consulted only by the
// manual CScanGainDlg path at 004A4E8C.
[[nodiscard]] inline bool ShouldAnalyzeReplayGainOnPlayback(
    bool auto_scan_gain) noexcept {
    // 004B107E reads existing tags for the playback multiplier first, then
    // tests only param_8 before ordinal 100/101. Existing tags are therefore
    // replaced by the completed live analysis at 004B1A5C.
    return auto_scan_gain;
}

// The recovered processor is enabled only when both ReplayGain fields were
// read from the current sound. Peak is the presence guard in the original
// chain; the gain tag supplies the dB multiplier.
[[nodiscard]] inline double ResolveReplayGainScale(
    bool auto_gain, const std::optional<double>& gain_db,
    const std::optional<double>& peak) noexcept {
    if (!auto_gain || !gain_db || !peak) return 1.0;
    return std::pow(10.0, *gain_db * 0.05);
}

class ReplayGainRuntime {
public:
    ReplayGainRuntime(std::optional<double> gain_db,
                      std::optional<double> peak) noexcept
        : gain_db_(gain_db), peak_(peak) {}

    void Update(bool auto_gain) noexcept {
        scale_ = ResolveReplayGainScale(auto_gain, gain_db_, peak_);
    }

    [[nodiscard]] double Scale() const noexcept { return scale_; }

private:
    std::optional<double> gain_db_;
    std::optional<double> peak_;
    double scale_{1.0};
};

} // namespace ttplayer::audio
