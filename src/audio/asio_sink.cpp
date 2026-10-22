#include "ttplayer/audio/asio_sink.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <objbase.h>
#include <sstream>
#include <utility>
#include <vector>

namespace ttplayer::audio {
namespace {

// Standard ASIO is a deliberately non-COM interface, but TTPlayer activates
// its in-proc object as one and then calls this x86 table directly.
struct RawIAsio {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    // IASIO itself is a C++ interface (ordinary x86 member calls), not an
    // IUnknown-derived COM interface.  TTPlayer's wrapper has the first
    // three IUnknown slots, then embeds this exact SDK method order.
    virtual AsioBool init(void*) = 0;                                      // +0x0C
    virtual void getDriverName(char*) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char*) = 0;
    virtual long start() = 0;                                              // +0x1C ASIOError
    virtual long stop() = 0;                                               // +0x20 ASIOError
    virtual long getChannels(long*, long*) = 0;                            // +0x24 ASIOError
    virtual long getLatencies(long*, long*) = 0;
    virtual long getBufferSize(long*, long*, long*, long*) = 0;            // +0x2C
    virtual long canSampleRate(double) = 0;                                // +0x30
    virtual long getSampleRate(double*) = 0;
    virtual long setSampleRate(double) = 0;                                // +0x38
    virtual long getClockSources(void*, long*) = 0;
    virtual long setClockSource(long) = 0;
    virtual long getSamplePosition(void*, void*) = 0;
    virtual long getChannelInfo(AsioChannelInfo*) = 0;                     // +0x48
    virtual long createBuffers(AsioBufferInfo*, long, long, AsioCallbacks*) = 0; // +0x4C
    virtual long disposeBuffers() = 0;                                     // +0x50
    virtual long controlPanel() = 0;
    virtual long future(long, void*) = 0;
    virtual long outputReady() = 0;                                        // +0x5C
};

constexpr long kAsioOk = 0;

std::wstring ResultError(std::wstring_view stage, long code) {
    std::wostringstream text;
    text << stage << L" failed (" << std::dec << code << L", 0x"
         << std::hex << static_cast<unsigned long>(code) << L")";
    return text.str();
}

class ComAsioDriver final : public AsioDriver {
public:
    explicit ComAsioDriver(RawIAsio* value, HMODULE library = nullptr)
        : value_(value), library_(library) {}
    ~ComAsioDriver() override {
        if (value_) value_->Release();
        if (library_) FreeLibrary(library_);
    }
    bool Init(HWND window) override {
        if (value_->init(window) != 0) { error_.clear(); return true; }
        error_ = L"IASIO::init returned false";
        return false;
    }
    bool Start() override { return Check(L"IASIO::start", value_->start()); }
    bool Stop() override { return Check(L"IASIO::stop", value_->stop()); }
    bool GetChannels(long& i, long& o) override { return Check(L"IASIO::getChannels", value_->getChannels(&i, &o)); }
    bool GetBufferSize(long& a, long& b, long& c, long& d) override { return Check(L"IASIO::getBufferSize", value_->getBufferSize(&a, &b, &c, &d)); }
    bool CanSampleRate(double rate) override { return Check(L"IASIO::canSampleRate", value_->canSampleRate(rate)); }
    bool SetSampleRate(double rate) override { return Check(L"IASIO::setSampleRate", value_->setSampleRate(rate)); }
    bool GetChannelInfo(AsioChannelInfo& info) override { return Check(L"IASIO::getChannelInfo", value_->getChannelInfo(&info)); }
    bool CreateBuffers(AsioBufferInfo* b, long n, long f, AsioCallbacks* c) override { return Check(L"IASIO::createBuffers", value_->createBuffers(b, n, f, c)); }
    bool DisposeBuffers() override { return Check(L"IASIO::disposeBuffers", value_->disposeBuffers()); }
    // outputReady runs on the realtime callback. Its optional unsupported
    // result must not allocate or mutate the worker's diagnostic string.
    bool OutputReady() override { return value_->outputReady() == kAsioOk; }
    std::wstring Diagnostic() const override { return error_; }
private:
    bool Check(std::wstring_view stage, long result) {
        if (result == kAsioOk) { error_.clear(); return true; }
        error_ = ResultError(stage, result);
        return false;
    }
    RawIAsio* value_{};
    HMODULE library_{};
    std::wstring error_;
};

std::wstring DriverError(const AsioDriver& driver, std::wstring fallback) {
    const auto detail = driver.Diagnostic();
    return detail.empty() ? std::move(fallback) : detail;
}

std::unique_ptr<AsioDriver> ActivateAsio(const std::wstring& name,
    const std::wstring& module, const GUID& class_id, std::wstring& error) {
    struct LibraryOwner {
        HMODULE value{};
        ~LibraryOwner() { if (value) FreeLibrary(value); }
    } library;
    using GetClassObject = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    GetClassObject get_class_object{};
    AsioActivationAttempts attempts;
    attempts.class_factory = [&](bool& direct_retry, std::wstring& failure) -> std::unique_ptr<AsioDriver> {
        direct_retry = false;
        library.value = LoadLibraryW(module.c_str());
        if (!library.value) {
            failure = ResultError(L"ASIO LoadLibrary", static_cast<long>(GetLastError())) + L"; module=" + module;
            return {};
        }
        get_class_object = reinterpret_cast<GetClassObject>(
            GetProcAddress(library.value, "DllGetClassObject"));
        if (!get_class_object) {
            failure = ResultError(L"ASIO DllGetClassObject lookup", static_cast<long>(GetLastError()));
            return {};
        }
        IClassFactory* factory{};
        const HRESULT queried = get_class_object(class_id, IID_IClassFactory,
                                                  reinterpret_cast<void**>(&factory));
        if (FAILED(queried) || !factory) {
            if (factory) factory->Release();
            direct_retry = true;
            failure = ResultError(L"ASIO DllGetClassObject(IClassFactory)", FAILED(queried) ? queried : E_NOINTERFACE);
            return {};
        }
        RawIAsio* raw{};
        const HRESULT created = factory->CreateInstance(nullptr, class_id, reinterpret_cast<void**>(&raw));
        factory->Release();
        if (FAILED(created) || !raw) {
            if (raw) raw->Release();
            failure = ResultError(L"ASIO IClassFactory::CreateInstance", FAILED(created) ? created : E_NOINTERFACE);
            return {};
        }
        auto driver = std::make_unique<ComAsioDriver>(raw, library.value);
        library.value = nullptr;
        return driver;
    };
    attempts.direct_export = [&](std::wstring& failure) -> std::unique_ptr<AsioDriver> {
        RawIAsio* raw{};
        const HRESULT created = get_class_object(class_id, class_id, reinterpret_cast<void**>(&raw));
        if (FAILED(created) || !raw) {
            if (raw) raw->Release();
            failure = ResultError(L"ASIO DllGetClassObject(IID=CLSID)", FAILED(created) ? created : E_NOINTERFACE);
            return {};
        }
        auto driver = std::make_unique<ComAsioDriver>(raw, library.value);
        library.value = nullptr;
        return driver;
    };
    attempts.registered_class = [&](std::wstring& failure) -> std::unique_ptr<AsioDriver> {
        // FUN_004E207B drops the exceptional module before generic activation.
        if (library.value) { FreeLibrary(library.value); library.value = nullptr; }
        RawIAsio* raw{};
        const HRESULT created = CoCreateInstance(class_id, nullptr, CLSCTX_INPROC_SERVER,
                                                  class_id, reinterpret_cast<void**>(&raw));
        if (FAILED(created) || !raw) {
            if (raw) raw->Release();
            failure = ResultError(L"ASIO CoCreateInstance(CLSID, IID=CLSID)", FAILED(created) ? created : E_NOINTERFACE);
            return {};
        }
        return std::make_unique<ComAsioDriver>(raw);
    };
    return ActivateAsioWithFallback(attempts,
        _wcsicmp(name.c_str(), L"ASIO Kernel-Streaming driver") == 0 && !module.empty(), error);
}

bool ValidSource(const WAVEFORMATEX& f) noexcept {
    TtpCommPcmFormat pcm{};
    return DescribeHiresPcm(f,pcm) && !pcm.floating_point && f.nChannels > 0 && f.nChannels <= 64 &&
        f.nSamplesPerSec > 0 && (f.wBitsPerSample == 16 || f.wBitsPerSample == 24 ||
        f.wBitsPerSample == 32) && f.nBlockAlign == f.nChannels * f.wBitsPerSample / 8;
}

double ReadPcm(const std::byte* p, unsigned bits) noexcept {
    std::uint32_t value{};
    for (unsigned i = 0; i < bits / 8; ++i)
        value |= std::uint32_t{std::to_integer<unsigned char>(p[i])} << (i * 8);
    if (bits < 32 && (value & (std::uint32_t{1} << (bits - 1))))
        value |= ~((std::uint32_t{1} << bits) - 1);
    const auto signed_value = static_cast<std::int32_t>(value);
    return std::clamp(static_cast<double>(signed_value) /
        static_cast<double>(std::uint64_t{1} << (bits - 1)), -1.0, 1.0);
}

void WriteInteger(std::byte* p, const AsioChannelLayout& layout, double sample) noexcept {
    const auto max = (std::int64_t{1} << (layout.valid_bits - 1)) - 1;
    const auto min = -(std::int64_t{1} << (layout.valid_bits - 1));
    const auto value = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(sample * static_cast<double>(std::int64_t{1} << (layout.valid_bits - 1)))), min, max);
    std::uint64_t raw = static_cast<std::uint64_t>(value);
    const unsigned bytes = static_cast<unsigned>(layout.container_bits / 8);
    if (layout.big_endian) {
        for (unsigned i = 0; i < bytes; ++i)
            p[i] = static_cast<std::byte>(raw >> ((bytes - 1 - i) * 8));
    } else {
        for (unsigned i = 0; i < bytes; ++i)
            p[i] = static_cast<std::byte>(raw >> (i * 8));
    }
}

