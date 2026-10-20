#include "ttplayer/audio/hires_output.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace ttplayer::audio {
namespace {
constexpr GUID kPcm{1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
constexpr GUID kFloat{3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
DWORD DefaultMask(unsigned channels) noexcept {
    constexpr DWORD masks[]{0,4,3,7,0x33,0x37,0x3f,0x13f,0x63f};
    return channels < std::size(masks) ? masks[channels] : 0;
}
}
bool DescribeHiresPcm(const WAVEFORMATEX& wave, TtpCommPcmFormat& pcm) noexcept {
    pcm = {};
    bool floating = wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    unsigned valid = wave.wBitsPerSample;
    DWORD mask = DefaultMask(wave.nChannels);
    if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wave.cbSize < sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX)) return false;
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wave);
        if (IsEqualGUID(extended.SubFormat,kFloat)) floating = true;
        else if (!IsEqualGUID(extended.SubFormat,kPcm)) return false;
        valid = extended.Samples.wValidBitsPerSample;
        if (!valid) valid = wave.wBitsPerSample;
        mask = extended.dwChannelMask;
    } else if (!floating && wave.wFormatTag != WAVE_FORMAT_PCM) return false;
    const unsigned bits = wave.wBitsPerSample;
    if (!wave.nChannels || wave.nChannels > 64 || wave.nSamplesPerSec < 1000 ||
        wave.nSamplesPerSec > 768000 || !valid || valid > bits ||
        (floating ? ((bits != 32 && bits != 64) || valid != bits)
                  : (bits != 8 && bits != 16 && bits != 24 && bits != 32)) ||
        wave.nBlockAlign != wave.nChannels*(bits/8) ||
        wave.nAvgBytesPerSec != wave.nSamplesPerSec*wave.nBlockAlign ||
        (mask && std::popcount(mask) != wave.nChannels)) return false;
    pcm = {sizeof(pcm),wave.nChannels,wave.nSamplesPerSec,bits,valid,
           wave.nBlockAlign,floating ? 1u : 0u,mask};
    return true;
}
WAVEFORMATEXTENSIBLE MakeHiresWave(const TtpCommPcmFormat& p, bool extended) noexcept {
    WAVEFORMATEXTENSIBLE w{};
    if (p.valid_bits != p.bits || p.channels > 2) extended = true;
    w.Format = {WORD(extended ? WAVE_FORMAT_EXTENSIBLE :
        p.floating_point ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM),
        WORD(p.channels),p.sample_rate,p.sample_rate*p.block_align,
        WORD(p.block_align),WORD(p.bits),WORD(extended ? 22 : 0)};
    w.Samples.wValidBitsPerSample = WORD(p.valid_bits);
    w.dwChannelMask = p.channel_mask;
    w.SubFormat = p.floating_point ? kFloat : kPcm;
    return w;
}
bool CanPreserveHiresPcm(const TtpCommPcmFormat& s, const TtpCommPcmFormat& d) noexcept {
    if (s.channels != d.channels || s.sample_rate != d.sample_rate ||
        s.channel_mask != d.channel_mask) return false;
    if (s.floating_point) return d.floating_point && d.bits >= s.bits;
    return d.floating_point ? (d.bits == 64 || s.valid_bits <= 24)
                            : d.valid_bits >= s.valid_bits;
}
std::vector<WAVEFORMATEXTENSIBLE> HiresOutputCandidates(
    const WAVEFORMATEX& source, PcmPlaybackMode mode) {
    TtpCommPcmFormat s{};
    std::vector<WAVEFORMATEXTENSIBLE> result;
    if (!DescribeHiresPcm(source,s)) return result;
    const auto add = [&](TtpCommPcmFormat p) {
        p.block_align = p.channels*(p.bits/8);
        if (mode==PcmPlaybackMode::high_precision && !p.floating_point && p.valid_bits<24) return;
        if (mode == PcmPlaybackMode::bit_exact && !CanPreserveHiresPcm(s,p)) return;
        for (bool ext : {true,false}) {
            if (!ext && (p.channels > 2 || p.valid_bits != p.bits ||
                p.channel_mask != DefaultMask(p.channels))) continue;
            const auto w = MakeHiresWave(p,ext);
            if (std::none_of(result.begin(),result.end(),[&](const auto& x) {
                return std::memcmp(&w,&x,sizeof(w)) == 0; })) result.push_back(w);
        }
    };
    if (mode == PcmPlaybackMode::bit_exact) add(s);
    for (const auto& spec : {std::array<unsigned,3>{64,64,1}, {32,32,0},
                           {32,24,0}, {24,24,0}, {32,32,1}, {16,16,0}}) {
        auto p=s; p.bits=spec[0];p.valid_bits=spec[1];p.floating_point=spec[2];add(p);
    }
    return result;
}
double ReadHiresSample(const std::byte* bytes, const TtpCommPcmFormat& p) noexcept {
    if (p.floating_point) {
        if (p.bits == 32) { float v;std::memcpy(&v,bytes,4);return v; }
        double v;std::memcpy(&v,bytes,8);return v;
    }
    uint32_t raw{};
    for (unsigned b=0;b<p.bits/8;++b) raw|=uint32_t(std::to_integer<unsigned char>(bytes[b]))<<(8*b);
    int64_t value{};
    if (p.bits == 8) value=int(raw)-128;
    else { const int64_t sign=int64_t{1}<<(p.bits-1);value=(int64_t(raw)^sign)-sign; }
    // Extensible integers are left aligned. Ignored padding is not audio.
    value >>= p.bits-p.valid_bits;
    return std::ldexp(double(value),1-int(p.valid_bits));
}
bool FiniteHiresPacket(std::span<const std::byte> bytes,const TtpCommPcmFormat& p) noexcept {
    if (!p.block_align || bytes.size()%p.block_align) return false;
    if (p.floating_point) for (size_t i=0;i<bytes.size();i+=p.bits/8)
        if (!std::isfinite(ReadHiresSample(bytes.data()+i,p))) return false;
    return true;
}
HiresPcmEncoder::~HiresPcmEncoder() { if (encoder_) api_.encoder_destroy(encoder_); }
bool HiresPcmEncoder::Open(HMODULE module,const TtpCommPcmFormat& s,
    const TtpCommPcmFormat& d,PcmPlaybackMode mode,size_t frames,std::wstring& error,int dither,
    uint32_t dither_seed) {
    if (encoder_) {api_.encoder_destroy(encoder_);encoder_=nullptr;}
    if (s.channels != d.channels || s.sample_rate != d.sample_rate ||
        s.channel_mask != d.channel_mask ||
        (mode == PcmPlaybackMode::bit_exact && !CanPreserveHiresPcm(s,d))) {
        error=L"Output format would change channel mapping, rate or source precision";return false;
    }
    if (mode==PcmPlaybackMode::high_precision && !d.floating_point && d.valid_bits<24) {
        error=L"High precision output requires at least 24 valid integer bits or floating point; select compatible mode for this device";
        return false;
    }
    if (!ttpcomm::host::QueryAudio(module,api_)) {
        error=L"High precision output requires ttpcomm audio ABI 2";return false;
    }
    TtpCommAudioEncoderConfig config{sizeof(config),d,
        mode==PcmPlaybackMode::high_precision && !d.floating_point && dither ? 1u:0u,dither_seed};
    encoder_=api_.encoder_create(&config);
    if (!encoder_) {error=L"Unable to create final PCM encoder";return false;}
    source_=s;target_=d;mode_=mode;
    scratch_.resize(frames*s.channels);gains_.resize(s.channels,1.);
    return true;
}
bool HiresPcmEncoder::EncodeHalf(std::span<const double> samples,
    std::span<std::byte> output) noexcept {
    if (!encoder_ || samples.size()%target_.channels || output.size()>UINT32_MAX ||
        samples.size()/target_.channels>UINT32_MAX) return false;
    TtpCommAudioEncodeResult result{sizeof(result)};
    return api_.encoder_encode(encoder_,samples.data(),uint32_t(samples.size()/target_.channels),
        nullptr,0,output.data(),uint32_t(output.size()),&result)==TTPCOMM_OK;
}
bool HiresPcmEncoder::Convert(std::span<const std::byte> input,float left,float right,
    std::span<std::byte> output) noexcept {
    if (!encoder_ || !source_.block_align || input.size()%source_.block_align ||
        !std::isfinite(left) || !std::isfinite(right)) return false;
    const size_t frames=input.size()/source_.block_align,samples=frames*source_.channels;
    if (samples>scratch_.size() || output.size()!=frames*target_.block_align ||
        output.size()>UINT32_MAX) return false;
    if (mode_==PcmPlaybackMode::bit_exact) {
        if (left!=1 || right!=1 || !FiniteHiresPacket(input,source_)) return false;
        if (source_.bits==target_.bits && source_.valid_bits==target_.valid_bits &&
            source_.floating_point==target_.floating_point) {
            std::memcpy(output.data(),input.data(),input.size());return true;
        }
    }
    for (size_t i=0;i<samples;++i)
        scratch_[i]=ReadHiresSample(input.data()+i*(source_.bits/8),source_)*0.5;
    for (size_t c=0;c<gains_.size();++c)
        gains_[c]=std::clamp(double(c==0?left:c==1?right:std::max(left,right)),0.,1.);
    TtpCommAudioEncodeResult result{sizeof(result)};
    return api_.encoder_encode(encoder_,scratch_.data(),uint32_t(frames),
        gains_.data(),uint32_t(gains_.size()),output.data(),uint32_t(output.size()),&result)==TTPCOMM_OK;
}
void HiresPcmEncoder::Reset() noexcept { if (encoder_) api_.encoder_reset(encoder_); }
} // namespace ttplayer::audio
