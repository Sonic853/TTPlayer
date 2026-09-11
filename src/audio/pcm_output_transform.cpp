#include "ttplayer/audio/pcm_output_transform.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace ttplayer::audio {
namespace {

struct InputEncoding {
    bool supported{};
    bool floating_point{};
    WORD valid_bits{};
    DWORD channel_mask{};
};

bool IsWaveSubtype(const GUID& subtype, WORD tag) noexcept {
    static constexpr std::array<std::uint8_t, 8> tail{
        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    return subtype.Data1 == tag && subtype.Data2 == 0 &&
           subtype.Data3 == 0x0010 &&
           std::equal(tail.begin(), tail.end(), subtype.Data4);
}

InputEncoding DescribeInput(const WAVEFORMATEX& format) noexcept {
    InputEncoding result;
    if (format.nChannels == 0 || format.nSamplesPerSec == 0 ||
        format.nBlockAlign == 0 || format.wBitsPerSample == 0) {
        return result;
    }
    const size_t container_bytes = (format.wBitsPerSample + 7U) / 8U;
    if (container_bytes * format.nChannels != format.nBlockAlign) {
        return result;
    }
    result.valid_bits = format.wBitsPerSample;
    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        result.supported = format.wBitsPerSample == 8 ||
                           format.wBitsPerSample == 16 ||
                           format.wBitsPerSample == 24 ||
                           format.wBitsPerSample == 32;
        return result;
    }
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        result.floating_point = true;
        result.supported = format.wBitsPerSample == 32 ||
                           format.wBitsPerSample == 64;
        return result;
    }
    if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return result;
    }

    // The caller supplies the complete cbSize-qualified object.  This is the
    // same 18+22-byte contract inspected by FUN_004C8492 before it selects a
    // decoder/output subtype.
    const auto& extended =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    result.channel_mask = extended.dwChannelMask;
    result.valid_bits = extended.Samples.wValidBitsPerSample == 0
        ? format.wBitsPerSample : extended.Samples.wValidBitsPerSample;
    if (IsWaveSubtype(extended.SubFormat, WAVE_FORMAT_PCM)) {
        result.supported = (format.wBitsPerSample == 8 ||
                            format.wBitsPerSample == 16 ||
                            format.wBitsPerSample == 24 ||
                            format.wBitsPerSample == 32) &&
                           result.valid_bits != 0 &&
                           result.valid_bits <= format.wBitsPerSample &&
                           (format.wBitsPerSample != 8 ||
                            result.valid_bits == 8);
    } else if (IsWaveSubtype(extended.SubFormat, WAVE_FORMAT_IEEE_FLOAT)) {
        result.floating_point = true;
        result.supported = (format.wBitsPerSample == 32 ||
                            format.wBitsPerSample == 64) &&
                           result.valid_bits == format.wBitsPerSample;
    }
    return result;
}

double DecodeSample(const std::uint8_t* value,
                    const WAVEFORMATEX& format,
                    bool floating_point,
                    WORD valid_bits) noexcept {
    if (floating_point &&
        format.wBitsPerSample == 32) {
        float number{};
        std::memcpy(&number, value, sizeof(number));
        return std::isfinite(number) ? static_cast<double>(number) * 0.5 : 0.0;
    }
    if (floating_point &&
        format.wBitsPerSample == 64) {
        double number{};
        std::memcpy(&number, value, sizeof(number));
        return std::isfinite(number) ? number * 0.5 : 0.0;
    }
    if (format.wBitsPerSample == 8)
        return (static_cast<int>(*value) - 128) / 256.0;
    const size_t bytes = format.wBitsPerSample / 8U;
    std::uint64_t raw{};
    for (size_t index = 0; index < bytes; ++index)
        raw |= static_cast<std::uint64_t>(value[index]) << (index * 8U);
    const unsigned container_bits = format.wBitsPerSample;
    const std::uint64_t sign = std::uint64_t{1} << (container_bits - 1U);
    std::int64_t number = static_cast<std::int64_t>(
        (raw ^ sign) - sign);
    if (valid_bits < container_bits)
        number >>= container_bits - valid_bits;
    return static_cast<double>(number) /
           std::ldexp(1.0, static_cast<int>(valid_bits));
}