void WriteSample(std::byte* p, const AsioChannelLayout& layout, double sample) noexcept {
    sample = std::clamp(sample, -1.0, 1.0);
    if (!layout.floating_point) { WriteInteger(p, layout, sample); return; }
    if (layout.container_bits == 32) {
        float value = static_cast<float>(sample);
        auto raw = std::bit_cast<std::array<std::byte, sizeof(value)>>(value);
        if (layout.big_endian) std::reverse(raw.begin(), raw.end());
        std::memcpy(p, raw.data(), raw.size());
    } else {
        double value = sample;
        auto raw = std::bit_cast<std::array<std::byte, sizeof(value)>>(value);
        if (layout.big_endian) std::reverse(raw.begin(), raw.end());
        std::memcpy(p, raw.data(), raw.size());
    }
}
} // namespace

std::unique_ptr<AsioDriver> ActivateAsioWithFallback(
    const AsioActivationAttempts& attempts, bool special_driver, std::wstring& error) {
    error.clear();
    std::wstring failure;
    const auto retain_failure = [&] {
        if (!error.empty()) error += L"; ";
        error += failure.empty() ? L"ASIO activation returned no driver" : failure;
        failure.clear();
    };
    if (special_driver && attempts.class_factory) {
        bool direct_retry{};
        if (auto driver = attempts.class_factory(direct_retry, failure)) {
            error.clear(); return driver;
        }
        retain_failure();
        if (direct_retry && attempts.direct_export) {
            if (auto driver = attempts.direct_export(failure)) {
                error.clear(); return driver;
            }
            retain_failure();
        }
    }
    if (attempts.registered_class) {
        if (auto driver = attempts.registered_class(failure)) {
            error.clear(); return driver;
        }
        retain_failure();
    }
    if (error.empty()) error = L"ASIO activation has no available provider";
    return {};
}

