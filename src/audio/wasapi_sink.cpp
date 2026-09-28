#include "ttplayer/audio/wasapi_sink.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace ttplayer::audio {
namespace {
using Microsoft::WRL::ComPtr;
constexpr PROPERTYKEY kFriendlyName{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};
constexpr GUID kPcmSubtype{1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};

ComPtr<IMMDeviceEnumerator> Enumerator() {
    ComPtr<IMMDeviceEnumerator> value;
    // COM activation is available on XP too; REGDB_E_CLASSNOTREG is harmless.
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&value));
    return value;
}

WasapiEndpoint Describe(IMMDevice* device, bool default_device) {
    WasapiEndpoint result;
    LPWSTR identifier{};
    if (SUCCEEDED(device->GetId(&identifier)) && identifier) {
        result.id = identifier;
        CoTaskMemFree(identifier);
    }
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
        PROPVARIANT name{};
        if (SUCCEEDED(properties->GetValue(kFriendlyName, &name)) &&
            name.vt == VT_LPWSTR && name.pwszVal) result.name = name.pwszVal;
        PropVariantClear(&name);
    }
    if (result.name.empty()) result.name = result.id;
    if (default_device) {
        result.id.clear();
        result.name = L"默认音频设备 (" + result.name + L")";
    }
    ComPtr<IAudioClient> client;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                                  nullptr, &client))) {
        WAVEFORMATEX* format{};
        if (SUCCEEDED(client->GetMixFormat(&format)) && format) {
            result.mix_format = std::to_wstring(format->nSamplesPerSec) + L" Hz / " +
                std::to_wstring(format->wBitsPerSample) + L" bit / " +
                std::to_wstring(format->nChannels) + L" ch";
            CoTaskMemFree(format);
        }
    }
    return result;
}

WAVEFORMATEXTENSIBLE PcmFormat(const WAVEFORMATEX& source, WORD bits,
                             bool extended) {
    WAVEFORMATEXTENSIBLE value{};
    value.Format = source;
    value.Format.wFormatTag = extended ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM;
    value.Format.wBitsPerSample = bits;
    value.Format.nBlockAlign = static_cast<WORD>(source.nChannels * (bits / 8));
    value.Format.nAvgBytesPerSec = source.nSamplesPerSec * value.Format.nBlockAlign;
    value.Format.cbSize = extended ? 22 : 0;
    value.Samples.wValidBitsPerSample = bits;
    // Standard WAVE channel order. The preceding PCM transform preserves it.
    constexpr DWORD masks[]{0,0x4,0x3,0x7,0x33,0x37,0x3f,0x13f,0x63f};
    value.dwChannelMask = source.nChannels < std::size(masks)
        ? masks[source.nChannels] : 0;
    value.SubFormat = kPcmSubtype;
    return value;
}
} // namespace

std::vector<WasapiEndpoint> EnumerateWasapiEndpoints() {
    std::vector<WasapiEndpoint> result;
    const auto enumerator = Enumerator();
    if (!enumerator) return result;
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)))
        result.push_back(Describe(device.Get(), true));
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
        return result;
    UINT count{};
    if (FAILED(devices->GetCount(&count))) return result;
    for (UINT index = 0; index < count && index < 1024; ++index) {
        device.Reset();
        if (FAILED(devices->Item(index, &device))) continue;
        auto entry = Describe(device.Get(), false);
        if (!entry.id.empty()) result.push_back(std::move(entry));
    }
    return result;
}

namespace {
// Created, used and released only on WasapiSink's render thread. In
// particular, GetService/GetBuffer/ReleaseBuffer/Release never cross threads.
struct WasapiStream {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX source{};
    WAVEFORMATEXTENSIBLE format{};
    UINT32 capacity{};
    bool paused{true};
    bool started{};
    std::deque<std::vector<std::byte>> queue;
    size_t offset{};
    std::uint64_t written_frames{}, played_frames{};
    std::wstring error;
    HRESULT error_result{S_OK};

    bool OpenDevice(const std::wstring& endpoint_id, bool exclusive,
                    const WAVEFORMATEX& source_format, int buffer_ms);
    bool Submit(std::span<const std::byte> pcm);
    bool Pump(float left_gain, float right_gain);
    bool SetPaused(bool value);
    bool Reset();
    void Close() noexcept;
    std::uint64_t PositionSourceBytes();

