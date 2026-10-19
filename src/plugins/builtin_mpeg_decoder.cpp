#include "builtin_mpeg_decoder.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <mmreg.h>
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
    virtual HRESULT STDMETHODCALLTYPE Capacity(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Data(BYTE **, DWORD *) = 0;
};
struct Decoder : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(const WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Sizes(DWORD *, DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE NeedsInput() = 0;
    virtual HRESULT STDMETHODCALLTYPE Available() = 0;
    virtual HRESULT STDMETHODCALLTYPE Input(Buffer *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Output(Buffer *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
};
struct Finish : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EndInput() = 0;
};
// TTPCOMM ordinals 11..14; identical to http_mpeg_source and original 004C9965.
struct Io {
    const BYTE *input;
    unsigned bytes, consumed;
    double *output;
    unsigned capacity, produced;
};
static_assert(sizeof(Io) == 24);
constexpr size_t maximum = 64U * 1024U * 1024U;
template <class F> HRESULT guard(F &&action) noexcept {
    try {
        return action();
    } catch (const std::bad_alloc &) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
DWORD tag(const WAVEFORMATEX &f) noexcept {
    if (f.wFormatTag != WAVE_FORMAT_EXTENSIBLE)
        return f.wFormatTag;
    if (f.cbSize < 22)
        return 0;
    const auto &id = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(f).SubFormat;
    const GUID canonical{id.Data1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    return InlineIsEqualGUID(id, canonical) ? id.Data1 : 0;
}
class Mpeg final : public Decoder, public Finish {
    LONG refs_{1};
    HMODULE module_{};
    void *state_{};
    unsigned(__cdecl *bits_)(){};
    void *(__cdecl *create_)(unsigned){};
    void(__cdecl *free_)(void *){};
    int(__cdecl *process_)(void *, Io *){};
    void(__cdecl *reset_)(void *){};
    std::vector<BYTE> input_;
    size_t offset_{};
    WAVEFORMATEX format_{};
    DWORD suggested_{};
    bool need_{true}, ended_{}, drained_{};
    ~Mpeg() {
        if (state_)
            free_(state_);
        if (module_)
            FreeLibrary(module_);
    }

  public:
    explicit Mpeg(HMODULE module) : module_(module) {
        bits_ = reinterpret_cast<decltype(bits_)>(GetProcAddress(module, MAKEINTRESOURCEA(10)));
        create_ = reinterpret_cast<decltype(create_)>(GetProcAddress(module, MAKEINTRESOURCEA(11)));
        free_ = reinterpret_cast<decltype(free_)>(GetProcAddress(module, MAKEINTRESOURCEA(12)));
        process_ =
            reinterpret_cast<decltype(process_)>(GetProcAddress(module, MAKEINTRESOURCEA(13)));
        reset_ = reinterpret_cast<decltype(reset_)>(GetProcAddress(module, MAKEINTRESOURCEA(14)));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (InlineIsEqualGUID(id, IID_IUnknown) || InlineIsEqualGUID(id, decoder_id))
            *out = static_cast<Decoder *>(this);
        else if (InlineIsEqualGUID(id, finish_id))
            *out = static_cast<Finish *>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = InterlockedDecrement(&refs_);
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE Initialize(const WAVEFORMATEX *from, WAVEFORMATEX *to) override {
        if (!from || !to)
            return E_POINTER;
        if (state_)
            return E_UNEXPECTED;
        if (!SupportsBuiltinMpegDecoder(*from))
            return E_INVALIDARG;
        if (!bits_ || !create_ || !free_ || !process_ || !reset_ || bits_() != 64)
            return E_NOINTERFACE;
        state_ = create_(tag(*from) == 0x55756461);
        if (!state_)
            return E_OUTOFMEMORY;
        const WORD align = WORD(from->nChannels * sizeof(double));
        if (!from->nSamplesPerSec || from->nSamplesPerSec > 384000)
            return E_INVALIDARG;
        format_ = {WAVE_FORMAT_IEEE_FLOAT,
                   from->nChannels,
                   from->nSamplesPerSec,
                   from->nSamplesPerSec * align,
                   align,
                   64,
                   0};
        *to = format_;
        suggested_ = std::clamp<DWORD>(from->nAvgBytesPerSec >> 4, 4096, 16 * 1024 * 1024);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Sizes(DWORD *in, DWORD *out) override {
        if (!state_)
            return E_UNEXPECTED;
        if (in)
            *in = suggested_;
        if (out)
            *out = format_.nBlockAlign * 1152;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE NeedsInput() override {
        return !state_ ? E_UNEXPECTED : !ended_ && need_ ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Available() override {
        return !state_ ? E_UNEXPECTED : !drained_ && !need_ ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Input(Buffer *buffer) override {
        return guard([&]() -> HRESULT {
            if (!state_ || ended_)
                return E_UNEXPECTED;
            if (!buffer)
                return E_POINTER;
            BYTE *p{};
            DWORD n{}, capacity{};
            auto hr = buffer->Data(&p, &n);
            if (FAILED(hr))
                return hr;
            hr = buffer->Capacity(&capacity);
            if (FAILED(hr))
                return hr;
            if (n > capacity || (!p && n))
                return E_INVALIDARG;
            const size_t remaining = input_.size() - offset_;
            if (n > maximum - remaining)
                return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            if (offset_) {
                input_.erase(input_.begin(), input_.begin() + offset_);
                offset_ = 0;
            }
            if (n)
                input_.insert(input_.end(), p, p + n);
            need_ = false;
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE Output(Buffer *buffer) override {
        return guard([&]() -> HRESULT {
            if (!state_)
                return E_UNEXPECTED;
            if (!buffer)
                return E_POINTER;
            auto hr = buffer->SetLength(0);
            if (FAILED(hr))
                return hr;
            BYTE *p{};
            DWORD unused{}, capacity{};
            hr = buffer->Capacity(&capacity);
            if (FAILED(hr))
                return hr;
            hr = buffer->Data(&p, &unused);
            if (FAILED(hr))
                return hr;
            if (!p || capacity < DWORD(format_.nBlockAlign) * 1152U)
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            if (drained_ || need_)
                return S_OK;
            const size_t remaining = input_.size() - offset_;
            if (!remaining) {
                need_ = !ended_;
                drained_ = ended_;
                return S_OK;
            }
            // The original core requires eight bytes of look-ahead. Only append
            // padding after explicit EOF; an ASF packet boundary is not EOF.
            const size_t real_size = input_.size();
            if (ended_)
                input_.resize(real_size + 8, 0);
            std::array<double, 2304> samples{};
            Io io{input_.data() + offset_,
                  unsigned(remaining + (ended_ ? 8 : 0)),
                  0,
                  samples.data(),
                  std::min<unsigned>(capacity, unsigned(sizeof(samples))),
                  0};
            const int result = process_(state_, &io);
            if (ended_)
                input_.resize(real_size);
            if (io.consumed > remaining + (ended_ ? 8 : 0) || io.produced > sizeof(samples) ||
                io.produced % format_.nBlockAlign)
                return E_UNEXPECTED;
            offset_ += std::min<size_t>(io.consumed, remaining);
            if (io.produced)
                memcpy(p, samples.data(), io.produced);
            if (!io.produced) {
                // Recoverable MPEG reservoir underflow after seek may consume a
                // frame without output. Keep draining while bytes are consumed.
                if (!io.consumed) {
                    need_ = !ended_;
                    drained_ = ended_;
                } else if (offset_ == input_.size()) {
                    need_ = !ended_;
                    drained_ = ended_;
                }
                if (result < 0 && !io.consumed)
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            return buffer->SetLength(io.produced);
        });
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        if (!state_)
            return E_UNEXPECTED;
        reset_(state_);
        input_.clear();
        offset_ = 0;
        need_ = true;
        ended_ = drained_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndInput() override {
        if (!state_)
            return E_UNEXPECTED;
        ended_ = true;
        need_ = false;
        return S_OK;
    }
};
} // namespace
bool SupportsBuiltinMpegDecoder(const WAVEFORMATEX &f) noexcept {
    const auto value = tag(f);
    return f.nChannels >= 1 && f.nChannels <= 2 &&
           (value == WAVE_FORMAT_MPEGLAYER3 || value == WAVE_FORMAT_MPEG || value == 0x55756461);
}
HRESULT CreateBuiltinMpegDecoder(void **out, std::filesystem::path &path) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    return guard([&]() -> HRESULT {
        HMODULE module{};
        if (!GetModuleHandleExW(0, L"ttpcomm.dll", &module)) {
            std::array<wchar_t, 32768> exe{};
            auto n = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
            if (!n || n >= exe.size())
                return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
            path = std::filesystem::path(exe.data()).parent_path() / L"ttpcomm.dll";
            module = LoadLibraryW(path.c_str());
        }
        if (!module)
            return HRESULT_FROM_WIN32(GetLastError());
        std::array<wchar_t, 32768> name{};
        GetModuleFileNameW(module, name.data(), DWORD(name.size()));
        try {
            path = name.data();
            *out = static_cast<Decoder *>(new Mpeg(module));
        } catch (...) {
            FreeLibrary(module);
            throw;
        }
        return S_OK;
    });
}
HRESULT FinishBuiltinMpegDecoder(void *decoder) noexcept {
    if (!decoder)
        return E_POINTER;
    Finish *finish{};
    auto hr = static_cast<IUnknown *>(decoder)->QueryInterface(finish_id,
                                                               reinterpret_cast<void **>(&finish));
    if (hr == E_NOINTERFACE)
        return S_FALSE;
    if (FAILED(hr))
        return hr;
    if (!finish)
        return E_UNEXPECTED;
    hr = finish->EndInput();
    finish->Release();
    return hr;
}
} // namespace ttplayer::plugins::detail
