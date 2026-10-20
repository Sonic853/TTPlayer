#pragma once
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>
#include <mmreg.h>
namespace ttplayer::audio {
struct PcmEffectSettings {
    double gain{1.0};
    bool equalizer{};
    std::array<int, 11> bands{};
    int surround{};
};
// Reader slot 10 describes the source, independently of a decoder's double PCM.
int ResolveOutputBits(int configured, int source_bits) noexcept;
double PcmReplayGain(bool enabled, std::optional<double> gain, std::optional<double> peak) noexcept;
// 004B1950 -> 004B1B81, at the SOURCE rate. Both live and offline hosts use
// this state; SSRC and the Winamp bridge are deliberately output stages.
class PcmSourceEffects {
public:
    PcmSourceEffects(const WAVEFORMATEX& input, HMODULE ttpcomm, bool high_precision = false);
    ~PcmSourceEffects();
    PcmSourceEffects(const PcmSourceEffects&) = delete;
    PcmSourceEffects& operator=(const PcmSourceEffects&) = delete;
    bool Update(const PcmEffectSettings& settings);
    bool Active() const noexcept;
    // Modern mode drains every ready output block and may buffer a partial
    // FIR block. Call Process({}, output, true) at EOF to return the remaining
    // accepted frames; total output equals total input. Reset before a seek
    // or another track. Legacy mode retains its historical queue behavior.
    bool Process(const std::vector<std::byte>& input, std::vector<double>& half_samples,
                 bool end_of_stream = false);
    bool Encode(const std::vector<double>& half_samples, std::vector<std::byte>& output);
    void Reset() noexcept;
    const std::wstring& Error() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
