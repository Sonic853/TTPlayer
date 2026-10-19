#include "ttplayer/plugins/legacy_host_exports.h"

#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/audio/http_stream.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>

namespace ttplayer::plugins {
namespace {

constexpr HRESULT kInetOpenFailed = static_cast<HRESULT>(0x80040064u);
constexpr HRESULT kInetLengthUnavailable = static_cast<HRESULT>(0x80040065u);

class SoundBuffer final : public LegacyHostSoundBuffer {
public:
    SoundBuffer(BYTE* data, DWORD capacity) : data_(data), capacity_(capacity) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != kLegacySoundBufferInterface) return E_NOINTERFACE;
        *out = static_cast<LegacyHostSoundBuffer*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE SetLength(DWORD length) override {
        if (length > capacity_) return E_FAIL;
        length_ = length; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCapacity(DWORD* capacity) override {
        if (!capacity) return E_POINTER;
        *capacity = capacity_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetData(BYTE** data, DWORD* length) override {
        if (!data && !length) return E_POINTER;
        if (data) *data = data_;
        if (length) *length = length_;
        return S_OK;
    }
private:
    ~SoundBuffer() { std::free(data_); }
    std::atomic<ULONG> refs_{1};
    BYTE* data_{};
    DWORD length_{}, capacity_{};
};

struct CodecName { DWORD tag; const wchar_t* name; };
// Exact strings in TTPlayer.exe!0053C278 / 0053C350. The spelling is ABI data:
// AddIns return this short-name|description pair to the property window.
constexpr CodecName kWaveNames[]{
    {1,L"PCM|PCM Audio"}, {2,L"ADPCM|ADPCM Audio"}, {0x11,L"ADPCM|Intel ADPCM"},
    {3,L"IEEE|IEEE Audio"}, {0x40,L"G721|G721 ADPCM"}, {0x65,L"G722|G722 ADPCM"},
    {0x14,L"G723|G723 ADPCM"}, {0x64,L"G726|G726 ADPCM"}, {0x140,L"G726|G726 ADPCM"},
    {0x41,L"G728|G728 ADPCM"}, {0x83,L"G729|G729 ADPCM"}, {0x31,L"GSM|GSM610 Audio"},
    {0x50,L"MPA|MPEG-1 Audio"}, {0x55,L"MP3|MPEG-1 Layer 3 Audio"},
    {0xff,L"AAC|MPEG-4 AAC Audio"}, {0x2000,L"AC3|Dolby AC3 Audio"},
    {8,L"DTS|DTS Audio"}, {0x2001,L"DTS|DVD DTS Audio"}, {0x43414146,L"AAC|MPEG-4 AAC Audio"},
    {0x345f3431,L"14_4|14.4k RealAudio"}, {0x385f3832,L"28_8|28.8k RealAudio"},
    {0x63727461,L"ATRAC3|ATRAC3 RealAudio"}, {0x6b6f6f63,L"Cooker|Cooker-G2 RealAudio"},
    {0x74656e64,L"AC3|DolbyNet AC3 Audio"}, {0x72706973,L"ACELP|ACELP-NET RealAudio"},
    {0x63616172,L"AAC|AAC RealAudio"}, {0x55756461,L"MP3|MPEG-1 Layer 3 Audio"}
};
constexpr CodecName kMpegNames[]{
    {0xe06d802b,L"MPA|MPEG-2 Audio"}, {0xe06d802c,L"AC3|Dolby AC3 Audio"},
    {0xe06d8032,L"LPCM|DVD LPCM Audio"}, {0xe06d8033,L"DTS|DVD DTS Audio"}
};

HRESULT Duplicate(std::wstring_view text, LPWSTR* output) noexcept {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    auto* copy = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
    if (!copy) return E_OUTOFMEMORY;
    std::memcpy(copy, text.data(), text.size() * sizeof(wchar_t));
    copy[text.size()] = 0; *output = copy; return S_OK;
}

std::mutex profile_mutex;
audio::PlaybackOptions network_profile;

// Each callback stream owns its cancellation state. Never capture a borrowed
// open-call event in a producer that may continue after CreateStreamOnInet.
struct CancelState {
    std::mutex mutex;
    HANDLE persistent{};
    HANDLE operation{}; // borrowed only while the corresponding call is active
    DWORD started{}, timeout{};
    bool active{};
    ~CancelState() { if (persistent) CloseHandle(persistent); }
    bool Canceled() {
        std::lock_guard lock(mutex);
        const HANDLE event = persistent ? persistent : operation;
        return (event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0) ||
            (active && timeout && GetTickCount() - started >= timeout);
    }
    void Begin(HANDLE event, DWORD milliseconds) {
        std::lock_guard lock(mutex);
        operation = event; timeout = milliseconds ? milliseconds : 60000;
        started = GetTickCount(); active = true;
    }
    void End() {
        std::lock_guard lock(mutex);
        operation = nullptr; active = false;
    }
    HRESULT Set(HANDLE event) {
        HANDLE copy{};
        if (event && !DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
                                     &copy, SYNCHRONIZE, FALSE, 0))
            return HRESULT_FROM_WIN32(GetLastError());
        std::lock_guard lock(mutex);
        if (persistent) CloseHandle(persistent);
        persistent = copy; return S_OK;
    }
};
class Operation final {
public:
    Operation(CancelState& state, HANDLE event, DWORD timeout) : state_(state) { state_.Begin(event, timeout); }
    ~Operation() { state_.End(); }
private:
    CancelState& state_;
};

class InetStream final : public LegacyHostInetStream {
public:
    explicit InetStream(std::shared_ptr<CancelState> cancel) : cancel_(std::move(cancel)) {}
    ~InetStream() { if (stream_) stream_->Release(); }
    HRESULT Open(const wchar_t* url, audio::PlaybackOptions options, HANDLE cancel, DWORD timeout) {
        Operation operation(*cancel_, cancel, timeout);
        options.network_canceled = [state = cancel_] { return state->Canceled(); };
        options.network_timeout_ms = static_cast<int>(std::min<DWORD>(timeout ? timeout : 60000, 60000));
        const HRESULT result = audio::OpenHttpAudioStream(url, options, &stream_);
        if (FAILED(result)) return result == E_OUTOFMEMORY ? result : kInetOpenFailed;
        STATSTG stat{};
        if (FAILED(stream_->Stat(&stat, STATFLAG_NONAME)) || !stat.cbSize.QuadPart)
            return kInetLengthUnavailable;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IStream && iid != IID_ISequentialStream &&
            iid != kLegacyInetStreamInterface) return E_NOINTERFACE;
        *output = static_cast<LegacyHostInetStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) override {
        return ReadEx(data, size, read, nullptr, 60000);
    }
    HRESULT STDMETHODCALLTYPE ReadEx(void* data, ULONG size, ULONG* read,
                                      HANDLE cancel, DWORD timeout) override {
        if (read) *read = 0;
        if (!data) return E_INVALIDARG;
        if (!size) return S_FALSE;
        std::lock_guard lock(operation_mutex_);
        Operation operation(*cancel_, cancel, timeout);
        ULONG actual{};
        stream_->Read(data, size, &actual);
        if (read) *read = actual;
        // 004C45EB / 004C4641: any positive short read is S_OK; zero is E_FAIL.
        return actual ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE SetCancelEvent(HANDLE event) override { return cancel_->Set(event); }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER distance, DWORD origin, ULARGE_INTEGER* position) override {
        return SeekEx(distance, origin, position, nullptr, 60000);
    }
    HRESULT STDMETHODCALLTYPE SeekEx(LARGE_INTEGER distance, DWORD origin, ULARGE_INTEGER* position,
                                      HANDLE cancel, DWORD timeout) override {
        if (origin > STREAM_SEEK_END) return E_INVALIDARG;
        std::lock_guard lock(operation_mutex_);
        Operation operation(*cancel_, cancel, timeout);
        return stream_->Seek(distance, origin, position);
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* output, DWORD flags) override { return stream_->Stat(output, flags); }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clone(IStream** output) override { if (output) *output = nullptr; return E_NOTIMPL; }
private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<CancelState> cancel_;
    std::mutex operation_mutex_;
    audio::HttpAudioStream* stream_{};
};

} // namespace

