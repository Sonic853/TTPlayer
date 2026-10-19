#include "ttplayer/audio/pcm_effects.h"
#include "ttplayer/audio/legacy_equalizer.h"
#include "ttplayer/audio/replay_gain_policy.h"
#include <ttpcomm/client.h>
#include <ttpcomm/runtime_pcm.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace ttplayer::audio {
int ResolveOutputBits(int configured, int source_bits) noexcept {
    const int bits = configured ? configured : source_bits;
    return bits == 8 || bits == 16 || bits == 24 || bits == 32 ? bits : 16;
}
double PcmReplayGain(bool enabled, std::optional<double> gain, std::optional<double> peak) noexcept {
    if (!enabled || !gain || !peak || !std::isfinite(*gain) || !std::isfinite(*peak)) return 1.0;
    const double scale = ResolveReplayGainScale(enabled,gain,peak);
    return std::isfinite(scale) ? scale : 1.0;
}
struct PcmSourceEffects::Impl {
    WAVEFORMATEXTENSIBLE format{};
    ttpcomm::client::ModuleReference module;
    void* eq{}; void* surround{};
    PcmEffectSettings settings;
    std::wstring error;
    Impl(const WAVEFORMATEX& input, HMODULE dll) : module(dll) {
        std::memcpy(&format, &input, std::min(sizeof(format), sizeof(input) + size_t(input.cbSize)));
    }
    ~Impl() { ttpcomm::client::Destroy(surround); ttpcomm::client::Destroy(eq); }
};
PcmSourceEffects::PcmSourceEffects(const WAVEFORMATEX& f, HMODULE dll) : impl_(std::make_unique<Impl>(f,dll)) {}
PcmSourceEffects::~PcmSourceEffects() = default;
__declspec(noinline) bool PcmSourceEffects::Update(const PcmEffectSettings& settings) {
    auto& p=*impl_; p.error.clear(); bool initialized=true;
    p.settings.gain=std::isfinite(settings.gain) ? settings.gain : 1.0;
    if (!settings.equalizer) { ttpcomm::client::Destroy(p.eq); p.eq=nullptr; }
    else {
        if (!p.eq) p.eq=ttpcomm::client::Create(p.module.get(),103);
        if (!p.eq || !InitializeLegacyEqualizer(p.eq,p.format.Format.nSamplesPerSec,p.format.Format.nChannels) ||
            !SetLegacyEqualizer(p.eq,settings.bands)) {
            ttpcomm::client::Destroy(p.eq);p.eq=nullptr;p.error=L"ttpcomm equalizer initialization failed";initialized=false;
        }
    }
    if (settings.surround != p.settings.surround) { ttpcomm::client::Destroy(p.surround);p.surround=nullptr; }
    p.settings.surround=settings.surround;
    if (!p.surround && settings.surround) {
        p.surround=ttpcomm::client::Create(p.module.get(),104);
        if (!p.surround || !ttpcomm::client::InitializeSurround(p.surround,p.format.Format.nSamplesPerSec,
            p.format.Format.nChannels,settings.surround)) {
            ttpcomm::client::Destroy(p.surround);p.surround=nullptr;if(!p.error.empty())p.error+=L"; ";p.error+=L"ttpcomm surround initialization failed";initialized=false;
        }
    }
    return initialized;
}
bool PcmSourceEffects::Active() const noexcept { return impl_->settings.gain!=1 || impl_->eq || impl_->surround; }
bool PcmSourceEffects::Process(const std::vector<std::byte>& input, std::vector<double>& samples) {
    auto& p=*impl_;
    if (!ttpcomm::host::pcm::Decode(input.data(),input.size(),p.format.Format,samples,ttpcomm::host::pcm::Domain::half)) {
        p.error=L"Unsupported source PCM format";return false;
    }
    if(samples.empty())return true;
    if(p.settings.gain!=1)ttpcomm::host::pcm::ApplyGainHalf(samples,p.settings.gain);
    if(samples.size()>size_t(INT_MAX)){p.error=L"Source PCM block is too large";return false;}
    int count=static_cast<int>(samples.size());
    if(p.eq && !ttpcomm::client::Equalize(p.eq,samples.data(),&count)){p.error=L"ttpcomm equalizer processing failed";return false;}
    if(count<0 || size_t(count)>samples.size() || count%p.format.Format.nChannels){p.error=L"Invalid equalizer output size";return false;}
    samples.resize(static_cast<size_t>(count));
    if(p.surround && !ttpcomm::client::Surround(p.surround,samples.data(),count)){p.error=L"ttpcomm surround processing failed";return false;}
    return true;
}
bool PcmSourceEffects::Encode(const std::vector<double>& samples,std::vector<std::byte>& bytes) {
    return ttpcomm::host::pcm::EncodeHalf(samples,impl_->format.Format,bytes);
}
void PcmSourceEffects::Reset() noexcept {
    ttpcomm::client::Reset(impl_->eq,6);ttpcomm::client::Reset(impl_->surround,3);
}
const std::wstring& PcmSourceEffects::Error() const noexcept {return impl_->error;}
}
