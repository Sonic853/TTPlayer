#include "builtin_acm_decoder.h"
#include "ttplayer/audio/native_output_contract.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mmreg.h>
#include <msacm.h>
#include <objbase.h>
#include <vector>

namespace ttplayer::plugins::detail {
namespace {
constexpr GUID decoder_id{
    0xcc94b115, 0xa1a7, 0x4362, {0x95, 0x23, 0x41, 0x23, 0xe2, 0xa0, 0xb3, 0xc7}};
constexpr GUID finish_id{
    0xf65853b2, 0x753c, 0x45ac, {0x97, 0x9f, 0x36, 0x6c, 0x03, 0xe4, 0xa4, 0xcf}};
struct Buffer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetLength(DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE Capacity(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Data(BYTE**, DWORD*) = 0;
};
struct Decoder : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(const WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Sizes(DWORD*, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE NeedsInput() = 0;
    virtual HRESULT STDMETHODCALLTYPE Available() = 0;
    virtual HRESULT STDMETHODCALLTYPE Input(Buffer*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Output(Buffer*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
};
struct Finish : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EndInput() = 0;
};
constexpr DWORD maximum = 64U * 1024U * 1024U;

HRESULT Result(MMRESULT result) noexcept {
    if (result == MMSYSERR_NOERROR) return S_OK;
    if (result == MMSYSERR_NOMEM) return E_OUTOFMEMORY;
    if (result == MMSYSERR_INVALPARAM || result == MMSYSERR_INVALFLAG) return E_INVALIDARG;
    if (result == ACMERR_NOTPOSSIBLE || result == MMSYSERR_NOTSUPPORTED ||
        result == MMSYSERR_NODRIVER) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    return E_FAIL;
}
template<class F> HRESULT Guard(F&& action) noexcept {
    try { return action(); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

class Acm final : public Decoder, public Finish {
    LONG references_{1};
    HACMSTREAM stream_{};
    std::vector<BYTE> source_format_;
    WAVEFORMATEXTENSIBLE output_format_{};
    std::vector<BYTE> input_;
    DWORD input_size_{}, suggested_input_{}, suggested_output_{};
    bool need_{true}, ended_{}, drained_{};

    ~Acm() { Close(); }
    void Close() noexcept {
        if (stream_) {
            acmStreamClose(stream_, 0);
            stream_ = nullptr;
        }
        input_.clear();
        source_format_.clear();
        input_size_ = suggested_input_ = suggested_output_ = 0;
        need_ = true;
        ended_ = drained_ = false;
    }
    HRESULT InitializeImpl(const WAVEFORMATEX& from, WAVEFORMATEX& to) {
        source_format_.resize(sizeof(WAVEFORMATEX) + from.cbSize);
        std::memcpy(source_format_.data(), &from, source_format_.size());
        const auto* source = reinterpret_cast<const WAVEFORMATEX*>(source_format_.data());
        WORD bits = from.wBitsPerSample;
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32) bits = 16;
        WORD tag = to.wFormatTag;
        MMRESULT opened = ACMERR_NOTPOSSIBLE;
        for (;;) {
            auto& output = output_format_.Format;
            output = {WAVE_FORMAT_EXTENSIBLE, from.nChannels, from.nSamplesPerSec,
                      0, 0, tag == WAVE_FORMAT_IEEE_FLOAT ? WORD{64} : bits, 22};
            const auto align = static_cast<DWORD>(output.nChannels) * output.wBitsPerSample / 8;
            const auto rate = static_cast<std::uint64_t>(align) * output.nSamplesPerSec;
            if (!align || align > MAXWORD || rate > MAXDWORD) return E_INVALIDARG;
            output.nBlockAlign = static_cast<WORD>(align);
            output.nAvgBytesPerSec = static_cast<DWORD>(rate);
            output_format_.Samples.wValidBitsPerSample = 0;
            output_format_.dwChannelMask = audio::LegacyOutputChannelMask(output.nChannels);
            output_format_.SubFormat = {tag, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
            // 004C9C1F: extensible then basic, float64 then PCM, finally PCM16.
            opened = acmStreamOpen(&stream_, nullptr, const_cast<WAVEFORMATEX*>(source),
                                   &output, nullptr, 0, 0, 0);
            if (opened == MMSYSERR_NOERROR) break;
            stream_ = nullptr;
            output.wFormatTag = tag;
            output.cbSize = 0;
            opened = acmStreamOpen(&stream_, nullptr, const_cast<WAVEFORMATEX*>(source),
                                   &output, nullptr, 0, 0, 0);
            if (opened == MMSYSERR_NOERROR) break;
            stream_ = nullptr;
            if (tag == WAVE_FORMAT_IEEE_FLOAT) tag = WAVE_FORMAT_PCM;
            else if (bits != 16) bits = 16;
            else return Result(opened);
        }
        if (!stream_) return E_UNEXPECTED;
        if (output_format_.Format.cbSize > to.cbSize)
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        // The original advances to the next block even when exactly aligned.
        auto input = static_cast<std::uint64_t>(from.nAvgBytesPerSec >> 5);
        if (from.nBlockAlign) input += from.nBlockAlign - input % from.nBlockAlign;
        if (!input || input > maximum / 2) return E_INVALIDARG;
        suggested_input_ = static_cast<DWORD>(input);
        auto result = Result(acmStreamSize(stream_, suggested_input_, &suggested_output_,
                                           ACM_STREAMSIZEF_SOURCE));
        if (FAILED(result)) return result;
        const auto& output = output_format_.Format;
        const auto minimum = output.nAvgBytesPerSec / 24;
        suggested_output_ = std::max<DWORD>(suggested_output_,
            minimum + output.nBlockAlign - minimum % output.nBlockAlign);
        if (!suggested_output_ || suggested_output_ > maximum) return E_INVALIDARG;
        input_.resize(suggested_input_ * 2);
        std::memcpy(&to, &output_format_, sizeof(WAVEFORMATEX) + output.cbSize);
        return S_OK;
    }

public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (InlineIsEqualGUID(id, IID_IUnknown) || InlineIsEqualGUID(id, decoder_id))
            *output = static_cast<Decoder*>(this);
        else if (InlineIsEqualGUID(id, finish_id)) *output = static_cast<Finish*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = InterlockedDecrement(&references_);
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Initialize(const WAVEFORMATEX* from, WAVEFORMATEX* to) override {
        if (!from || !to) return E_POINTER;
        if (stream_) return E_UNEXPECTED;
        if (!SupportsBuiltinAcmDecoder(*from) ||
            (to->wFormatTag != WAVE_FORMAT_PCM && to->wFormatTag != WAVE_FORMAT_IEEE_FLOAT))
            return E_INVALIDARG;
        const auto result = Guard([&] { return InitializeImpl(*from, *to); });
        if (FAILED(result)) Close();
        return result;
    }
    HRESULT STDMETHODCALLTYPE Sizes(DWORD* input, DWORD* output) override {
        if (!stream_) return E_UNEXPECTED;
        if (input) *input = suggested_input_;
        if (output) *output = suggested_output_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE NeedsInput() override {
        return !stream_ ? E_UNEXPECTED : need_ && !ended_ ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Available() override {
        return !stream_ ? E_UNEXPECTED : !need_ && !drained_ ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Input(Buffer* buffer) override {
        if (!stream_ || ended_) return E_UNEXPECTED;
        if (!buffer) return E_POINTER;
        BYTE* bytes{};
        DWORD count{}, capacity{};
        auto result = buffer->Data(&bytes, &count);
        if (SUCCEEDED(result)) result = buffer->Capacity(&capacity);
        if (FAILED(result)) return result;
        if (count > capacity || (!bytes && count)) return E_INVALIDARG;
        // A reader can advertise a packet larger than ACM's 1/32-second hint.
        // Unlike 004C9E7B, never overrun the first packet or discard pending
        // compressed input on a later append. Grow before copying, with a bound.
        if (count > maximum - input_size_)
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        if (input_size_ + count > input_.size()) {
            result = Guard([&]() -> HRESULT { input_.resize(input_size_ + count); return S_OK; });
            if (FAILED(result)) return result;
        }
        if (count) std::memcpy(input_.data() + input_size_, bytes, count);
        input_size_ += count;
        need_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Output(Buffer* buffer) override {
        if (!stream_) return E_UNEXPECTED;
        if (!buffer) return E_POINTER;
        auto result = buffer->SetLength(0);
        if (FAILED(result)) return result;
        if (need_ || drained_) return S_OK;
        BYTE* bytes{};
        DWORD unused{}, capacity{};
        result = buffer->Capacity(&capacity);
        if (SUCCEEDED(result)) result = buffer->Data(&bytes, &unused);
        if (FAILED(result)) return result;
        if (!bytes || capacity > maximum)
            return E_INVALIDARG;
        if (capacity < suggested_output_)
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        ACMSTREAMHEADER header{};
        header.cbStruct = sizeof(header);
        header.pbSrc = input_.data();
        header.cbSrcLength = static_cast<DWORD>(input_.size());
        header.pbDst = bytes;
        header.cbDstLength = capacity - capacity % output_format_.Format.nBlockAlign;
        result = Result(acmStreamPrepareHeader(stream_, &header, 0));
        if (FAILED(result)) return result;
        // ACM codecs such as G.711 reject oversized input instead of partially
        // consuming it when the destination is too short. The negotiated
        // source hint is guaranteed to fit Sizes()'s output packet.
        const DWORD submitted = std::min(input_size_, suggested_input_);
        header.cbSrcLength = submitted;
        // END removes BLOCKALIGN so an ACM codec can finish a short last block.
        // Prepared source capacity stays nonzero even for a zero-input flush.
        const DWORD flags = ended_ && submitted == input_size_
            ? ACM_STREAMCONVERTF_END : ACM_STREAMCONVERTF_BLOCKALIGN;
        result = Result(acmStreamConvert(stream_, &header, flags));
        header.cbSrcLength = static_cast<DWORD>(input_.size());
        const auto unprepared = acmStreamUnprepareHeader(stream_, &header, 0);
        if (unprepared != MMSYSERR_NOERROR) {
            // The stream is synchronous. On a broken codec, release its buffer
            // reference before the host's output buffer can leave this call.
            acmStreamReset(stream_, 0);
            acmStreamUnprepareHeader(stream_, &header, 0);
            Close();
            return Result(unprepared);
        }
        if (FAILED(result)) return result;
        if (header.cbSrcLengthUsed > submitted ||
            header.cbDstLengthUsed > header.cbDstLength ||
            header.cbDstLengthUsed % output_format_.Format.nBlockAlign)
            return E_UNEXPECTED;
        input_size_ -= header.cbSrcLengthUsed;
        if (header.cbSrcLengthUsed && input_size_)
            std::memmove(input_.data(), input_.data() + header.cbSrcLengthUsed, input_size_);
        if (!header.cbSrcLengthUsed && !header.cbDstLengthUsed) {
            if (ended_ && input_size_) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            need_ = !ended_;
            drained_ = ended_;
        } else if (!ended_) need_ = input_size_ < suggested_input_;
        // 004C9FD9 requests the next normal packet after conversion. A reader
        // packet larger than the original fixed allocation is split first;
        // never request more input while a whole negotiated block is pending.
        // EOF drains the last incomplete block without requesting a packet.
        return buffer->SetLength(header.cbDstLengthUsed);
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        if (!stream_) return E_UNEXPECTED;
        const auto result = Result(acmStreamReset(stream_, 0));
        if (SUCCEEDED(result)) {
            input_size_ = 0;
            need_ = true;
            ended_ = drained_ = false;
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE EndInput() override {
        if (!stream_) return E_UNEXPECTED;
        ended_ = true;
        need_ = false;
        return S_OK;
    }
};
} // namespace

bool SupportsBuiltinAcmDecoder(const WAVEFORMATEX& format) noexcept {
    // The original 004C9C1F explicitly excludes Voxware (0x75).
    return format.wFormatTag && format.wFormatTag != 0x75 &&
        format.nChannels && format.nChannels <= 32 &&
        format.nSamplesPerSec >= 1000 && format.nSamplesPerSec <= 768000;
}
HRESULT CreateBuiltinAcmDecoder(void** decoder, std::filesystem::path& module_path) noexcept {
    if (!decoder) return E_POINTER;
    *decoder = nullptr;
    return Guard([&]() -> HRESULT {
        module_path = L"msacm32.dll";
        *decoder = static_cast<Decoder*>(new Acm);
        return S_OK;
    });
}
HRESULT FinishBuiltinAcmDecoder(void* decoder) noexcept {
    if (!decoder) return E_POINTER;
    Finish* finish{};
    const auto result = static_cast<IUnknown*>(decoder)->QueryInterface(
        finish_id, reinterpret_cast<void**>(&finish));
    if (FAILED(result)) return result == E_NOINTERFACE ? S_FALSE : result;
    if (!finish) return E_UNEXPECTED;
    const auto ended = finish->EndInput();
    finish->Release();
    return ended;
}
} // namespace ttplayer::plugins::detail