bool QueryAsioDriverCapabilities(AsioDriver& driver,
    AsioDriverCapabilities& capabilities, HWND window) noexcept {
    capabilities = {};
    try {
        if (!driver.Init(window)) return false;

        long inputs{};
        long outputs{};
        if (!driver.GetChannels(inputs, outputs) || outputs <= 0 ||
            outputs > 4096) {
            return false;
        }

        long minimum{};
        long maximum{};
        long preferred{};
        long granularity{};
        if (!driver.GetBufferSize(minimum, maximum, preferred, granularity) ||
            preferred <= 0) {
            return false;
        }

        AsioChannelInfo channel{};
        channel.channel = 0;
        channel.is_input = 0;
        if (!driver.GetChannelInfo(channel)) return false;
        // FUN_004E207B passes 32 to FUN_004E14BD and copies only that
        // routine's valid-bit field into the cached descriptor.
        const auto layout = ResolveAsioChannelLayout(32, channel.type);
        if (!layout) return false;

        AsioDriverCapabilities result;
        result.output_channels = outputs;
        result.first_output_valid_bits = layout->valid_bits;
        result.preferred_buffer_frames = preferred;
        result.supported_sample_rates.reserve(
            kAsioCapabilitySampleRates.size());
        for (const long sample_rate : kAsioCapabilitySampleRates) {
            if (driver.CanSampleRate(static_cast<double>(sample_rate)))
                result.supported_sample_rates.push_back(sample_rate);
        }
        capabilities = std::move(result);
        return true;
    } catch (...) {
        capabilities = {};
        return false;
    }
}

bool ProbeAsioDriverCapabilities(const std::wstring& name,
    const std::wstring& module, const GUID& class_id,
    AsioDriverCapabilities& capabilities, std::wstring& error,
    HWND window) noexcept {
    capabilities = {};
    error.clear();
    try {
        auto driver = ActivateAsio(name, module, class_id, error);
        if (!driver) return false;
        if (!QueryAsioDriverCapabilities(*driver, capabilities, window)) {
            error = L"IASIO capability query failed";
            return false;
        }
        return true;
    } catch (...) {
        capabilities = {};
        error = L"ASIO driver raised an exception during capability probing";
        return false;
    }
}

