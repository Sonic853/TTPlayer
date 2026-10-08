#pragma once
#include <ttpcomm/pcm.h>
#include <ttpcomm/runtime_client.h>
#include <stdexcept>

namespace ttpcomm::host::pcm {
using ttpcomm::pcm::Domain;
using ttpcomm::pcm::Encoding;
using ttpcomm::pcm::Describe;
inline TtpCommPcmFormat Format(const WAVEFORMATEX& wave, bool padding=false) noexcept {
    const auto encoding=Describe(wave,padding);
    if(!encoding.supported) return {};
    return {sizeof(TtpCommPcmFormat),wave.nChannels,wave.nSamplesPerSec,wave.wBitsPerSample,
        encoding.valid_bits,wave.nBlockAlign,encoding.floating_point?1u:0u,encoding.channel_mask};
}
inline bool Decode(const void* input,size_t size,const WAVEFORMATEX& wave,
    std::vector<double>& samples,Domain domain,bool padding=false) {
    const auto api=Runtime();const auto format=Format(wave,padding);
    if(!api || !format.size || size>UINT32_MAX || size%format.block_align || (size && !input)) return false;
    const uint64_t count=uint64_t(size/format.block_align)*format.channels;
    if(count>UINT32_MAX || count>samples.max_size()) return false;
    samples.resize(static_cast<size_t>(count));uint32_t produced{};
    return api->pcm_decode(&format,domain==Domain::half?TTPCOMM_PCM_HALF:TTPCOMM_PCM_FULL,
        input,static_cast<uint32_t>(size),samples.data(),static_cast<uint32_t>(count),&produced)==TTPCOMM_OK && produced==count;
}
template<class Byte>
bool EncodeHalf(const std::vector<double>& samples,const WAVEFORMATEX& wave,std::vector<Byte>& bytes) {
    static_assert(sizeof(Byte)==1,"PCM output is a byte buffer");
    const auto api=Runtime();const auto format=Format(wave);
    if(!api || !format.size || samples.size()>UINT32_MAX || samples.size()%format.channels) return false;
    const uint64_t count=uint64_t(samples.size())*(format.bits/8);
    if(count>UINT32_MAX || count>bytes.max_size()) return false;
    bytes.resize(static_cast<size_t>(count));uint32_t produced{};
    return api->pcm_encode_half(&format,samples.data(),static_cast<uint32_t>(samples.size()),
        bytes.data(),static_cast<uint32_t>(count),&produced)==TTPCOMM_OK && produced==count;
}
inline void ApplyGainHalf(std::vector<double>& samples,double gain) {
    const auto api=Runtime();
    if(!api || samples.size()>UINT32_MAX || api->pcm_gain_half(samples.data(),static_cast<uint32_t>(samples.size()),gain)!=TTPCOMM_OK)
        throw std::runtime_error("The rebuilt ttpcomm.dll PCM runtime is unavailable");
}
}