    bool Fail(HRESULT hr, const wchar_t* operation) {
        if (SUCCEEDED(hr)) return true;
        error_result = hr;
        wchar_t hex[16]{};
        swprintf_s(hex, L"0x%08X", static_cast<unsigned>(hr));
        error = std::wstring(L"WASAPI ") + operation + L"失败 (" + hex + L")";
        if (hr == AUDCLNT_E_DEVICE_IN_USE) error += L"：设备已被其它程序占用。";
        else if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
            error += L"：请在系统声音设置中允许独占模式，或选择 WASAPI 共享模式。";
        else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
            error += L"：设备不支持该采样率、位深或声道数，请调整音频设备选项或使用共享模式。";
        else if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
            error += L"：音频设备已断开或配置已更改，请重新选择设备。";
        else if (hr == REGDB_E_CLASSNOTREG)
            error += L"：当前系统不支持 WASAPI，请选择 WaveOut 或 DirectSound。";
        return false;
    }
    bool Padding(UINT32& frames) {
        if (!client || !Fail(client->GetCurrentPadding(&frames), L"查询缓冲区"))
            return false;
        if (frames > capacity) return Fail(E_UNEXPECTED, L"缓冲区大小");
        // Padding counts in the initialized stream format, including shared
        // mode's automatic sample-rate conversion. Silence during underruns
        // must not advance the source clock or skip queued music.
        played_frames = std::max(played_frames,
            written_frames - std::min<std::uint64_t>(written_frames, frames));
        return true;
    }
};

bool WasapiStream::OpenDevice(const std::wstring& endpoint_id, bool exclusive,
                           const WAVEFORMATEX& source_format, int buffer_ms) {
    Close();
    auto& s = *this;
    if (!s.error.empty()) return false;
    s.source = source_format;
    if (s.source.wFormatTag != WAVE_FORMAT_PCM || s.source.nChannels == 0 ||
        s.source.nChannels > 8 || s.source.nSamplesPerSec < 1000 ||
        s.source.nSamplesPerSec > 768000 ||
        (s.source.wBitsPerSample != 8 && s.source.wBitsPerSample != 16 &&
         s.source.wBitsPerSample != 24 && s.source.wBitsPerSample != 32) ||
        s.source.nBlockAlign != s.source.nChannels * (s.source.wBitsPerSample / 8))
        return s.Fail(E_INVALIDARG, L"PCM 格式");
    const auto enumerator = Enumerator();
    if (!enumerator) return s.Fail(REGDB_E_CLASSNOTREG, L"初始化");
    const HRESULT found = endpoint_id.empty()
        ? enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &s.device)
        : enumerator->GetDevice(endpoint_id.c_str(), &s.device);
    if (!s.Fail(found, L"打开音频设备")) return false;
    const auto activate = [&] {
        s.client.Reset();
        return s.device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                                  nullptr, &s.client);
    };
    if (!s.Fail(activate(), L"创建音频流")) return false;
    s.format = PcmFormat(s.source, s.source.wBitsPerSample,
                        s.source.nChannels > 2 || s.source.wBitsPerSample > 16);
    if (exclusive) {
        bool supported{};
        // Keep rate/channels explicit; try container widths at that rate only.
        for (const WORD bits : {s.source.wBitsPerSample, WORD{32}, WORD{24}, WORD{16}}) {
            for (const bool extended : {true, false}) {
                if (!extended && s.source.nChannels > 2) continue;
                const auto candidate = PcmFormat(s.source, bits, extended);
                if (s.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                &candidate.Format, nullptr) == S_OK) {
                    s.format = candidate;
                    supported = true;
                    break;
                }
            }
            if (supported) break;
        }
        if (!supported) return s.Fail(AUDCLNT_E_UNSUPPORTED_FORMAT, L"独占格式协商");
    }
    // Retain the configured decoded lookahead in AudioEngine. Keep the native
    // buffer short so software volume, balance and fades respond promptly.
    REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(std::clamp(buffer_ms, 40, 80)) * 10000;
    const auto mode = exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
    const DWORD flags = exclusive ? 0 :
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    auto initialize = [&] {
        return s.client->Initialize(mode, flags, duration, 0, &s.format.Format, nullptr);
    };
    HRESULT hr = initialize();
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 frames{};
        if (!s.Fail(s.client->GetBufferSize(&frames), L"查询对齐尺寸")) return false;
        duration = static_cast<REFERENCE_TIME>((10000000ULL * frames +
            s.format.Format.nSamplesPerSec - 1) / s.format.Format.nSamplesPerSec);
        if (!s.Fail(activate(), L"重新创建音频流")) return false;
        hr = initialize();
    }
    if (!s.Fail(hr, exclusive ? L"初始化独占模式" : L"初始化共享模式") ||
        !s.Fail(s.client->GetBufferSize(&s.capacity), L"查询缓冲区大小") ||
        !s.Fail(s.client->GetService(IID_PPV_ARGS(&s.render)), L"创建渲染接口")) return false;
    if (s.capacity == 0) return s.Fail(E_UNEXPECTED, L"空音频缓冲区");
    return true;
}

