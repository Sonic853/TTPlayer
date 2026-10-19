#include <ttpcomm/client.h>
#include <ttpcomm/runtime_pcm.h>
#include "ttplayer/audio/pcm_output_transform.h"
#include "ttplayer/audio/pcm_effects.h"
#include "ttplayer/audio/winamp_dsp.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace ttplayer::audio {
namespace {
using InputEncoding = ttpcomm::host::pcm::Encoding;
InputEncoding DescribeInput(const WAVEFORMATEX& format) noexcept {
    return ttpcomm::host::pcm::Describe(format);
}
} // namespace

struct PcmOutputTransform::Impl {
    WAVEFORMATEX input{};
    WAVEFORMATEX output{};
    bool input_floating_point{};
    WORD input_valid_bits{};
    DWORD input_channel_mask{};
    std::wstring error;
    ttpcomm::client::ModuleReference module_reference;
    void* resampler{};
    bool input_ended{};
    const TtpCommRuntimeApi* runtime{};
    void* quantizer{};
    void* dsp_quantizer{};
    WinampDspChain* dsp{};
    bool dsp_enabled{true};
    int dither_mode{};
    std::vector<std::wstring> diagnostics;

#if defined(_MSC_VER) && defined(_M_IX86)
    static void Destroy(void* object) noexcept {
        ttpcomm::client::Destroy(object);
    }

    static bool Initialize(void* object, DWORD input_rate, DWORD output_rate,
                           WORD channels, bool high_quality) noexcept {
        return ttpcomm::client::InitializeResampler(object, input_rate, output_rate, channels, high_quality);
    }

    static int Process(void* object, const double* input, int input_samples,
                       double* output, int output_capacity) noexcept {
        return ttpcomm::client::Resample(object, input, input_samples, output, output_capacity);
    }

    static void ResetObject(void* object) noexcept {
        ttpcomm::client::Reset(object, 3);
    }
#endif

    ~Impl() {
        if(runtime) { runtime->quantizer_destroy(quantizer); runtime->quantizer_destroy(dsp_quantizer); }
#if defined(_MSC_VER) && defined(_M_IX86)
        Destroy(resampler);
#endif
    }

    bool InitializeDither(int configured) {
        dither_mode=configured;
        runtime=ttpcomm::host::Runtime();
        const auto format=ttpcomm::host::pcm::Format(output);
        quantizer=runtime?runtime->quantizer_create(&format,configured):nullptr;
        if(!quantizer) error=L"The rebuilt ttpcomm.dll output quantizer is unavailable";
        return quantizer!=nullptr;
    }
    bool Encode(const std::vector<double>& samples,std::vector<std::byte>& bytes) {
        const uint64_t size=uint64_t(samples.size())*(output.wBitsPerSample/8);
        if(!runtime || !quantizer || samples.size()>UINT32_MAX || size>UINT32_MAX || size>bytes.max_size()) {
            error=L"PCM output block is too large or the quantizer is unavailable";return false;
        }
        bytes.resize(static_cast<size_t>(size));uint32_t produced{};
        const auto result=runtime->quantizer_encode(quantizer,samples.data(),static_cast<uint32_t>(samples.size()),
            bytes.data(),static_cast<uint32_t>(size),&produced);
        if(result!=TTPCOMM_OK || produced!=size) {error=L"The output quantizer rejected the PCM block";return false;}
        return true;
    }

};

PcmOutputTransform::PcmOutputTransform() : impl_(new Impl) {}

PcmOutputTransform::~PcmOutputTransform() { delete impl_; }