struct AsioSink::State {
    std::mutex mutex;
    std::unique_ptr<AsioDriver> driver;
    WAVEFORMATEX source{};
    TtpCommPcmFormat source_pcm{};
    PcmPlaybackMode mode{PcmPlaybackMode::compatible};
    std::vector<std::unique_ptr<HiresPcmEncoder>> encoders;
    std::vector<double> samples;
    std::vector<std::byte> encoded;
    float left_gain{1},right_gain{1};
    std::atomic<bool> conversion_failed{};
    std::vector<AsioBufferInfo> buffers;
    std::vector<AsioChannelLayout> layouts;
    std::vector<std::byte> queue;
    size_t read_offset{};
    std::uint32_t capacity{};
    std::uint32_t frames{};
    std::atomic<std::uint64_t> consumed{};
    // PCM retained until the corresponding driver buffer is returned. A
    // bufferSwitch(i) releases the OLD contents of i; the data written by
    // that callback is still waiting for the device and must not count as played.
    std::array<std::vector<std::byte>, 2> period_source;
    std::array<size_t, 2> period_bytes{};
    std::array<std::uint64_t, 2> period_sequence{};
    std::uint64_t next_sequence{};
    std::atomic<unsigned> notifications{};
    std::atomic<double> changed_rate{};
    bool output_ready{};
    bool primed{};
    bool running{};
    bool closing{};
    unsigned callbacks{};
    AsioCallbacks callback_table{};
    HANDLE activity_event{};
    ~State() { if (activity_event) CloseHandle(activity_event); }

    void ClearDriverBuffers() noexcept {
        for (size_t channel = 0; channel < buffers.size(); ++channel)
            for (unsigned index = 0; index != 2; ++index)
                if (auto* target = buffers[channel].buffers[index])
                    std::memset(target, 0, size_t{frames} * layouts[channel].container_bits / 8);
    }

    // Called with mutex held. All per-period storage was allocated at open;
    // this routine neither allocates nor invokes a driver.
    void Fill(unsigned index) noexcept {
        const size_t frame_bytes = source.nBlockAlign;
        const size_t available = (queue.size() - read_offset) / frame_bytes;
        const size_t count = std::min<size_t>(frames, available);
        const size_t used = count * frame_bytes;
        if (used) std::memcpy(period_source[index].data(), queue.data() + read_offset, used);
        period_bytes[index] = used;
        period_sequence[index] = next_sequence++;
        const unsigned input_bytes = source.wBitsPerSample / 8;
        for (size_t channel = 0; channel < buffers.size(); ++channel) {
            auto* out = static_cast<std::byte*>(buffers[channel].buffers[index]);
            const auto& layout = layouts[channel];
            const size_t output_bytes = layout.container_bits / 8;
            if (mode!=PcmPlaybackMode::compatible) {
                // Source stays unquantized in the lookahead queue. Encode at
                // the driver boundary, once, using its real sample type.
                const double gain=channel==0?left_gain:channel==1?right_gain:
                    std::max(left_gain,right_gain);
                const bool direct_float=mode==PcmPlaybackMode::bit_exact &&
                    source_pcm.floating_point && layout.floating_point &&
                    source_pcm.bits==unsigned(layout.container_bits);
                if (direct_float) {
                    for (size_t frame=0;frame<count;++frame) {
                        const auto* p=period_source[index].data()+frame*frame_bytes+channel*input_bytes;
                        std::memcpy(out+frame*output_bytes,p,output_bytes);
                    }
                } else {
                    for (size_t frame=0;frame<count;++frame) {
                        const auto* p=period_source[index].data()+frame*frame_bytes+channel*input_bytes;
                        samples[frame]=ReadHiresSample(p,source_pcm)*0.5*gain;
                    }
                    if (!encoders[channel]->EncodeHalf({samples.data(),count},
                        {encoded.data(),count*output_bytes})) {
                        conversion_failed.store(true,std::memory_order_release);
                        std::memset(out,0,count*output_bytes);
                    } else {
                        std::memcpy(out,encoded.data(),count*output_bytes);
                        // WAVE valid bits are left aligned; ASIO's Int32LSBxx
                        // and Int32MSBxx are signed, right-aligned containers.
                        if (!layout.floating_point && layout.valid_bits<layout.container_bits) {
                            for (size_t frame=0;frame<count;++frame) {
                                int32_t value{};std::memcpy(&value,out+frame*output_bytes,4);
                                value >>= layout.container_bits-layout.valid_bits;
                                std::memcpy(out+frame*output_bytes,&value,4);
                            }
                        }
                    }
                }
                if (layout.big_endian) for (size_t frame=0;frame<count;++frame)
                    std::reverse(out+frame*output_bytes,out+(frame+1)*output_bytes);
                if (count<frames) std::memset(out+count*output_bytes,0,(frames-count)*output_bytes);
                continue;
            }
            for (size_t frame = 0; frame < count; ++frame) {
                double sample{};
                if (channel < source.nChannels || source.nChannels == 1) {
                    const size_t input_channel = source.nChannels == 1 ? 0 : channel;
                    const auto* input = period_source[index].data() + frame * frame_bytes + input_channel * input_bytes;
                    sample = ReadPcm(input, source.wBitsPerSample);
                }
                WriteSample(out + frame * output_bytes, layout, sample);
            }
            if (count < frames) std::memset(out + count * output_bytes, 0,
                                            (frames - count) * output_bytes);
        }
        read_offset += used;
        if (read_offset == queue.size()) { queue.clear(); read_offset = 0; }
        else if (read_offset > capacity / 2) {
            queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(read_offset));
            read_offset = 0;
        }
    }

    // Stop has returned and realtime callbacks no longer mutate the queue.
    // Recover every unacknowledged period in submission order for pause/resume.
    // Seek discards this data separately after the clock has stopped.
    void RequeueDriverBuffers() {
        std::vector<std::byte> pending;
        pending.reserve(period_bytes[0] + period_bytes[1] + queue.size() - read_offset);
        const unsigned first = period_sequence[0] < period_sequence[1] ? 0 : 1;
        for (const unsigned index : {first, 1u - first})
            pending.insert(pending.end(), period_source[index].begin(),
                period_source[index].begin() + static_cast<std::ptrdiff_t>(period_bytes[index]));
        pending.insert(pending.end(), queue.begin() + static_cast<std::ptrdiff_t>(read_offset), queue.end());
        queue.swap(pending);
        read_offset = 0;
        period_bytes = {};
        primed = false;
        ClearDriverBuffers();
    }

    void Consume(long index) noexcept {
        {
            std::scoped_lock lock(mutex);
            if (closing || !running || index < 0 || index > 1) return;
            const auto slot = static_cast<unsigned>(index);
            consumed.fetch_add(period_bytes[slot], std::memory_order_relaxed);
            Fill(slot);
        }
        if (output_ready) static_cast<void>(driver->OutputReady());
        if (activity_event) SetEvent(activity_event);
    }
};

