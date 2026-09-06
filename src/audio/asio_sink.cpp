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

class ComAsioDriver final : public AsioDriver {
public:
    explicit ComAsioDriver(RawIAsio* value, HMODULE library = nullptr)
        : value_(value), library_(library) {}
    ~ComAsioDriver() override {
        if (value_) value_->Release();
        if (library_) FreeLibrary(library_);
    }
    bool Init(HWND window) override { return value_->init(window) != 0; }
    bool Start() override { return value_->start() == kAsioOk; }
    bool Stop() override { return value_->stop() == kAsioOk; }
    bool GetChannels(long& i, long& o) override { return value_->getChannels(&i, &o) == kAsioOk; }
    bool GetBufferSize(long& a, long& b, long& c, long& d) override { return value_->getBufferSize(&a, &b, &c, &d) == kAsioOk; }
    bool CanSampleRate(double rate) override { return value_->canSampleRate(rate) == kAsioOk; }
    bool SetSampleRate(double rate) override { return value_->setSampleRate(rate) == kAsioOk; }
    bool GetChannelInfo(AsioChannelInfo& info) override { return value_->getChannelInfo(&info) == kAsioOk; }
    bool CreateBuffers(AsioBufferInfo* b, long n, long f, AsioCallbacks* c) override { return value_->createBuffers(b, n, f, c) == kAsioOk; }
    bool DisposeBuffers() override { return value_->disposeBuffers() == kAsioOk; }
    bool OutputReady() override { return value_->outputReady() == kAsioOk; }
private:
    RawIAsio* value_{};
    HMODULE library_{};
};

std::unique_ptr<AsioDriver> ActivateAsio(const std::wstring& name,
    const std::wstring& module, const GUID& class_id, std::wstring& error) {
    RawIAsio* raw{};
    if (_wcsicmp(name.c_str(), L"ASIO Kernel-Streaming driver") == 0 && !module.empty()) {
        HMODULE library = LoadLibraryW(module.c_str());
        if (!library) { error = L"ASIO Kernel-Streaming driver module could not be loaded"; return {}; }
        using GetClassObject = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
        const auto get_class_object = reinterpret_cast<GetClassObject>(
            GetProcAddress(library, "DllGetClassObject"));
        IClassFactory* factory{};
        const HRESULT result = get_class_object
            ? get_class_object(class_id, IID_IClassFactory, reinterpret_cast<void**>(&factory))
            : E_NOINTERFACE;
        if (FAILED(result) || !factory ||
            FAILED(factory->CreateInstance(nullptr, class_id, reinterpret_cast<void**>(&raw))) || !raw) {
            if (factory) factory->Release();
            FreeLibrary(library);
            error = L"ASIO Kernel-Streaming driver DllGetClassObject activation failed";
            return {};
        }
        factory->Release();
        return std::make_unique<ComAsioDriver>(raw, library);
    }
    const HRESULT result = CoCreateInstance(class_id, nullptr, CLSCTX_INPROC_SERVER,
                                             class_id, reinterpret_cast<void**>(&raw));
    if (FAILED(result) || !raw) {
        error = L"ASIO CoCreateInstance(CLSID, IID=CLSID) failed";
        return {};
    }
    return std::make_unique<ComAsioDriver>(raw);
}