bool PcmOutputTransform::Open(const WAVEFORMATEX& input,
                              const PcmOutputTransformOptions& options,
                              HMODULE ttpcomm_module) {
    delete impl_;
    impl_ = new (std::nothrow) Impl;
    if (!impl_) return false;
    const InputEncoding encoding = DescribeInput(input);
    if (!encoding.supported) {
        impl_->error = L"The decoder returned an unsupported PCM format";
        return false;
    }
    impl_->input = input;
    // Keep a canonical tag internally; cbSize data has already been captured
    // above and Process must never dereference the caller's format again.
    impl_->input.wFormatTag = encoding.floating_point
        ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    impl_->input.cbSize = 0;
    impl_->input_floating_point = encoding.floating_point;
    impl_->input_valid_bits = encoding.valid_bits;
    impl_->input_channel_mask = encoding.channel_mask;
    const WORD bits = options.floating_point ? 64 : static_cast<WORD>(ResolveOutputBits(
        options.output_bits, options.source_bits ? options.source_bits :
        (encoding.floating_point ? 0 : encoding.valid_bits)));
    const DWORD rate = options.resample_rate > 0
        ? static_cast<DWORD>(options.resample_rate) : input.nSamplesPerSec;
    if (rate < 1000 || rate > 768000) {
        impl_->error = L"The configured output sample rate is invalid";
        return false;
    }
    impl_->output.wFormatTag = options.floating_point
        ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    impl_->output.nChannels = input.nChannels;
    impl_->output.nSamplesPerSec = rate;
    impl_->output.wBitsPerSample = bits;
    impl_->output.nBlockAlign = static_cast<WORD>(
        impl_->output.nChannels * (bits / 8));
    impl_->output.nAvgBytesPerSec =
        impl_->output.nSamplesPerSec * impl_->output.nBlockAlign;

    if (rate == input.nSamplesPerSec) {
        return impl_->InitializeDither(options.dither);
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    if (!ttpcomm_module) {
        impl_->error = L"ttpcomm.dll is required for configured SSRC output";
        return false;
    }
    impl_->module_reference = ttpcomm::client::ModuleReference(ttpcomm_module);
    impl_->resampler = ttpcomm::client::CreateResampler(
        impl_->module_reference.get(), std::clamp(options.ssrc_mode, 0, 2));
    if (!impl_->resampler || !Impl::Initialize(
            impl_->resampler, input.nSamplesPerSec, rate, input.nChannels,
            options.ssrc_mode == 1)) {
        Impl::Destroy(impl_->resampler);
        impl_->resampler = nullptr;
        impl_->error = L"ttpcomm.dll ordinal 102 rejected the PCM format";
        return false;
    }
    return impl_->InitializeDither(options.dither);
#else
    static_cast<void>(ttpcomm_module);
    impl_->error = L"ttpcomm.dll ordinal 102 requires the Win32/x86 build";
    return false;
#endif
}

bool PcmOutputTransform::Process(const std::vector<std::byte>& input,
                                 std::vector<std::byte>& output,
                                 bool end_of_stream) {
    output.clear();
    if (!impl_ || impl_->output.nBlockAlign == 0) return false;
    if (input.size() % impl_->input.nBlockAlign != 0) {
        impl_->error = L"The decoder returned a partial PCM frame";
        return false;
    }
    std::vector<double> samples;
    // Preserve the valid-bit count captured before the caller's extensible
    // format went out of scope, and convert once per block inside the DLL.
    auto format=ttpcomm::host::pcm::Format(impl_->input);
    format.valid_bits=impl_->input_valid_bits;format.channel_mask=impl_->input_channel_mask;
    const uint64_t count=uint64_t(input.size()/impl_->input.nBlockAlign)*impl_->input.nChannels;
    if(!impl_->runtime || input.size()>UINT32_MAX || count>UINT32_MAX || count>samples.max_size()) return false;
    samples.resize(static_cast<size_t>(count));uint32_t produced{};
    if(impl_->runtime->pcm_decode(&format,TTPCOMM_PCM_HALF,input.data(),static_cast<uint32_t>(input.size()),
        samples.data(),static_cast<uint32_t>(count),&produced)!=TTPCOMM_OK || produced!=count) return false;

    return ProcessSamples(std::move(samples), output, end_of_stream);
}

bool PcmOutputTransform::ProcessSamples(std::vector<double> samples,
                                       std::vector<std::byte>& output,
                                       bool end_of_stream) {
    output.clear();
    if (!impl_ || impl_->output.nBlockAlign == 0) return false;
    if (samples.size() % impl_->input.nChannels != 0 ||
        samples.size() > static_cast<size_t>(INT_MAX)) {
        impl_->error = L"The processor returned an invalid PCM block";
        return false;
    }
    if (impl_->input_ended) {
        if (samples.empty()) return true;
        impl_->error = L"PCM input after end of stream requires Reset";
        return false;
    }
    if (samples.empty() && !end_of_stream) return true;

#if defined(_MSC_VER) && defined(_M_IX86)
    if (impl_->resampler) {
        const size_t input_frames = samples.size() / impl_->input.nChannels;
        const long double ratio = static_cast<long double>(
            impl_->output.nSamplesPerSec) / impl_->input.nSamplesPerSec;
        const size_t output_frames = static_cast<size_t>(std::ceil(
            (static_cast<long double>(input_frames) + 256.0L) * ratio)) + 4096;
        if (output_frames > static_cast<size_t>(INT_MAX) /
                                impl_->output.nChannels) {
            impl_->error = L"SSRC output block is too large";
            return false;
        }
        std::vector<double> converted(
            output_frames * impl_->output.nChannels);
        const int produced = Impl::Process(
            impl_->resampler, samples.empty() ? nullptr : samples.data(),
            static_cast<int>(samples.size()), converted.data(),
            static_cast<int>(converted.size()));
        if (produced < 0 || static_cast<size_t>(produced) > converted.size()) {
            impl_->error = L"ttpcomm.dll ordinal 102 failed while resampling";
            return false;
        }
        converted.resize(static_cast<size_t>(produced));
        samples = std::move(converted);

        if (end_of_stream) {
            std::vector<double> tail(4096U * impl_->output.nChannels);
            // 004B1375 repeats the null-input ordinal-102 call until its
            // queue is empty. This drains already produced PCM; it must not
            // call Reset/Finish or submit synthetic zeros to the equalizer.
            for (;;) {
                const int tail_count = Impl::Process(
                    impl_->resampler, nullptr, 0, tail.data(),
                    static_cast<int>(tail.size()));
                if (tail_count < 0 || static_cast<size_t>(tail_count) > tail.size() ||
                    samples.size() > static_cast<size_t>(INT_MAX) - tail_count) {
                    impl_->error = L"ttpcomm.dll ordinal 102 failed while draining";
                    return false;
                }
                if (tail_count == 0) break;
                samples.insert(samples.end(), tail.begin(), tail.begin() + tail_count);
            }
        }
    }
#else
    static_cast<void>(end_of_stream);
#endif
    impl_->input_ended = end_of_stream;
    if (impl_->dsp_enabled && impl_->dsp && impl_->dsp->ActiveCount() && !samples.empty()) {
        // 004AC166 quantizes to 16 bits WITH the configured dither before
        // ModifySamples. 004AA8B0 subsequently expands/truncates those exact
        // integers; it neither recovers lost source bits nor dithers twice.
        if (!impl_->dsp_quantizer) {
            auto format=ttpcomm::host::pcm::Format(impl_->output);
            format.bits=format.valid_bits=16;format.floating_point=0;
            format.block_align=format.channels*2;
            impl_->dsp_quantizer=impl_->runtime->quantizer_create(&format,impl_->dither_mode);
        }
        if(!impl_->dsp_quantizer){impl_->error=L"Unable to create the Winamp PCM quantizer";return false;}
        std::vector<std::int16_t> pcm(samples.size());uint32_t written{};
        if(pcm.size()>UINT32_MAX/2 || impl_->runtime->quantizer_encode(impl_->dsp_quantizer,
            samples.data(),static_cast<uint32_t>(samples.size()),pcm.data(),static_cast<uint32_t>(pcm.size()*2),
            &written)!=TTPCOMM_OK || written!=pcm.size()*2){impl_->error=L"Winamp PCM quantization failed";return false;}
        impl_->dsp->Process(pcm,impl_->output.nChannels,static_cast<int>(impl_->output.nSamplesPerSec),&impl_->diagnostics);
        if(impl_->output.wFormatTag==WAVE_FORMAT_IEEE_FLOAT) {
            for(size_t i=0;i<pcm.size();++i)samples[i]=pcm[i]/65536.0;
            return ttpcomm::host::pcm::EncodeHalf(samples,impl_->output,output);
        }
        const unsigned bytes=impl_->output.wBitsPerSample/8;
        output.resize(pcm.size()*bytes);
        for(size_t i=0;i<pcm.size();++i) {
            const uint32_t value=bytes==1 ? static_cast<uint32_t>((pcm[i]>>8)+128) :
                static_cast<uint32_t>(static_cast<int32_t>(pcm[i])) << (8*(bytes-2));
            for(unsigned b=0;b<bytes;++b)output[i*bytes+b]=std::byte((value>>(8*b))&255);
        }
        return true;
    }
    return impl_->Encode(samples, output);
}

void PcmOutputTransform::SetWinampDsp(WinampDspChain* chain, bool enabled) noexcept {
    if(impl_){impl_->dsp=chain;impl_->dsp_enabled=enabled;}
}
std::vector<std::wstring> PcmOutputTransform::TakeDiagnostics() {
    return impl_ ? std::exchange(impl_->diagnostics,{}) : std::vector<std::wstring>{};
}
void PcmOutputTransform::Reset() noexcept {
    if (!impl_) return;
    impl_->input_ended = false;
#if defined(_MSC_VER) && defined(_M_IX86)
    Impl::ResetObject(impl_->resampler);
#endif
    if(impl_->runtime) { impl_->runtime->quantizer_reset(impl_->quantizer); impl_->runtime->quantizer_reset(impl_->dsp_quantizer); }
}

WAVEFORMATEX PcmOutputTransform::OutputFormat() const noexcept {
    return impl_ ? impl_->output : WAVEFORMATEX{};
}

bool PcmOutputTransform::UsesOrdinal102() const noexcept {
    return impl_ && impl_->resampler;
}

const std::wstring& PcmOutputTransform::Error() const noexcept {
    static const std::wstring empty;
    return impl_ ? impl_->error : empty;
}

} // namespace ttplayer::audio