bool WasapiStream::Submit(std::span<const std::byte> pcm) {
    auto& s = *this;
    if (!s.render || pcm.empty() || pcm.size() % s.source.nBlockAlign != 0 ||
        pcm.size() > 4 * 1024 * 1024 || s.queue.size() >= 4)
        return s.Fail(E_INVALIDARG, L"提交音频数据");
    s.queue.emplace_back(pcm.begin(), pcm.end());
    return true;
}

bool WasapiStream::Pump(float left_gain, float right_gain) {
    auto& s = *this;
    if (!s.error.empty()) return false;
    if (s.paused) return true;
    UINT32 padding{};
    if (!s.Padding(padding)) return false;
    UINT32 available = s.capacity - padding;
    while (available && !s.queue.empty()) {
        auto& packet = s.queue.front();
        const UINT32 frames = static_cast<UINT32>(std::min<size_t>(available,
            (packet.size() - s.offset) / s.source.nBlockAlign));
        BYTE* destination{};
        if (!s.Fail(s.render->GetBuffer(frames, &destination), L"取得音频缓冲区")) return false;
        const size_t input_bytes = s.source.wBitsPerSample / 8;
        const size_t output_bytes = s.format.Format.wBitsPerSample / 8;
        const auto* input = reinterpret_cast<const BYTE*>(packet.data()) + s.offset;
        for (size_t sample = 0; sample < static_cast<size_t>(frames) * s.source.nChannels; ++sample) {
            const size_t channel = sample % s.source.nChannels;
            const double gain = std::clamp(static_cast<double>(
                channel == 1 ? right_gain : channel == 0 ? left_gain :
                    std::max(left_gain, right_gain)), 0.0, 1.0);
            std::int64_t value{};
            if (input_bytes == 1) value = static_cast<int>(*input) - 128;
            else {
                std::uint32_t raw{};
                for (size_t byte = 0; byte < input_bytes; ++byte)
                    raw |= static_cast<std::uint32_t>(input[byte]) << (byte * 8);
                const std::int64_t sign = std::int64_t{1} << (s.source.wBitsPerSample - 1);
                value = (static_cast<std::int64_t>(raw) ^ sign) - sign;
            }
            const double scale = std::ldexp(gain,
                s.format.Format.wBitsPerSample - s.source.wBitsPerSample);
            const std::int64_t limit = std::int64_t{1} << (s.format.Format.wBitsPerSample - 1);
            value = std::clamp<std::int64_t>(std::llround(value * scale), -limit, limit - 1);
            if (output_bytes == 1) *destination = static_cast<BYTE>(value + 128);
            else for (size_t byte = 0; byte < output_bytes; ++byte)
                destination[byte] = static_cast<BYTE>(static_cast<std::uint64_t>(value) >> (byte * 8));
            input += input_bytes;
            destination += output_bytes;
        }
        if (!s.Fail(s.render->ReleaseBuffer(frames, 0), L"提交音频缓冲区")) return false;
        s.written_frames += frames;
        s.offset += static_cast<size_t>(frames) * s.source.nBlockAlign;
        available -= frames;
        if (s.offset == packet.size()) { s.queue.pop_front(); s.offset = 0; }
    }
    if (!s.started && s.written_frames > s.played_frames) {
        if (!s.Fail(s.client->Start(), L"开始播放")) return false;
        s.started = true;
    }
    return true;
}

bool WasapiStream::SetPaused(bool value) {
    auto& s = *this;
    if (!s.error.empty()) return false;
    if (value && s.started) {
        if (!s.Fail(s.client->Stop(), L"暂停播放")) return false;
        s.started = false;
    }
    s.paused = value;
    return true; // Pump starts/resumes after prefill on this same thread.
}

bool WasapiStream::Reset() {
    auto& s = *this;
    if (!s.client || !SetPaused(true) || !s.Fail(s.client->Reset(), L"重置音频流")) return false;
    s.queue.clear();
    s.offset = 0;
    s.written_frames = s.played_frames = 0;
    return true;
}

void WasapiStream::Close() noexcept {
    if (client && started) client->Stop();
    started = false;
    paused = true;
    render.Reset();
    client.Reset();
    device.Reset();
    queue.clear();
}