namespace {
std::mutex g_callback_mutex;
std::condition_variable g_callback_idle;
void* g_active_sink{};
} // namespace

AsioSink::AsioSink() = default;
AsioSink::~AsioSink() { Close(); }

bool AsioSink::OpenDevice(const std::wstring& name, const std::wstring& module,
    const GUID& class_id, const WAVEFORMATEX& source, std::uint32_t requested, HWND window,
    PcmPlaybackMode mode, HMODULE audio_module, int dither) {
    Close(); error_.clear(); diagnostic_.clear();
    auto driver = ActivateAsio(name, module, class_id, error_);
    return driver && OpenDriver(std::move(driver), source, requested, window,mode,audio_module,dither);
}

bool AsioSink::OpenDriver(std::unique_ptr<AsioDriver> driver, const WAVEFORMATEX& source,
    std::uint32_t requested, HWND window, PcmPlaybackMode mode, HMODULE audio_module,int dither) {
    Close(); error_.clear(); diagnostic_.clear();
    TtpCommPcmFormat source_pcm{};
    if (!driver || (mode==PcmPlaybackMode::compatible ? !ValidSource(source) :
        !DescribeHiresPcm(source,source_pcm))) { error_ = L"ASIO requires a valid PCM source"; return false; }
    {
        std::scoped_lock lock(g_callback_mutex);
        if (g_active_sink) { error_ = L"Only one active ASIO callback owner is supported"; return false; }
    }
    auto state = std::make_unique<State>();
    state->driver = std::move(driver); state->source = source;
    state->mode=mode;state->source_pcm=source_pcm;
    state->activity_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->activity_event) { error_ = L"Unable to create ASIO callback event"; return false; }
    if (!state->driver->Init(window)) { error_ = DriverError(*state->driver, L"IASIO::init failed"); return false; }
    long inputs{}, outputs{}, minimum{}, maximum{}, preferred{}, granularity{};
    if (!state->driver->GetChannels(inputs, outputs)) {
        error_ = DriverError(*state->driver, L"IASIO::getChannels failed"); return false;
    }
    if (outputs < (mode==PcmPlaybackMode::compatible ? 2 : source.nChannels) || outputs > 4096) {
        error_ = L"IASIO::getChannels returned unusable output count " + std::to_wstring(outputs); return false;
    }
    if (!state->driver->GetBufferSize(minimum, maximum, preferred, granularity)) {
        error_ = DriverError(*state->driver, L"IASIO::getBufferSize failed"); return false;
    }
    const auto plan = PlanAsioBuffers(requested, source.nBlockAlign, preferred);
    if (!plan || preferred < minimum || preferred > maximum ||
        plan->queued_source_bytes > 64u * 1024 * 1024) {
        error_ = L"IASIO preferred buffer size is invalid or exceeds the bounded PCM queue"; return false;
    }
    // The original open path calls setSampleRate; capability queries must not
    // veto a rate that the driver can actually select.
    if (!state->driver->CanSampleRate(static_cast<double>(source.nSamplesPerSec)))
        diagnostic_ = DriverError(*state->driver, L"IASIO::canSampleRate did not confirm the requested rate") +
            L"; attempting setSampleRate at " + std::to_wstring(source.nSamplesPerSec) + L" Hz";
    if (!state->driver->SetSampleRate(static_cast<double>(source.nSamplesPerSec))) {
        error_ = DriverError(*state->driver, L"IASIO::setSampleRate failed") + L"; requested=" +
            std::to_wstring(source.nSamplesPerSec) + L" Hz"; return false;
    }
    // 004E1610 creates max(2, min(source channels, driver outputs)) buffers,
    // not every physical output offered by the driver.  A mono source is
    // deliberately duplicated to the first stereo pair; excess source
    // channels are truncated only when the selected driver exposes fewer.
    const long stream_channels = mode==PcmPlaybackMode::compatible ?
        std::clamp<long>(source.nChannels, 2, outputs) : source.nChannels;
    if (mode!=PcmPlaybackMode::compatible && source.nChannels>2)
        diagnostic_ += L"; channel order maps explicitly to ASIO output indices 0..N-1; verify driver routing";
    state->frames = plan->frames_per_buffer; state->capacity = plan->queued_source_bytes;
    state->buffers.resize(static_cast<size_t>(stream_channels));
    state->layouts.resize(static_cast<size_t>(stream_channels));
    for (long channel = 0; channel < stream_channels; ++channel) {
        auto& info = state->buffers[static_cast<size_t>(channel)];
        info.is_input = 0; info.channel_num = channel;
        AsioChannelInfo channel_info{}; channel_info.channel = channel; channel_info.is_input = 0;
        if (!state->driver->GetChannelInfo(channel_info)) {
            error_ = DriverError(*state->driver, L"IASIO::getChannelInfo failed") +
                L"; output=" + std::to_wstring(channel); return false;
        }
        auto layout = ResolveAsioChannelLayout(mode==PcmPlaybackMode::compatible ? source.wBitsPerSample : 32,
            channel_info.type);
        if (mode!=PcmPlaybackMode::compatible && channel_info.type>=8 && channel_info.type<=11) {
            constexpr int valid[]{16,18,20,24};
            layout=AsioChannelLayout{32,valid[channel_info.type-8],false,true};
        }
        if (!layout) {
            error_ = L"IASIO output sample type " + std::to_wstring(channel_info.type) +
                L" is unsupported; output=" + std::to_wstring(channel); return false;
        }
        state->layouts[static_cast<size_t>(channel)] = *layout;
        if (mode!=PcmPlaybackMode::compatible) {
            auto input=source_pcm;input.channels=1;input.block_align=input.bits/8;input.channel_mask=0;
            auto target=input;target.bits=layout->container_bits;target.valid_bits=layout->valid_bits;
            target.block_align=target.bits/8;target.floating_point=layout->floating_point ? 1:0;
            auto encoder=std::make_unique<HiresPcmEncoder>();
            if (!encoder->Open(audio_module,input,target,mode,state->frames,error_,dither,
                0x6d2b79f5u+uint32_t(channel)*0x9e3779b9u)) return false;
            state->encoders.push_back(std::move(encoder));
        }
    }
    const size_t period_bytes = size_t{state->frames} * source.nBlockAlign;
    if (mode!=PcmPlaybackMode::compatible) {
        state->samples.resize(state->frames);state->encoded.resize(size_t(state->frames)*8);
    }
    for (auto& pcm : state->period_source) pcm.resize(period_bytes);
    state->callback_table = {&BufferSwitch, &SampleRateDidChange, &AsioMessage,
                             &BufferSwitchTimeInfo};
    if (!state->driver->CreateBuffers(state->buffers.data(), stream_channels, preferred,
                                      &state->callback_table)) {
        // A driver is permitted to allocate a subset before reporting an
        // error. Match 004E1383's disposal-before-release ordering even on
        // this failed open path.
        error_ = DriverError(*state->driver, L"IASIO::createBuffers failed");
        static_cast<void>(state->driver->DisposeBuffers());
        return false;
    }
    for (const auto& buffer : state->buffers) {
        if (!buffer.buffers[0] || !buffer.buffers[1]) {
            error_ = L"IASIO::createBuffers returned a null output buffer";
            static_cast<void>(state->driver->DisposeBuffers());
            return false;
        }
    }
    state->ClearDriverBuffers();
    state->output_ready = state->driver->OutputReady();
    bool claimed{};
    {
        std::scoped_lock lock(g_callback_mutex);
        if (!g_active_sink) {
            g_active_sink = state.get();
            claimed = true;
        }
    }
    if (!claimed) {
        // Dispose can reenter a callback. Never call it under the global
        // callback lock even when another opener won the ownership race.
        static_cast<void>(state->driver->DisposeBuffers());
        error_ = L"Only one active ASIO callback owner is supported";
        return false;
    }
    state_ = std::move(state);
    return true;
}