bool ValidSource(const WAVEFORMATEX& f) noexcept {
    return f.wFormatTag == WAVE_FORMAT_PCM && f.nChannels > 0 && f.nChannels <= 64 &&
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
        static_cast<std::int64_t>(std::llround(sample * static_cast<double>(max))), min, max);
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
    std::vector<AsioBufferInfo> buffers;
    std::vector<AsioChannelLayout> layouts;
    std::vector<std::byte> queue;
    size_t read_offset{};
    std::uint32_t capacity{};
    std::uint32_t frames{};
    std::atomic<std::uint64_t> consumed{};
    bool running{};
    bool closing{};
    unsigned callbacks{};
    AsioCallbacks callback_table{};
    HANDLE activity_event{};
    ~State() { if (activity_event) CloseHandle(activity_event); }

    void Consume(long index) noexcept {
        bool output_ready{};
        HANDLE event{};
        {
            std::scoped_lock lock(mutex);
            if (closing || index < 0 || static_cast<size_t>(index) > 1) return;
            const size_t frame_bytes = source.nBlockAlign;
            const size_t available = (queue.size() - read_offset) / frame_bytes;
            const size_t count = std::min<size_t>(frames, available);
            const unsigned input_bytes = source.wBitsPerSample / 8;
            for (size_t channel = 0; channel < buffers.size(); ++channel) {
                auto* out = static_cast<std::byte*>(buffers[channel].buffers[index]);
                if (!out) continue;
                const auto& layout = layouts[channel];
                const size_t output_bytes = layout.container_bits / 8;
                for (size_t frame = 0; frame < count; ++frame) {
                    double sample{};
                    if (channel < source.nChannels || source.nChannels == 1) {
                        const size_t input_channel = source.nChannels == 1 ? 0 : channel;
                        const auto* input = queue.data() + read_offset + frame * frame_bytes + input_channel * input_bytes;
                        sample = ReadPcm(input, source.wBitsPerSample);
                    }
                    WriteSample(out + frame * output_bytes, layout, sample);
                }
                if (count < frames) std::memset(out + count * output_bytes, 0,
                                                (frames - count) * output_bytes);
            }
            const size_t used = count * frame_bytes;
            read_offset += used;
            consumed.fetch_add(used, std::memory_order_relaxed);
            if (read_offset == queue.size()) { queue.clear(); read_offset = 0; }
            else if (read_offset > capacity / 2) {
                queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(read_offset));
                read_offset = 0;
            }
            output_ready = driver != nullptr;
            event = activity_event;
        }
        if (output_ready) static_cast<void>(driver->OutputReady());
        if (event) SetEvent(event);
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
    const GUID& class_id, const WAVEFORMATEX& source, std::uint32_t requested, HWND window) {
    Close(); error_.clear();
    auto driver = ActivateAsio(name, module, class_id, error_);
    return driver && OpenDriver(std::move(driver), source, requested, window);
}

bool AsioSink::OpenDriver(std::unique_ptr<AsioDriver> driver, const WAVEFORMATEX& source,
    std::uint32_t requested, HWND window) {
    Close(); error_.clear();
    if (!driver || !ValidSource(source)) { error_ = L"ASIO requires 16/24/32-bit integer PCM"; return false; }
    auto state = std::make_unique<State>();
    state->driver = std::move(driver); state->source = source;
    state->activity_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->activity_event) { error_ = L"Unable to create ASIO callback event"; return false; }
    if (!state->driver->Init(window)) { error_ = L"IASIO::init failed"; return false; }
    long inputs{}, outputs{}, minimum{}, maximum{}, preferred{}, granularity{};
    if (!state->driver->GetChannels(inputs, outputs) || outputs < 2 || outputs > 64 ||
        !state->driver->GetBufferSize(minimum, maximum, preferred, granularity)) {
        error_ = L"IASIO channel or buffer-size query failed"; return false;
    }
    const auto plan = PlanAsioBuffers(requested, source.nBlockAlign, preferred);
    if (!plan || preferred < minimum || preferred > maximum) { error_ = L"IASIO preferred buffer size is invalid"; return false; }
    if (!state->driver->CanSampleRate(static_cast<double>(source.nSamplesPerSec)) ||
        !state->driver->SetSampleRate(static_cast<double>(source.nSamplesPerSec))) {
        error_ = L"IASIO sample rate is unsupported"; return false;
    }
    // 004E1610 creates max(2, min(source channels, driver outputs)) buffers,
    // not every physical output offered by the driver.  A mono source is
    // deliberately duplicated to the first stereo pair; excess source
    // channels are truncated only when the selected driver exposes fewer.
    const long stream_channels = std::clamp<long>(source.nChannels, 2, outputs);
    state->frames = plan->frames_per_buffer; state->capacity = plan->queued_source_bytes;
    state->buffers.resize(static_cast<size_t>(stream_channels));
    state->layouts.resize(static_cast<size_t>(stream_channels));
    for (long channel = 0; channel < stream_channels; ++channel) {
        auto& info = state->buffers[static_cast<size_t>(channel)];
        info.is_input = 0; info.channel_num = channel;
        AsioChannelInfo channel_info{}; channel_info.channel = channel; channel_info.is_input = 0;
        if (!state->driver->GetChannelInfo(channel_info)) { error_ = L"IASIO output-channel query failed"; return false; }
        const auto layout = ResolveAsioChannelLayout(source.wBitsPerSample, channel_info.type);
        if (!layout) { error_ = L"IASIO output sample type is unsupported"; return false; }
        state->layouts[static_cast<size_t>(channel)] = *layout;
    }
    state->callback_table = {&BufferSwitch, &SampleRateDidChange, &AsioMessage,
                             &BufferSwitchTimeInfo};
    if (!state->driver->CreateBuffers(state->buffers.data(), stream_channels, preferred,
                                      &state->callback_table)) {
        // A driver is permitted to allocate a subset before reporting an
        // error. Match 004E1383's disposal-before-release ordering even on
        // this failed open path.
        static_cast<void>(state->driver->DisposeBuffers());
        error_ = L"IASIO::createBuffers failed"; return false;
    }
    {
        std::scoped_lock lock(g_callback_mutex);
        if (g_active_sink) {
            static_cast<void>(state->driver->DisposeBuffers());
            error_ = L"Only one active ASIO callback owner is supported";
            return false;
        }
        g_active_sink = state.get();
    }
    state_ = std::move(state);
    return true;
}