// CDither::Initialize (004AAC63) and CDither::Quantize (004AAEC1) use a
// fixed, executable-resident noise-shaping catalogue.  The values below are
// the exact doubles at 00517DC0 (eight rows, 0xA8 bytes per row); each
// non-zero value was originally promoted from a binary32 coefficient.
constexpr size_t kDitherHistoryLength = 21;
constexpr size_t kDitherShuffleSize = 0x61;
constexpr size_t kDitherNoiseSize = 0x4000;
constexpr std::array<DWORD, 6> kDitherSampleRates{
    0, 48000, 44100, 37800, 32000, 22050};
constexpr std::array<size_t, 8> kDitherTapCounts{
    1, 16, 20, 16, 16, 15, 16, 15};
constexpr std::array<std::array<double, kDitherHistoryLength>, 8>
    kDitherCoefficients{{
        {{-1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-2.87207293510437, 5.041323184967041,
          -6.2442994117736816, 5.8483986854553223,
          -3.706754207611084, 1.0495119094848633,
          1.1830236911773682, -2.1126792430877686,
          1.9094531536102295, -0.99913084506988525,
          0.17090806365013123, 0.32615602016448975,
          -0.39127644896507263, 0.26876461505889893,
          -0.0976761057972908, 0.023473845794796944,
          0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-2.6773197650909424, 4.8308925628662109,
          -6.5701103210449219, 7.4572014808654785,
          -6.7263274192810059, 4.8481650352478027,
          -2.0412089824676514, -0.70063591003417969,
          2.95375657081604, -4.0800385475158691,
          4.1845216751098633, -3.3311812877655029,
          2.117992639541626, -0.879302978515625,
          0.031759146600961685, 0.4238278865814209,
          -0.4788210391998291, 0.35490813851356506,
          -0.17496839165687561, 0.06090816855430603, 0.0}},
        {{-1.6335992813110352, 2.2615492343902588,
          -2.4077029228210449, 2.634171724319458,
          -2.1440362930297852, 1.8153258562088013,
          -1.0816224813461304, 0.703026533126831,
          -0.15991993248462677, -0.041549518704414368,
          0.29416576027870178, -0.25183168053627014,
          0.27766478061676025, -0.15785403549671173,
          0.10165894031524658, -0.016833892092108727,
          0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-0.82901298999786377, 0.9892265796661377,
          -0.59825712442398071, 1.0028809309005737,
          -0.59938216209411621, 0.79502451419830322,
          -0.42723315954208374, 0.5449252724647522,
          -0.30792605876922607, 0.36871799826622009,
          -0.187920480966568, 0.22611270844936371,
          -0.10573341697454453, 0.11435490846633911,
          -0.0388006791472435, 0.040842197835445404,
          0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-0.065229974687099457, 0.54981261491775513,
          0.40278548002243042, 0.31783768534660339,
          0.28201797604560852, 0.16985194385051727,
          0.15433363616466522, 0.12507140636444092,
          0.089039452373981476, 0.064410120248794556,
          0.047146003693342209, 0.032805237919092178,
          0.028495194390416145, 0.011695005930960178,
          0.011831838637590408, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.0}},
        {{-2.3925774097442627, 3.4350297451019287,
          -3.185370922088623, 1.8117271661758423,
          0.20124770700931549, -1.4759907722473145,
          1.7210904359817505, -0.97746700048446655,
          0.13790138065814972, 0.38185903429985046,
          -0.27421241998672485, -0.066584214568138123,
          0.35223302245140076, -0.37672343850135803,
          0.23964276909828186, -0.068674825131893158,
          0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-2.0833916664123535, 3.0418450832366943,
          -3.2047898769378662, 2.7571926116943359,
          -1.4978630542755127, 0.34275946021080017,
          0.71733748912811279, -1.073705792427063,
          1.0225815773010254, -0.56649994850158691,
          0.20968692004680634, 0.065378531813621521,
          -0.10322438180446625, 0.067442022264003754,
          0.00495197344571352, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.0}}
    }};