std::uint64_t WasapiStream::PositionSourceBytes() {
    UINT32 padding{};
    Padding(padding);
    return played_frames * source.nBlockAlign;
}
} // namespace

struct WasapiSink::Impl {
    WasapiStream stream;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::shared_ptr<std::packaged_task<void()>> command;
    bool closing{};
    float left_gain{}, right_gain{};
    // Only synchronous facade calls update these snapshots. Returning Error
    // by reference must not expose a string concurrently changed by Pump.
    std::wstring reported_error;
    HRESULT reported_result{S_OK};

    Impl() {
        const int priority = GetThreadPriority(GetCurrentThread());
        worker = std::thread([this, priority] {
            if (priority != THREAD_PRIORITY_ERROR_RETURN)
                SetThreadPriority(GetCurrentThread(), std::max(priority, THREAD_PRIORITY_HIGHEST));
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            stream.Fail(com, L"COM initialization");
            for (;;) {
                std::shared_ptr<std::packaged_task<void()>> work;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(5),
                                  [this] { return closing || command; });
                    if (closing) break;
                    work = std::exchange(command, nullptr);
                }
                // The producer can spend hundreds of milliseconds decoding,
                // resampling or waiting for a DSP window. Already processed
                // PCM must keep reaching the 40-80 ms endpoint in that time.
                if (work) (*work)();
                if (stream.client && !stream.paused && stream.error.empty())
                    stream.Pump(left_gain, right_gain);
            }
            stream.Close();
            if (SUCCEEDED(com)) CoUninitialize();
        });
    }
    ~Impl() { Stop(); }

    void Invoke(std::function<void()> function) {
        auto task = std::make_shared<std::packaged_task<void()>>([&] {
            function();
            reported_error = stream.error;
            reported_result = stream.error_result;
        });
        auto result = task->get_future();
        {
            std::scoped_lock lock(mutex);
            if (closing) return;
            command = std::move(task);
        }
        wake.notify_one();
        result.get();
    }
    void Stop() noexcept {
        if (!worker.joinable()) return;
        {
            std::scoped_lock lock(mutex);
            closing = true;
        }
        wake.notify_one();
        worker.join();
    }
};

WasapiSink::WasapiSink() = default;
WasapiSink::~WasapiSink() { Close(); }

bool WasapiSink::OpenDevice(const std::wstring& endpoint_id, bool exclusive,
                           const WAVEFORMATEX& source_format, int buffer_ms) {
    Close();
    impl_ = std::make_unique<Impl>();
    bool opened{};
    impl_->Invoke([&] {
        opened = impl_->stream.OpenDevice(endpoint_id, exclusive, source_format, buffer_ms);
    });
    if (!opened) impl_->Stop();
    return opened;
}

bool WasapiSink::Submit(std::span<const std::byte> pcm) {
    if (!impl_) return false;
    bool submitted{};
    impl_->Invoke([&] { submitted = impl_->stream.Submit(pcm); });
    return submitted;
}

bool WasapiSink::Pump(float left_gain, float right_gain) {
    if (!impl_) return false;
    bool good{};
    impl_->Invoke([&] {
        impl_->left_gain = left_gain;
        impl_->right_gain = right_gain;
        good = impl_->stream.Pump(left_gain, right_gain);
    });
    return good;
}

bool WasapiSink::SetPaused(bool paused) {
    if (!impl_) return false;
    bool good{};
    impl_->Invoke([&] { good = impl_->stream.SetPaused(paused); });
    return good;
}

bool WasapiSink::Reset() {
    if (!impl_) return false;
    bool good{};
    impl_->Invoke([&] { good = impl_->stream.Reset(); });
    return good;
}

void WasapiSink::Close() noexcept {
    if (impl_) impl_->Stop();
}

std::uint64_t WasapiSink::PositionSourceBytes() {
    if (!impl_) return 0;
    std::uint64_t position{};
    impl_->Invoke([&] { position = impl_->stream.PositionSourceBytes(); });
    return position;
}
const WAVEFORMATEX& WasapiSink::DeviceFormat() const noexcept {
    static const WAVEFORMATEX empty{};
    return impl_ ? impl_->stream.format.Format : empty;
}
const std::wstring& WasapiSink::Error() const noexcept {
    static const std::wstring empty;
    return impl_ ? impl_->reported_error : empty;
}
HRESULT WasapiSink::ErrorResult() const noexcept {
    return impl_ ? impl_->reported_result : S_OK;
}
} // namespace ttplayer::audio
