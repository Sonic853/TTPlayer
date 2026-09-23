#include "ttplayer/audio/directshow_source.h"
#include "ttplayer/audio/archive_member.h"

#include <dshow.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>

namespace ttplayer::audio {
namespace {
using Microsoft::WRL::ComPtr;

void ClearType(AM_MEDIA_TYPE& type) {
    if (type.pbFormat) CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    type = {};
}
HRESULT CopyType(AM_MEDIA_TYPE& to, const AM_MEDIA_TYPE& from) {
    to = from;
    to.pbFormat = nullptr;
    to.pUnk = nullptr;
    if (from.cbFormat) {
        if (!from.pbFormat || from.cbFormat > 65536) { to = {}; return E_INVALIDARG; }
        to.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(from.cbFormat));
        if (!to.pbFormat) { to = {}; return E_OUTOFMEMORY; }
        std::memcpy(to.pbFormat, from.pbFormat, from.cbFormat);
    }
    to.pUnk = from.pUnk;
    if (to.pUnk) to.pUnk->AddRef();
    return S_OK;
}
bool WaveType(const AM_MEDIA_TYPE& type, WAVEFORMATEXTENSIBLE& wave) {
    if (type.majortype != MEDIATYPE_Audio || type.formattype != FORMAT_WaveFormatEx ||
        !type.pbFormat || type.cbFormat < 16 || type.cbFormat > 65536) return false;
    wave = {};
    std::memcpy(&wave, type.pbFormat, std::min<size_t>(sizeof(wave), type.cbFormat));
    return type.cbFormat >= 16U + (type.cbFormat >= 18 ? 2U + wave.Format.cbSize : 0U);
}
WORD EffectiveTag(const WAVEFORMATEXTENSIBLE& wave) {
    if (wave.Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE) return wave.Format.wFormatTag;
    if (wave.Format.cbSize < 22) return 0;
    if (wave.SubFormat == MEDIASUBTYPE_PCM) return WAVE_FORMAT_PCM;
    if (wave.SubFormat == MEDIASUBTYPE_IEEE_FLOAT) return WAVE_FORMAT_IEEE_FLOAT;
    return 0;
}
bool PcmType(const AM_MEDIA_TYPE& type, WAVEFORMATEXTENSIBLE& wave) {
    if (!WaveType(type, wave)) return false;
    const WORD tag = EffectiveTag(wave);
    const auto& f = wave.Format;
    if ((tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_IEEE_FLOAT) ||
        !f.nChannels || f.nChannels > 64 || !f.nSamplesPerSec || f.nSamplesPerSec > 768000 ||
        !f.wBitsPerSample || f.wBitsPerSample % 8 || f.wBitsPerSample > 64 ||
        (tag == WAVE_FORMAT_PCM && f.wBitsPerSample > 32) ||
        (tag == WAVE_FORMAT_IEEE_FLOAT && f.wBitsPerSample != 32 && f.wBitsPerSample != 64)) return false;
    if (f.nBlockAlign != f.nChannels * (f.wBitsPerSample / 8) ||
        f.nAvgBytesPerSec != static_cast<ULONGLONG>(f.nSamplesPerSec) * f.nBlockAlign) return false;
    // OutputFormat exposes this fixed-size object, never an arbitrary tail.
    wave.Format.cbSize = f.wFormatTag == WAVE_FORMAT_EXTENSIBLE ? 22 : 0;
    return true;
}

class PinEnumerator final : public IEnumPins {
public:
    explicit PinEnumerator(IPin* pin, ULONG position = 0) : pin_(pin), position_(position) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IEnumPins) return E_NOINTERFACE;
        *out = static_cast<IEnumPins*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG value = InterlockedDecrement(&refs_); if (!value) delete this; return value;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, IPin** pins, ULONG* fetched) override {
        if (!pins || (!fetched && count != 1)) return E_POINTER;
        ULONG got{};
        if (count && position_ == 0) { pins[0] = pin_.Get(); pins[0]->AddRef(); ++position_; ++got; }
        if (fetched) *fetched = got;
        return count == got ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const bool enough = count <= 1 - position_;
        position_ = std::min<ULONG>(1, position_ + std::min<ULONG>(count, 1));
        return enough ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { position_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumPins** out) override {
        if (!out) return E_POINTER;
        *out = new(std::nothrow) PinEnumerator(pin_.Get(), position_);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
private:
    LONG refs_{1}; ComPtr<IPin> pin_; ULONG position_{};
};

class TypeEnumerator final : public IEnumMediaTypes {
public:
    explicit TypeEnumerator(ULONG position = 0) : position_(position) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IEnumMediaTypes) return E_NOINTERFACE;
        *out = static_cast<IEnumMediaTypes*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG value = InterlockedDecrement(&refs_); if (!value) delete this; return value;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override {
        if (!types || (!fetched && count != 1)) return E_POINTER;
        ULONG got{};
        while (got < count && position_ < 2) {
            auto* type = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
            if (!type) { if (fetched) *fetched = got; return E_OUTOFMEMORY; }
            *type = {};
            type->majortype = MEDIATYPE_Audio;
            type->subtype = position_ == 0 ? MEDIASUBTYPE_PCM : MEDIASUBTYPE_IEEE_FLOAT;
            type->formattype = FORMAT_WaveFormatEx;
            type->bFixedSizeSamples = TRUE;
            types[got++] = type;
            ++position_;
        }
        if (fetched) *fetched = got;
        return got == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const bool enough = count <= 2 - position_;
        position_ = std::min<ULONG>(2, position_ + std::min<ULONG>(count, 2));
        return enough ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { position_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumMediaTypes** out) override {
        if (!out) return E_POINTER;
        *out = new(std::nothrow) TypeEnumerator(position_);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
private:
    LONG refs_{1}; ULONG position_{};
};

// A single PCM input pin. DirectShow does demux/decode only: the bounded queue
// feeds DecodedAudioSource, so waveOut/DS/ASIO, fades and DSP remain host-owned.
class PcmSink final : public IBaseFilter, public IPin, public IMemInputPin,
                      public IMediaSeeking, public IAMFilterMiscFlags {
public:
    ~PcmSink() { ClearType(type_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IPersist || iid == IID_IMediaFilter || iid == IID_IBaseFilter)
            *out = static_cast<IBaseFilter*>(this);
        else if (iid == IID_IPin) *out = static_cast<IPin*>(this);
        else if (iid == IID_IMemInputPin) *out = static_cast<IMemInputPin*>(this);
        else if (iid == IID_IMediaSeeking) *out = static_cast<IMediaSeeking*>(this);
        else if (iid == IID_IAMFilterMiscFlags) *out = static_cast<IAMFilterMiscFlags*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG value = InterlockedDecrement(&refs_); if (!value) delete this; return value;
    }
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID* id) override {
        if (!id) return E_POINTER; *id = CLSID_NULL; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        std::scoped_lock lock(mutex_); state_ = State_Stopped; ClearQueue(); changed_.notify_all(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Pause() override {
        std::scoped_lock lock(mutex_); state_ = State_Paused; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME) override {
        std::scoped_lock lock(mutex_); state_ = State_Running; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetState(DWORD, FILTER_STATE* state) override {
        if (!state) return E_POINTER; std::scoped_lock lock(mutex_); *state = state_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* clock) override {
        clock_ = clock; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** clock) override {
        if (!clock) return E_POINTER; return clock_.CopyTo(clock);
    }
    HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** out) override {
        if (!out) return E_POINTER; *out = new(std::nothrow) PinEnumerator(this);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR id, IPin** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (!id || wcscmp(id, L"Input") != 0) return VFW_E_NOT_FOUND;
        *out = static_cast<IPin*>(this); AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* info) override {
        if (!info) return E_POINTER; *info = {};
        wcscpy_s(info->achName, L"TTPlayer PCM Reader");
        info->pGraph = graph_; if (graph_) graph_->AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* graph, LPCWSTR) override {
        graph_ = graph; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR* out) override {
        if (out) *out = nullptr; return E_NOTIMPL;
    }
    ULONG STDMETHODCALLTYPE GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_RENDERER; }

    HRESULT STDMETHODCALLTYPE Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin* other, const AM_MEDIA_TYPE* type) override {
        if (!other || !type) return E_POINTER;
        if (peer_) return VFW_E_ALREADY_CONNECTED;
        PIN_DIRECTION direction{};
        if (FAILED(other->QueryDirection(&direction)) || direction != PINDIR_OUTPUT)
            return VFW_E_INVALID_DIRECTION;
        WAVEFORMATEXTENSIBLE wave{};
        if (!PcmType(*type, wave)) return VFW_E_TYPE_NOT_ACCEPTED;
        const HRESULT copied = CopyType(type_, *type);
        if (FAILED(copied)) return copied;
        wave_ = wave;
        // One second, capped at 4 MiB; a single unusually large sample is
        // allowed only under the separate 16 MiB receive limit.
        limit_ = std::clamp<size_t>(wave.Format.nAvgBytesPerSec, 65536, 4 * 1024 * 1024);
        peer_ = other; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Disconnect() override { peer_.Reset(); allocator_.Reset(); ClearType(type_); return S_OK; }
    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        return peer_ ? peer_.CopyTo(out) : VFW_E_NOT_CONNECTED;
    }
    HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* out) override {
        if (!out) return E_POINTER; *out = {};
        return peer_ ? CopyType(*out, type_) : VFW_E_NOT_CONNECTED;
    }
    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* info) override {
        if (!info) return E_POINTER; *info = {};
        info->pFilter = this; AddRef(); info->dir = PINDIR_INPUT;
        wcscpy_s(info->achName, L"Input"); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* out) override {
        if (!out) return E_POINTER; *out = PINDIR_INPUT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* out) override {
        if (!out) return E_POINTER;
        *out = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"Input")));
        if (!*out) return E_OUTOFMEMORY;
        std::memcpy(*out, L"Input", sizeof(L"Input")); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE* type) override {
        WAVEFORMATEXTENSIBLE wave{};
        return type && PcmType(*type, wave) ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes** out) override {
        if (!out) return E_POINTER; *out = new(std::nothrow) TypeEnumerator;
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndOfStream() override {
        std::scoped_lock lock(mutex_); ended_ = true; changed_.notify_all(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginFlush() override {
        std::scoped_lock lock(mutex_); flushing_ = true; ClearQueue(); changed_.notify_all(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndFlush() override {
        std::scoped_lock lock(mutex_); ClearQueue(); flushing_ = false; changed_.notify_all(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (!allocator_) {
            const HRESULT result = CoCreateInstance(CLSID_MemoryAllocator, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&allocator_));
            if (FAILED(result)) return result;
        }
        return allocator_.CopyTo(out);
    }
    HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator* allocator, BOOL) override {
        if (!allocator) return E_POINTER; allocator_ = allocator; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES* out) override {
        if (!out) return E_POINTER; *out = {}; return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Receive(IMediaSample* sample) override {
        if (!sample) return E_POINTER;
        if (sample->IsPreroll() == S_OK) return S_OK;
        AM_MEDIA_TYPE* changed{};
        if (sample->GetMediaType(&changed) == S_OK && changed) {
            WAVEFORMATEXTENSIBLE wave{};
            const bool same = PcmType(*changed, wave) &&
                std::memcmp(&wave, &wave_, sizeof(wave)) == 0;
            ClearType(*changed); CoTaskMemFree(changed);
            if (!same) return SetFailure(VFW_E_INVALIDMEDIATYPE);
        }
        BYTE* data{};
        const long count = sample->GetActualDataLength();
        if (count < 0 || count > sample->GetSize() || count > 16 * 1024 * 1024 ||
            !wave_.Format.nBlockAlign || count % wave_.Format.nBlockAlign ||
            FAILED(sample->GetPointer(&data)) || (count && !data)) return SetFailure(E_UNEXPECTED);
        if (!count) return S_OK;
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return closing_ || flushing_ || state_ == State_Stopped ||
            FAILED(failure_) || queued_ == 0 || queued_ + static_cast<size_t>(count) <= limit_; });
        if (closing_ || flushing_) return S_FALSE;
        if (state_ == State_Stopped) return VFW_E_WRONG_STATE;
        if (FAILED(failure_)) return failure_;
        try {
            queue_.emplace_back(reinterpret_cast<std::byte*>(data), reinterpret_cast<std::byte*>(data) + count);
        } catch (const std::bad_alloc&) { failure_ = E_OUTOFMEMORY; changed_.notify_all(); return failure_; }
        queued_ += count;
        changed_.notify_all(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample** samples, long count, long* processed) override {
        if (!samples || !processed || count < 0) return E_POINTER;
        *processed = 0;
        while (*processed < count) {
            const HRESULT result = Receive(samples[*processed]);
            if (result != S_OK) return result;
            ++*processed;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override { return S_OK; }

    // CPosPassThru-style seeking: the graph reaches the parser through its
    // renderer's input connection, without replacing the player's clock.
    template<class Method, class... Args> HRESULT Forward(Method method, Args... args) {
        ComPtr<IMediaSeeking> seeking;
        const HRESULT result = peer_ ? peer_.As(&seeking) : VFW_E_NOT_CONNECTED;
        return FAILED(result) ? result : (seeking.Get()->*method)(args...);
    }
    HRESULT STDMETHODCALLTYPE GetCapabilities(DWORD* p) override { return Forward(&IMediaSeeking::GetCapabilities,p); }
    HRESULT STDMETHODCALLTYPE CheckCapabilities(DWORD* p) override { return Forward(&IMediaSeeking::CheckCapabilities,p); }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(const GUID* p) override { return Forward(&IMediaSeeking::IsFormatSupported,p); }
    HRESULT STDMETHODCALLTYPE QueryPreferredFormat(GUID* p) override { return Forward(&IMediaSeeking::QueryPreferredFormat,p); }
    HRESULT STDMETHODCALLTYPE GetTimeFormat(GUID* p) override { return Forward(&IMediaSeeking::GetTimeFormat,p); }
    HRESULT STDMETHODCALLTYPE IsUsingTimeFormat(const GUID* p) override { return Forward(&IMediaSeeking::IsUsingTimeFormat,p); }
    HRESULT STDMETHODCALLTYPE SetTimeFormat(const GUID* p) override { return Forward(&IMediaSeeking::SetTimeFormat,p); }
    HRESULT STDMETHODCALLTYPE GetDuration(LONGLONG* p) override { return Forward(&IMediaSeeking::GetDuration,p); }
    HRESULT STDMETHODCALLTYPE GetStopPosition(LONGLONG* p) override { return Forward(&IMediaSeeking::GetStopPosition,p); }
    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LONGLONG* p) override { return Forward(&IMediaSeeking::GetCurrentPosition,p); }
    HRESULT STDMETHODCALLTYPE ConvertTimeFormat(LONGLONG* a,const GUID* b,LONGLONG c,const GUID* d) override { return Forward(&IMediaSeeking::ConvertTimeFormat,a,b,c,d); }
    HRESULT STDMETHODCALLTYPE SetPositions(LONGLONG* a,DWORD b,LONGLONG* c,DWORD d) override { return Forward(&IMediaSeeking::SetPositions,a,b,c,d); }
    HRESULT STDMETHODCALLTYPE GetPositions(LONGLONG* a,LONGLONG* b) override { return Forward(&IMediaSeeking::GetPositions,a,b); }
    HRESULT STDMETHODCALLTYPE GetAvailable(LONGLONG* a,LONGLONG* b) override { return Forward(&IMediaSeeking::GetAvailable,a,b); }
    HRESULT STDMETHODCALLTYPE SetRate(double rate) override { return Forward(&IMediaSeeking::SetRate,rate); }
    HRESULT STDMETHODCALLTYPE GetRate(double* p) override { return Forward(&IMediaSeeking::GetRate,p); }
    HRESULT STDMETHODCALLTYPE GetPreroll(LONGLONG* p) override { return Forward(&IMediaSeeking::GetPreroll,p); }

    void Close() { std::scoped_lock lock(mutex_); closing_ = true; changed_.notify_all(); }
    const WAVEFORMATEXTENSIBLE& Format() const { return wave_; }
    HRESULT Pull(size_t requested, std::vector<std::byte>& output, bool& end) {
        output.clear(); end = false;
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::milliseconds(500), [&] {
                return !queue_.empty() || ended_ || closing_ || FAILED(failure_); })) return S_FALSE;
        if (FAILED(failure_)) return failure_;
        if (closing_) return E_ABORT;
        while (!queue_.empty() && output.size() < requested) {
            const auto& first = queue_.front();
            const size_t bytes = std::min(requested - output.size(), first.size() - offset_);
            output.insert(output.end(), first.begin() + offset_, first.begin() + offset_ + bytes);
            offset_ += bytes; queued_ -= bytes;
            if (offset_ == first.size()) { queue_.pop_front(); offset_ = 0; }
        }
        end = ended_ && queue_.empty();
        changed_.notify_all(); return S_OK;
    }
private:
    HRESULT SetFailure(HRESULT result) {
        std::scoped_lock lock(mutex_); failure_ = result; changed_.notify_all(); return result;
    }
    void ClearQueue() { queue_.clear(); queued_ = offset_ = 0; ended_ = false; failure_ = S_OK; }
    LONG refs_{1}; IFilterGraph* graph_{};
    ComPtr<IPin> peer_; ComPtr<IReferenceClock> clock_; ComPtr<IMemAllocator> allocator_;
    AM_MEDIA_TYPE type_{}; WAVEFORMATEXTENSIBLE wave_{};
    std::mutex mutex_; std::condition_variable changed_;
    FILTER_STATE state_{State_Stopped};
    bool ended_{}, flushing_{}, closing_{};
    HRESULT failure_{S_OK};
    std::deque<std::vector<std::byte>> queue_;
    size_t queued_{}, offset_{}, limit_{65536};
};

bool EncodedFormat(IPin* output, WAVEFORMATEXTENSIBLE& format, unsigned depth = 0) {
    if (!output || depth > 16) return false;
    AM_MEDIA_TYPE type{};
    WAVEFORMATEXTENSIBLE found{};
    const HRESULT result = output->ConnectionMediaType(&type);
    const bool encoded = SUCCEEDED(result) && WaveType(type, found) &&
        found.Format.wFormatTag != WAVE_FORMAT_PCM && found.Format.wFormatTag != WAVE_FORMAT_IEEE_FLOAT &&
        found.Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE;
    ClearType(type);
    if (encoded) { format = found; return true; }
    PIN_INFO info{};
    if (FAILED(output->QueryPinInfo(&info)) || !info.pFilter) return false;
    ComPtr<IBaseFilter> filter; filter.Attach(info.pFilter);
    ComPtr<IEnumPins> pins;
    if (FAILED(filter->EnumPins(&pins))) return false;
    ComPtr<IPin> pin;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION direction{};
        ComPtr<IPin> upstream;
        if (SUCCEEDED(pin->QueryDirection(&direction)) && direction == PINDIR_INPUT &&
            SUCCEEDED(pin->ConnectedTo(&upstream)) && EncodedFormat(upstream.Get(), format, depth + 1)) return true;
        pin.Reset();
    }
    return false;
}

std::wstring Codec(WORD tag) {
    switch (tag) {
    case WAVE_FORMAT_PCM: return L"PCM";
    case WAVE_FORMAT_IEEE_FLOAT: return L"IEEE Float";
    case WAVE_FORMAT_MPEG: return L"MPEG-1 Audio";
    case WAVE_FORMAT_MPEGLAYER3: return L"MPEG Layer-3";
    case 0x0160: case 0x0161: case 0x0162: case 0x0163: return L"Windows Media Audio";
    case 0x00ff: case 0x1610: return L"AAC";
    case 0x2000: return L"AC-3";
    case 0x2001: return L"DTS";
    case 0xf1ac: return L"FLAC";
    default: return L"DirectShow Audio";
    }
}

class DirectShowSource final : public DecodedAudioSource {
public:
    ~DirectShowSource() override { Close(); }
    bool Open(const std::filesystem::path& path, const PlaybackOptions&) override {
        Close(); error_.clear(); error_result_ = S_OK; duration_ = {}; empty_reads_ = 0;
        ArchiveMemberPath member;
        if (ParseArchiveMemberPath(path.native(), member)) return Fail(L"DirectShow archive stream", E_NOTIMPL);
        HRESULT result = CoCreateInstance(CLSID_FilterGraph, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph_));
        if (FAILED(result)) return Fail(L"DirectShow graph", result);
        result = graph_->AddSourceFilter(path.c_str(), L"Source Filter", &source_);
        if (FAILED(result)) return Fail(L"DirectShow source", result);
        sink_ = new(std::nothrow) PcmSink;
        if (!sink_) return Fail(L"DirectShow PCM sink", E_OUTOFMEMORY);
        result = graph_->AddFilter(sink_, L"Audio Renderer");
        if (FAILED(result)) return Fail(L"DirectShow AddFilter", result);
        ComPtr<IEnumPins> pins;
        result = source_->EnumPins(&pins);
        if (FAILED(result)) return Fail(L"DirectShow source pins", result);
        ComPtr<IPin> pin;
        HRESULT connected = VFW_E_CANNOT_CONNECT;
        while (pins->Next(1, &pin, nullptr) == S_OK) {
            PIN_DIRECTION direction{};
            if (SUCCEEDED(pin->QueryDirection(&direction)) && direction == PINDIR_OUTPUT) {
                connected = graph_->Connect(pin.Get(), sink_);
                if (SUCCEEDED(connected)) break;
            }
            pin.Reset();
        }
        if (FAILED(connected)) return Fail(L"DirectShow audio connection", connected);
        wave_ = sink_->Format();
        const auto& f = wave_.Format;
        WAVEFORMATEXTENSIBLE native = wave_;
        ComPtr<IPin> upstream;
        if (SUCCEEDED(sink_->ConnectedTo(&upstream))) EncodedFormat(upstream.Get(), native);
        const auto& n = native.Format;
        display_ = {n.wFormatTag, f.nChannels, f.nSamplesPerSec, n.nAvgBytesPerSec,
                    f.nBlockAlign, f.wBitsPerSample, Codec(n.wFormatTag)};
        result = graph_.As(&control_);
        if (FAILED(result)) return Fail(L"DirectShow control", result);
        graph_.As(&seeking_); graph_.As(&events_);
        ComPtr<IMediaFilter> filter;
        if (SUCCEEDED(graph_.As(&filter))) filter->SetSyncSource(nullptr);
        LONGLONG length{};
        if (seeking_ && SUCCEEDED(seeking_->GetDuration(&length)))
            duration_ = std::chrono::milliseconds(std::max<LONGLONG>(0, length / 10000));
        return true;
    }
    bool Read(size_t requested, std::vector<std::byte>& output, bool& end) override {
        output.clear(); end = false;
        if (!sink_ || !wave_.Format.nBlockAlign) return Fail(L"DirectShow reader", E_UNEXPECTED);
        if (!running_) {
            const HRESULT result = control_->Run();
            if (FAILED(result)) return Fail(L"DirectShow Run", result);
            running_ = true;
        }
        if (!CheckEvents()) return false;
        requested = std::max<size_t>(requested - requested % wave_.Format.nBlockAlign, wave_.Format.nBlockAlign);
        const HRESULT result = sink_->Pull(requested, output, end);
        if (FAILED(result)) return Fail(L"DirectShow PCM read", result);
        if (!CheckEvents()) return false;
        if (result == S_FALSE && ++empty_reads_ >= 20) return Fail(L"DirectShow decoder timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        if (!output.empty() || end) empty_reads_ = 0;
        return true;
    }
    bool Seek(std::chrono::milliseconds position) override {
        if (!seeking_) return Fail(L"DirectShow seek", E_NOINTERFACE);
        if (position.count() < 0 || position.count() > std::numeric_limits<LONGLONG>::max() / 10000)
            return Fail(L"DirectShow seek", E_INVALIDARG);
        LONGLONG target = position.count() * 10000;
        // Release a producer blocked on our bounded queue before seeking.
        sink_->BeginFlush();
        const HRESULT stopped = control_->Stop();
        running_ = false;
        if (FAILED(stopped)) { sink_->EndFlush(); return Fail(L"DirectShow Stop for seek", stopped); }
        const HRESULT result = seeking_->SetPositions(&target, AM_SEEKING_AbsolutePositioning,
                                                       nullptr, AM_SEEKING_NoPositioning);
        sink_->EndFlush(); empty_reads_ = 0;
        return SUCCEEDED(result) || Fail(L"DirectShow SetPositions", result);
    }
    const WAVEFORMATEX& OutputFormat() const override { return wave_.Format; }
    AudioFormat DisplayFormat() const override { return display_; }
    std::chrono::milliseconds Duration() const override { return duration_; }
    std::wstring Error() const override { return error_; }
    HRESULT ErrorResult() const override { return error_result_; }
private:
    bool CheckEvents() {
        if (!events_) return true;
        long code{}; LONG_PTR first{}, second{};
        while (events_->GetEvent(&code, &first, &second, 0) == S_OK) {
            events_->FreeEventParams(code, first, second);
            if (code == EC_ERRORABORT) return Fail(L"DirectShow decoder", FAILED(static_cast<HRESULT>(first))
                ? static_cast<HRESULT>(first) : E_FAIL);
        }
        return true;
    }
    bool Fail(const wchar_t* operation, HRESULT result) {
        error_result_ = result;
        wchar_t hex[16]{}; swprintf_s(hex, L"%08X", static_cast<unsigned>(result));
        error_ = std::wstring(operation) + L" failed (0x" + hex + L")";
        return false;
    }
    void Close() {
        if (sink_) sink_->Close();
        if (control_) control_->Stop();
        if (sink_ && graph_) {
            ComPtr<IPin> upstream;
            if (SUCCEEDED(sink_->ConnectedTo(&upstream))) graph_->Disconnect(upstream.Get());
            graph_->Disconnect(sink_);
        }
        events_.Reset(); seeking_.Reset(); control_.Reset(); source_.Reset(); graph_.Reset();
        if (sink_) sink_->Release(); sink_ = nullptr; running_ = false;
    }
    PcmSink* sink_{};
    ComPtr<IGraphBuilder> graph_; ComPtr<IBaseFilter> source_;
    ComPtr<IMediaControl> control_; ComPtr<IMediaSeeking> seeking_; ComPtr<IMediaEvent> events_;
    WAVEFORMATEXTENSIBLE wave_{}; AudioFormat display_{};
    std::chrono::milliseconds duration_{};
    bool running_{}; unsigned empty_reads_{};
    HRESULT error_result_{E_FAIL}; std::wstring error_;
};
} // namespace

std::unique_ptr<DecodedAudioSource> CreateDirectShowSource() { return std::make_unique<DirectShowSource>(); }
} // namespace ttplayer::audio