// Exact constants at 00526440 and 00526608.  The former is 1/RAND_MAX;
// spelling both through their image bit patterns makes accidental decimal
// rounding changes visible during review.
constexpr double kLegacyRandScale =
    std::bit_cast<double>(UINT64_C(0x3F00002000400080));
constexpr double kLegacyDitherAmplitude =
    std::bit_cast<double>(UINT64_C(0x3FC70A3D70A3D70A));
static_assert(RAND_MAX == 0x7fff);

size_t DitherFilterIndex(int selector, DWORD sample_rate) noexcept {
    size_t result = 1;
    while (result < kDitherSampleRates.size() &&
           kDitherSampleRates[result] != sample_rate) {
        ++result;
    }
    // 004AAC8A..004AACA6: selector 1 is the one-tap row regardless of rate;
    // an unknown rate also uses row zero.  The fourth UI choice selects the
    // alternate 48/44.1-kHz rows six and seven.
    if (selector == 1 || result == kDitherSampleRates.size()) result = 0;
    if (selector == 3 && (result == 1 || result == 2)) result += 5;
    return result;
}

} // namespace

struct PcmOutputTransform::Impl {
    WAVEFORMATEX input{};
    WAVEFORMATEX output{};
    bool input_floating_point{};
    WORD input_valid_bits{};
    DWORD input_channel_mask{};
    std::wstring error;
    void* resampler{};
    int dither_selector{};
    size_t dither_filter{};
    size_t dither_taps{};
    size_t dither_cursor{};
    std::vector<double> dither_noise;
    std::vector<double> quantization_error;

#if defined(_MSC_VER) && defined(_M_IX86)
    static void Destroy(void* object) noexcept {
        if (!object) return;
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, int)>(table[0])(
                object, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static bool Initialize(void* object, DWORD input_rate, DWORD output_rate,
                           WORD channels, bool high_quality) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            return reinterpret_cast<unsigned char (__thiscall*)(
                void*, DWORD, DWORD, WORD, unsigned char)>(table[1])(
                    object, input_rate, output_rate, channels,
                    high_quality ? 1 : 0) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static int Process(void* object, const double* input, int input_samples,
                       double* output, int output_capacity) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            return reinterpret_cast<int (__thiscall*)(
                void*, const double*, int, double*, int)>(table[2])(
                    object, input, input_samples, output, output_capacity);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    static void ResetObject(void* object) noexcept {
        if (!object) return;
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*)>(table[3])(object);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
#endif

    ~Impl() {
#if defined(_MSC_VER) && defined(_M_IX86)
        Destroy(resampler);
#endif
    }

    void InitializeDither(int configured) {
        dither_selector = 0;
        dither_filter = 0;
        dither_taps = 0;
        dither_cursor = 0;
        dither_noise.clear();
        quantization_error.clear();
        configured = std::clamp(configured, 0, 4);
        if (configured == 0) return;

        // FUN_004AB58B passes Device/@Dither-1 in ECX, output channels in
        // EAX, output rate/bits on the stack, distribution 1 and scale 0.18.
        // Distribution 1 is the only executable path used by the player and
        // is the two-shuffled-uniform TPDF branch at 004AADCF.
        dither_selector = configured - 1;
        dither_filter = DitherFilterIndex(
            dither_selector, output.nSamplesPerSec);
        dither_taps = kDitherTapCounts[dither_filter];
        quantization_error.assign(
            static_cast<size_t>(output.nChannels) * kDitherHistoryLength,
            0.0);
        dither_noise.resize(kDitherNoiseSize);

        std::array<int, kDitherShuffleSize> shuffled{};
        for (auto& value : shuffled) value = std::rand();
        const auto next = [&shuffled]() {
            // Original x86 uses signed IDIV by 0x61. MSVCRT rand is
            // non-negative, so this is exactly the same slot operation.
            const size_t slot = static_cast<size_t>(std::rand()) %
                                kDitherShuffleSize;
            const int value = shuffled[slot];
            shuffled[slot] = std::rand();
            return value;
        };
        for (auto& value : dither_noise) {
            // Keep the two conversions/multiplications separate: that is the
            // x87 instruction order at 004AAE0D..004AAE2D.
            const double first = static_cast<double>(next()) *
                                 kLegacyRandScale;
            const double second = static_cast<double>(next()) *
                                  kLegacyRandScale;
            value = (first - second) * kLegacyDitherAmplitude;
        }
    }

    std::int64_t Quantize(double sample, size_t channel) noexcept {
        double shaped = sample;
        if (!dither_noise.empty()) {
            shaped += dither_noise[dither_cursor & (kDitherNoiseSize - 1)];
            ++dither_cursor;
            double* history = quantization_error.data() +
                              channel * kDitherHistoryLength;
            if (dither_selector > 0 && dither_taps > 0) {
                const auto& coefficients =
                    kDitherCoefficients[dither_filter];
                for (size_t index = 0; index < dither_taps; ++index)
                    shaped += coefficients[index] * history[index];
            }
            if (dither_taps > 1) {
                std::move_backward(history, history + dither_taps - 1,
                                   history + dither_taps);
            }

            // 004AAEC1 adds the 005264A0 magic double and extracts the low
            // integer bits. Under TTPlayer's default x87 control word this is
            // round-to-nearest-even; nearbyint retains that rounding-mode
            // contract instead of llround's half-away-from-zero rule.
            const double rounded = std::nearbyint(shaped);
            history[0] = shaped - rounded;
            if (rounded >= static_cast<double>(
                               std::numeric_limits<std::int64_t>::max()))
                return std::numeric_limits<std::int64_t>::max();
            if (rounded <= static_cast<double>(
                               std::numeric_limits<std::int64_t>::min()))
                return std::numeric_limits<std::int64_t>::min();
            return static_cast<std::int64_t>(rounded);
        }
        const double rounded = std::nearbyint(shaped);
        if (rounded >= static_cast<double>(
                           std::numeric_limits<std::int64_t>::max()))
            return std::numeric_limits<std::int64_t>::max();
        if (rounded <= static_cast<double>(
                           std::numeric_limits<std::int64_t>::min()))
            return std::numeric_limits<std::int64_t>::min();
        return static_cast<std::int64_t>(rounded);
    }

    bool Encode(const std::vector<double>& samples,
                std::vector<std::byte>& bytes) {
        const size_t sample_bytes = output.wBitsPerSample / 8U;
        if (samples.size() > std::numeric_limits<size_t>::max() / sample_bytes) {
            error = L"PCM output block is too large";
            return false;
        }
        bytes.resize(samples.size() * sample_bytes);
        if (output.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            for (size_t index = 0; index < samples.size(); ++index) {
                // Internal SSRC samples use half scale; the public IEEE
                // format uses full scale, just like DecodeSample's input.
                const double value = std::isfinite(samples[index])
                    ? samples[index] * 2.0 : 0.0;
                std::memcpy(bytes.data() + index * sizeof(value),
                            &value, sizeof(value));
            }
            return true;
        }
        auto* destination = reinterpret_cast<std::uint8_t*>(bytes.data());
        const double scale = std::ldexp(1.0, output.wBitsPerSample);
        for (size_t index = 0; index < samples.size(); ++index) {
            const size_t channel = index % output.nChannels;
            const double clean = std::isfinite(samples[index])
                ? samples[index] : 0.0;
            // DecodeSample represents full scale as +/-0.5; therefore this
            // 2^bits factor is the same integer-domain value as the original
            // double stream multiplied by 2^(bits-1) in FUN_004AAFAF.
            const std::int64_t number = Quantize(clean * scale, channel);
            std::uint8_t* value = destination + index * sample_bytes;
            if (output.wBitsPerSample == 8) {
                const auto limited = std::clamp<std::int64_t>(
                    number, -128, 127);
                value[0] = static_cast<std::uint8_t>(limited + 128);
            } else if (output.wBitsPerSample == 16) {
                const auto limited = static_cast<std::int16_t>(
                    std::clamp<std::int64_t>(number, -32768, 32767));
                std::memcpy(value, &limited, sizeof(limited));
            } else if (output.wBitsPerSample == 24) {
                const auto limited = static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(std::clamp<std::int64_t>(
                        number, -8388608, 8388607)));
                value[0] = static_cast<std::uint8_t>(limited);
                value[1] = static_cast<std::uint8_t>(limited >> 8);
                value[2] = static_cast<std::uint8_t>(limited >> 16);
            } else {
                const auto limited = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(number, INT32_MIN, INT32_MAX));
                std::memcpy(value, &limited, sizeof(limited));
            }
        }
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
    const int requested_bits = options.output_bits;
    const WORD bits = options.floating_point ? 64 :
        requested_bits == 8 || requested_bits == 16 ||
                      requested_bits == 24 || requested_bits == 32
        ? static_cast<WORD>(requested_bits) : 16;
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
        impl_->InitializeDither(options.dither);
        return true;
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    if (!ttpcomm_module) {
        impl_->error = L"ttpcomm.dll is required for configured SSRC output";
        return false;
    }
    const auto factory = reinterpret_cast<void* (__cdecl*)(int)>(
        GetProcAddress(ttpcomm_module, MAKEINTRESOURCEA(102)));
    if (!factory) {
        impl_->error = L"ttpcomm.dll ordinal 102 is unavailable";
        return false;
    }
    __try {
        impl_->resampler = factory(std::clamp(options.ssrc_mode, 0, 2));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        impl_->resampler = nullptr;
    }
    if (!impl_->resampler || !Impl::Initialize(
            impl_->resampler, input.nSamplesPerSec, rate, input.nChannels,
            options.ssrc_mode == 1)) {
        Impl::Destroy(impl_->resampler);
        impl_->resampler = nullptr;
        impl_->error = L"ttpcomm.dll ordinal 102 rejected the PCM format";
        return false;
    }
    impl_->InitializeDither(options.dither);
    return true;
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
    const size_t sample_bytes = (impl_->input.wBitsPerSample + 7U) / 8U;
    std::vector<double> samples(input.size() / sample_bytes);
    const auto* source = reinterpret_cast<const std::uint8_t*>(input.data());
    for (size_t index = 0; index < samples.size(); ++index)
        samples[index] = DecodeSample(source + index * sample_bytes,
                                      impl_->input,
                                      impl_->input_floating_point,
                                      impl_->input_valid_bits);

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
            const int tail_count = Impl::Process(
                impl_->resampler, nullptr, 0, tail.data(),
                static_cast<int>(tail.size()));
            if (tail_count < 0 || static_cast<size_t>(tail_count) > tail.size()) {
                impl_->error = L"ttpcomm.dll ordinal 102 failed while flushing";
                return false;
            }
            samples.insert(samples.end(), tail.begin(),
                           tail.begin() + tail_count);
        }
    }
#else
    static_cast<void>(end_of_stream);
#endif
    return impl_->Encode(samples, output);
}

void PcmOutputTransform::Reset() noexcept {
    if (!impl_) return;
#if defined(_MSC_VER) && defined(_M_IX86)
    Impl::ResetObject(impl_->resampler);
#endif
    // FUN_004AAEA2 preserves only CDither's selector/channel fields and
    // clears the remaining 0x206B0 bytes after a seek.  In particular the
    // pre-generated noise table and shaping tap count become zero until the
    // sound is reopened; reproduce that observable (if surprising) behavior.
    impl_->dither_filter = 0;
    impl_->dither_taps = 0;
    impl_->dither_cursor = 0;
    std::fill(impl_->dither_noise.begin(), impl_->dither_noise.end(), 0.0);
    std::fill(impl_->quantization_error.begin(),
              impl_->quantization_error.end(), 0.0);
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
