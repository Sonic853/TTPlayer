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
    PcmSourceEffects(const WAVEFORMATEX& input, HMODULE ttpcomm);
    ~PcmSourceEffects();
    PcmSourceEffects(const PcmSourceEffects&) = delete;
    PcmSourceEffects& operator=(const PcmSourceEffects&) = delete;
    bool Update(const PcmEffectSettings& settings);
    bool Active() const noexcept;
    bool Process(const std::vector<std::byte>& input, std::vector<double>& half_samples);
    bool Encode(const std::vector<double>& half_samples, std::vector<std::byte>& output);
    void Reset() noexcept;
    const std::wstring& Error() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
