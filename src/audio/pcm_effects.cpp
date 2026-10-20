#include "ttplayer/audio/pcm_effects.h"
#include "ttplayer/audio/legacy_equalizer.h"
#include "ttplayer/audio/replay_gain_policy.h"
#include <ttpcomm/client.h>
#include <ttpcomm/runtime_pcm.h>
#include <ttpcomm/audio_client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
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
    bool modern{};
    TtpCommAudioApi audio{};
    void* effects{};
    void* encoder{};
    TtpCommAudioEffectsConfig configured{};
    Impl(const WAVEFORMATEX& input, HMODULE dll, bool high_precision) : module(dll), modern(high_precision) {
        std::memcpy(&format, &input, std::min(sizeof(format), sizeof(input) + size_t(input.cbSize)));
    }
    ~Impl() {
        if (encoder) audio.encoder_destroy(encoder);
        if (effects) audio.effects_destroy(effects);
        ttpcomm::client::Destroy(surround); ttpcomm::client::Destroy(eq);
    }
};
PcmSourceEffects::PcmSourceEffects(const WAVEFORMATEX& f, HMODULE dll, bool modern)
    : impl_(std::make_unique<Impl>(f,dll,modern)) {}
PcmSourceEffects::~PcmSourceEffects() = default;
__declspec(noinline) bool PcmSourceEffects::Update(const PcmEffectSettings& settings) {
    auto& p=*impl_; p.error.clear(); bool initialized=true;
    if (p.modern) {
        if (!p.audio.size && !ttpcomm::host::QueryAudio(p.module.get(), p.audio)) {
            p.error=L"High precision audio requires the rebuilt ttpcomm.dll audio ABI 2";
            return false;
        }
        TtpCommAudioEffectsConfig config{};
        config.size=sizeof(config);
        config.sample_rate=p.format.Format.nSamplesPerSec;
        config.channels=p.format.Format.nChannels;
        config.flags=(settings.equalizer ? TTPCOMM_AUDIO_EQ : 0) |
                     (settings.surround ? TTPCOMM_AUDIO_SURROUND : 0);
        for (size_t i=0;i<10;++i) config.bands_db[i]=std::clamp(settings.bands[i+1],-60,24);
        config.preamp_db=std::clamp(settings.bands[0],-60,24);
        config.surround_strength=std::clamp(settings.surround,0,16);
        config.transition_frames=std::max(1u,config.sample_rate/50);
        if (!p.effects) p.effects=p.audio.effects_create(&config);
        else if (std::memcmp(&config,&p.configured,sizeof(config)) &&
                 p.audio.effects_configure(p.effects,&config)!=TTPCOMM_OK) {
            p.error=L"High precision effect configuration failed";
            return false;
        }
        if (!p.effects) {p.error=L"High precision effect initialization failed";return false;}
        p.configured=config;
        p.settings=settings;
        p.settings.gain=std::isfinite(settings.gain) ? settings.gain : 1.0;
        return true;
    }
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
bool PcmSourceEffects::Active() const noexcept { return impl_->modern || impl_->settings.gain!=1 || impl_->eq || impl_->surround; }
bool PcmSourceEffects::Process(const std::vector<std::byte>& input, std::vector<double>& samples, bool end) {
    auto& p=*impl_;
    if (!ttpcomm::host::pcm::Decode(input.data(),input.size(),p.format.Format,samples,ttpcomm::host::pcm::Domain::half)) {
        p.error=L"Unsupported source PCM format";return false;
    }
    if (p.modern) {
        if (!p.effects) {p.error=L"High precision effects are unavailable";return false;}
        if (p.settings.gain!=1) for (auto& sample:samples) sample*=p.settings.gain;
        const size_t channels=p.format.Format.nChannels;
        if (!channels || samples.size()%channels || samples.size()/channels>UINT32_MAX) {
            p.error=L"High precision source PCM block is too large or not frame aligned";
            return false;
        }
        TtpCommAudioEffectsInfo info{sizeof(info)};
        if(p.audio.effects_info(p.effects,&info)!=TTPCOMM_OK) {
            p.error=L"High precision effect buffering information is unavailable";
            return false;
        }
        const uint64_t capacity=uint64_t(samples.size())+
            (uint64_t(info.block_frames)+info.group_delay_frames+1)*channels;
        std::vector<double> result, block;
        if(capacity>result.max_size()) {
            p.error=L"High precision effect output buffer is too large";
            return false;
        }
        try {
            // Reserve all output space before accepting input into the DLL.
            // At most one block and the compensated FIR delay can be pending
            // from an earlier call, including an explicit empty-input drain.
            result.reserve(static_cast<size_t>(capacity));
            block.resize(4096*channels);
        }catch(const std::bad_alloc&) {
            p.error=L"Unable to allocate the high precision effect output buffer";
            return false;
        }
        size_t used{};
        // A call may consume input without producing output while priming
        // the FIR. At EOF the API removes its own latency and returns every
        // accepted frame, including tracks shorter than a processing block.
        do {
            const auto remaining=static_cast<uint32_t>((samples.size()-used)/channels);
            TtpCommAudioProcessResult processed{sizeof(processed)};
            const int status=p.audio.effects_process(p.effects,
                remaining ? samples.data()+used : nullptr,remaining,block.data(),4096,
                end ? TTPCOMM_AUDIO_END_INPUT : 0,&processed);
            if (status!=TTPCOMM_OK && status!=TTPCOMM_END) {
                p.error=L"High precision effects rejected the PCM block";return false;
            }
            if (processed.consumed_frames>remaining || processed.produced_frames>4096 ||
                result.size()+size_t(processed.produced_frames)*channels>result.capacity() ||
                (status==TTPCOMM_END && processed.consumed_frames!=remaining)) {
                p.error=L"High precision effects returned invalid frame counts";
                return false;
            }
            used+=size_t(processed.consumed_frames)*channels;
            result.insert(result.end(),block.begin(),block.begin()+size_t(processed.produced_frames)*channels);
            // Once all input is consumed, also collect every ready output
            // block. An empty non-EOF return only means that the FIR needs
            // more source samples; it is not a failure or the end of a song.
            if (status==TTPCOMM_END ||
                (!end && used==samples.size() && !processed.produced_frames)) break;
            if (!processed.consumed_frames && !processed.produced_frames) {
                p.error=L"High precision effects made no progress";return false;
            }
        } while (true);
        samples=std::move(result);
        return true;
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
    auto& p=*impl_;
    if(p.modern) {
        if(!p.audio.size || !p.effects) {
            p.error=L"High precision effects are unavailable";return false;
        }
        const auto format=ttpcomm::host::pcm::Format(p.format.Format);
        if(!format.channels || samples.size()%format.channels ||
            samples.size()/format.channels>UINT32_MAX) {
            p.error=L"High precision PCM encoding requires complete frames";return false;
        }
        const uint64_t size=uint64_t(samples.size()/format.channels)*format.block_align;
        if(size>UINT32_MAX || size>bytes.max_size()) {
            p.error=L"High precision encoded PCM block is too large";return false;
        }
        if(!p.encoder) {
            TtpCommAudioEncoderConfig config{sizeof(config),format,0,0};
            p.encoder=p.audio.encoder_create(&config);
        }
        if(!p.encoder) {p.error=L"High precision PCM encoder is unavailable";return false;}
        bytes.resize(static_cast<size_t>(size));
        TtpCommAudioEncodeResult result{sizeof(result)};
        if(p.audio.encoder_encode(p.encoder,samples.data(),
            static_cast<uint32_t>(samples.size()/format.channels),nullptr,0,
            bytes.data(),static_cast<uint32_t>(size),&result)!=TTPCOMM_OK ||
            result.bytes!=size) {
            p.error=L"High precision PCM encoder rejected the samples";return false;
        }
        return true;
    }
    return ttpcomm::host::pcm::EncodeHalf(samples,impl_->format.Format,bytes);
}
void PcmSourceEffects::Reset() noexcept {
    if (impl_->effects) impl_->audio.effects_reset(impl_->effects);
    if (impl_->encoder) impl_->audio.encoder_reset(impl_->encoder);
    ttpcomm::client::Reset(impl_->eq,6);ttpcomm::client::Reset(impl_->surround,3);
}
const std::wstring& PcmSourceEffects::Error() const noexcept {return impl_->error;}
}