HRESULT CreateLegacySoundBuffer(void** output, DWORD capacity) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!capacity) return E_INVALIDARG;
    // 004C4E14 allocates an extra 4096 bytes. Guard the original size overflow.
    if (capacity > std::numeric_limits<size_t>::max() - 4096) return E_OUTOFMEMORY;
    auto* data = static_cast<BYTE*>(std::malloc(static_cast<size_t>(capacity) + 4096));
    if (!data) return E_OUTOFMEMORY;
    auto* buffer = new (std::nothrow) SoundBuffer(data, capacity);
    if (!buffer) { std::free(data); return E_OUTOFMEMORY; }
    *output = static_cast<LegacyHostSoundBuffer*>(buffer); return S_OK;
}

HRESULT GetLegacySoundCodecName(const GUID* subtype, LPWSTR* output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!subtype) return E_INVALIDARG;
    try {
        GUID family{subtype->Data1, 0, 0x10, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
        if (*subtype == family) {
            if (subtype->Data1 >= 0x160 && subtype->Data1 <= 0x163)
                return Duplicate(L"WMA|Window Media Audio V" + std::to_wstring(subtype->Data1 - 0x159), output);
            for (const auto& item : kWaveNames) if (item.tag == subtype->Data1) return Duplicate(item.name, output);
            // Original callers initialise the output to null; retain their
            // failure result without exposing an uninitialised caller pointer.
            return E_OUTOFMEMORY;
        }
        family = {subtype->Data1, 0xdb46, 0x11cf, {0xb4,0xd1,0,0x80,0x5f,0x6c,0xbb,0xea}};
        if (*subtype == family) {
            for (const auto& item : kMpegNames) if (item.tag == subtype->Data1) return Duplicate(item.name, output);
            return E_OUTOFMEMORY;
        }
        LPOLESTR name{};
        if (FAILED(ProgIDFromCLSID(*subtype, &name))) {
            CoTaskMemFree(name); name = nullptr;
            if (FAILED(StringFromCLSID(*subtype, &name))) return Duplicate(L"{GUID}|Known Format Audio", output);
        }
        // 004C5116 copies this literal then appends the ProgID/GUID; it does
        // not call a printf formatter. Preserve the original literal "%s".
        std::wstring text;
        try { text = L"{GUID}|%s"; if (name) text += name; }
        catch (...) { CoTaskMemFree(name); throw; }
        CoTaskMemFree(name);
        return Duplicate(text, output);
    } catch (...) { return E_OUTOFMEMORY; }
}

void ConfigureLegacyHostNetwork(const audio::PlaybackOptions& options) {
    auto profile = options;
    profile.network_canceled = {};
    std::lock_guard lock(profile_mutex);
    network_profile = std::move(profile);
}

HRESULT CreateLegacyInetStream(LPCWSTR url, DWORD, IStream** output,
                              HANDLE cancel, DWORD timeout_ms) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!url || !*url) return E_INVALIDARG;
    try {
        audio::PlaybackOptions options;
        { std::lock_guard lock(profile_mutex); options = network_profile; }
        auto owner = std::make_unique<InetStream>(std::make_shared<CancelState>());
        const HRESULT result = owner->Open(url, std::move(options), cancel, timeout_ms);
        if (FAILED(result)) return result;
        *output = owner.release(); return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

} // namespace ttplayer::plugins