bool AsioSink::Submit(std::span<const std::byte> source, float left_gain, float right_gain) {
    if (!state_ || source.empty() || source.size() % state_->source.nBlockAlign ||
        !std::isfinite(left_gain) || !std::isfinite(right_gain)) { error_ = L"Invalid ASIO source packet"; return false; }
    std::scoped_lock lock(state_->mutex);
    if (state_->mode==PcmPlaybackMode::bit_exact &&
        (left_gain!=1 || right_gain!=1 || !FiniteHiresPacket(source,state_->source_pcm))) {
        error_=L"Bit-exact ASIO rejects gain changes and nonfinite floating samples";return false;
    }
    if (state_->mode!=PcmPlaybackMode::compatible) {
        state_->left_gain=std::clamp(left_gain,0.0F,1.0F);
        state_->right_gain=std::clamp(right_gain,0.0F,1.0F);
    }
    const size_t pending = state_->queue.size() - state_->read_offset +
        state_->period_bytes[0] + state_->period_bytes[1];
    if (pending > state_->capacity || source.size() > state_->capacity - pending) { error_ = L"ASIO source queue is full"; return false; }
    // Gain is applied in source PCM before the callback so each driver buffer
    // sees stable frame data.  The callback remains bounded and allocation-free.
    std::vector<std::byte> copy(source.begin(), source.end());
    const unsigned bytes = state_->source.wBitsPerSample / 8;
    if (state_->mode==PcmPlaybackMode::compatible)
    for (size_t frame = 0; frame < copy.size() / state_->source.nBlockAlign; ++frame) {
        for (unsigned channel = 0; channel < state_->source.nChannels; ++channel) {
            const float gain = std::clamp(channel == 0 ? left_gain : channel == 1 ? right_gain : 1.0F, 0.0F, 1.0F);
            if (gain == 1.0F) continue;
            auto* p = copy.data() + frame * state_->source.nBlockAlign + channel * bytes;
            const double value = ReadPcm(p, state_->source.wBitsPerSample) * gain;
            AsioChannelLayout pcm{static_cast<int>(state_->source.wBitsPerSample), static_cast<int>(state_->source.wBitsPerSample), false, false};
            WriteInteger(p, pcm, value);
        }
    }
    if (state_->read_offset && state_->read_offset == state_->queue.size()) { state_->queue.clear(); state_->read_offset = 0; }
    state_->queue.insert(state_->queue.end(), copy.begin(), copy.end());
    return true;
}