bool AsioSink::Submit(std::span<const std::byte> source, float left_gain, float right_gain) {
    if (!state_ || source.empty() || source.size() % state_->source.nBlockAlign ||
        !std::isfinite(left_gain) || !std::isfinite(right_gain)) { error_ = L"Invalid ASIO source packet"; return false; }
    std::scoped_lock lock(state_->mutex);
    const size_t pending = state_->queue.size() - state_->read_offset;
    if (source.size() > state_->capacity - pending) { error_ = L"ASIO source queue is full"; return false; }
    // Gain is applied in source PCM before the callback so each driver buffer
    // sees stable frame data.  The callback remains bounded and allocation-free.
    std::vector<std::byte> copy(source.begin(), source.end());
    const unsigned bytes = state_->source.wBitsPerSample / 8;
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
    bool previous{};
    const bool requested_running = !paused;
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->closing) { error_ = L"ASIO stream is closing"; return false; }
        previous = state_->running;
        if (previous == requested_running) return true;
        // Publish the desired clock state before entering a driver.  IASIO
        // stop/start can synchronously wait for bufferSwitch; that callback
        // takes this mutex to move PCM, so never hold it across the call.
        state_->running = requested_running;
    }
    const bool succeeded = paused ? state_->driver->Stop() : state_->driver->Start();
    if (!succeeded) {
        std::scoped_lock lock(state_->mutex);
        if (!state_->closing && state_->running == requested_running)
            state_->running = previous;
        error_ = paused ? L"IASIO::stop failed" : L"IASIO::start failed";
        return false;
    }
    return true;
}

bool AsioSink::Reset() {
    if (!state_) { error_ = L"ASIO stream is not open"; return false; }
    bool was_running{};
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->closing) { error_ = L"ASIO stream is closing"; return false; }
        was_running = state_->running;
        state_->running = false;
        state_->queue.clear(); state_->read_offset = 0;
        state_->consumed.store(0, std::memory_order_relaxed);
    }
    if (was_running && !state_->driver->Stop()) {
        std::scoped_lock lock(state_->mutex);
        if (!state_->closing) state_->running = true;
        error_ = L"IASIO::stop for reset failed";
        return false;
    }
    return true;
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

void AsioSink::BufferSwitch(long index, AsioBool) noexcept {
    State* state{};
    { std::scoped_lock lock(g_callback_mutex); state = static_cast<State*>(g_active_sink); if (!state) return; ++state->callbacks; }
    state->Consume(index);
    { std::scoped_lock lock(g_callback_mutex); if (--state->callbacks == 0) g_callback_idle.notify_all(); }
}
void AsioSink::SampleRateDidChange(double) noexcept {}
long AsioSink::AsioMessage(long, long, void*, double*) noexcept { return 0; }
AsioTime* AsioSink::BufferSwitchTimeInfo(AsioTime*, long index, AsioBool direct) noexcept { BufferSwitch(index, direct); return nullptr; }

} // namespace ttplayer::audio