bool AsioSink::SetPaused(bool paused) {
    if (!state_) { error_ = L"ASIO stream is not open"; return false; }
    if (!CheckStatus()) return false;
    const bool requested_running = !paused;
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->closing) { error_ = L"ASIO stream is closing"; return false; }
        if (state_->running == requested_running) return true;
        if (requested_running && !state_->primed) {
            // FUN_004E19E5 fills buffer B before start. The initial callback
            // fills A while B is playing; no bytes have completed yet.
            state_->ClearDriverBuffers();
            state_->Fill(1);
            state_->primed = true;
        }
        state_->running = requested_running;
    }
    // Drivers may synchronously invoke callbacks from Start/Stop. Do not hold
    // the PCM mutex while entering either operation.
    const bool succeeded = paused ? state_->driver->Stop() : state_->driver->Start();
    if (!succeeded) {
        error_ = DriverError(*state_->driver, paused ? L"IASIO::stop failed" : L"IASIO::start failed");
        {
            std::scoped_lock lock(state_->mutex);
            if (!state_->closing) state_->running = !requested_running;
        }
        if (requested_running) {
            // A driver may partially start before returning an error. Quiesce
            // it before restoring unplayed PCM, preserving the start error.
            static_cast<void>(state_->driver->Stop());
            std::scoped_lock lock(state_->mutex);
            state_->RequeueDriverBuffers();
        }
        return false;
    }
    if (paused) {
        std::scoped_lock lock(state_->mutex);
        state_->RequeueDriverBuffers();
    }
    return true;
}

bool AsioSink::Reset() {
    if (!state_) { error_ = L"ASIO stream is not open"; return false; }
    if (!SetPaused(true)) return false;
    std::scoped_lock lock(state_->mutex);
    state_->queue.clear(); state_->read_offset = 0;
    state_->period_bytes = {};
    state_->primed = false;
    state_->consumed.store(0, std::memory_order_relaxed);
    state_->conversion_failed.store(false);
    for (auto& encoder:state_->encoders) encoder->Reset();
    state_->ClearDriverBuffers();
    return true;
}

bool AsioSink::CheckStatus() {
    if (!state_) { error_ = L"ASIO stream is not open"; return false; }
    if (state_->conversion_failed.load(std::memory_order_acquire)) {
        error_=L"ASIO final high precision encoding failed";return false;
    }
    const unsigned notifications = state_->notifications.load(std::memory_order_acquire);
    if (!notifications) return true;
    if (notifications & 1u)
        error_ = L"ASIO driver requested reinitialization; restart playback to reopen the selected device";
    else
        error_ = L"ASIO driver sample clock changed to " + std::to_wstring(state_->changed_rate.load()) +
            L" Hz; restart playback to reopen the selected device";
    return false;
}

void AsioSink::Close() noexcept {
    if (!state_) return;
    State* state = state_.get();
    bool was_running{};
    {
        std::unique_lock lock(g_callback_mutex);
        if (g_active_sink == state) g_active_sink = nullptr;
        {
            std::scoped_lock state_lock(state->mutex);
            state->closing = true;
            was_running = state->running;
            state->running = false;
        }
        g_callback_idle.wait(lock, [&] { return state->callbacks == 0; });
    }
    // A driver may synchronously wait for bufferSwitch in Stop/DisposeBuffers.
    // The global target was detached and all entered callbacks drained above;
    // holding State::mutex here would deadlock a callback that began before
    // detachment.
    if (was_running) static_cast<void>(state->driver->Stop());
    static_cast<void>(state->driver->DisposeBuffers());
    state_.reset();
}

std::uint32_t AsioSink::PeriodFrames() const noexcept { return state_ ? state_->frames : 0; }
std::uint32_t AsioSink::CapacitySourceBytes() const noexcept { return state_ ? state_->capacity : 0; }
std::uint64_t AsioSink::ConsumedSourceBytes() const noexcept {
    return state_ ? state_->consumed.load(std::memory_order_relaxed) : 0;
}
HANDLE AsioSink::ActivityEvent() const noexcept { return state_ ? state_->activity_event : nullptr; }
const std::vector<AsioChannelLayout>& AsioSink::ChannelLayouts() const noexcept {
    static const std::vector<AsioChannelLayout> empty;
    return state_ ? state_->layouts : empty;
}
std::uint32_t AsioSink::SampleRate() const noexcept {
    return state_ ? state_->source.nSamplesPerSec : 0;
}

void AsioSink::BufferSwitch(long index, AsioBool) noexcept {
    State* state{};
    { std::scoped_lock lock(g_callback_mutex); state = static_cast<State*>(g_active_sink); if (!state) return; ++state->callbacks; }
    state->Consume(index);
    { std::scoped_lock lock(g_callback_mutex); if (--state->callbacks == 0) g_callback_idle.notify_all(); }
}
void AsioSink::SampleRateDidChange(double rate) noexcept {
    std::scoped_lock lock(g_callback_mutex);
    auto* state = static_cast<State*>(g_active_sink);
    if (!state || rate == static_cast<double>(state->source.nSamplesPerSec)) return;
    state->changed_rate.store(rate, std::memory_order_relaxed);
    state->notifications.fetch_or(2u, std::memory_order_release);
    if (state->activity_event) SetEvent(state->activity_event);
}
long AsioSink::AsioMessage(long selector, long value, void*, double*) noexcept {
    // Engine version and time-info are actually implemented. Do not advertise
    // timecode/resync/latency features which the host does not consume.
    if (selector == 1) return value == 2 || value == 3 || value == 7 ? 1 : 0;
    if (selector == 2) return 2;
    if (selector == 7) return 1;
    if (selector != 3) return 0;
    std::scoped_lock lock(g_callback_mutex);
    auto* state = static_cast<State*>(g_active_sink);
    if (!state) return 0;
    // The worker tears down the clock and presents the actionable error; the
    // realtime driver thread never reenters COM/disposeBuffers.
    state->notifications.fetch_or(1u, std::memory_order_release);
    if (state->activity_event) SetEvent(state->activity_event);
    return 1;
}
AsioTime* AsioSink::BufferSwitchTimeInfo(AsioTime*, long index, AsioBool direct) noexcept { BufferSwitch(index, direct); return nullptr; }

} // namespace ttplayer::audio
