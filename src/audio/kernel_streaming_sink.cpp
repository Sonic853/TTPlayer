#include "ttplayer/audio/kernel_streaming_sink.h"

#include <ksmedia.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

namespace ttplayer::audio {
namespace {
constexpr GUID kPinSet{0x8c134960,0x51ad,0x11cf,{0x87,0x8a,0x94,0xf8,0x01,0xc1,0,0}};
constexpr GUID kConnectionSet{0x1d58c920,0xac9b,0x11cf,{0xa5,0xd6,0x28,0xdb,0x04,0xc1,0,0}};
constexpr GUID kAudioSet{0x45ffaaa0,0x6e1b,0x11d0,{0xbc,0xf2,0x44,0x45,0x53,0x54,0,0}};
constexpr GUID kAudioType{0x73647561,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
constexpr GUID kPcmType{1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
constexpr GUID kWaveSpecifier{0x05589f81,0xc356,0x11ce,{0xbf,1,0,0xaa,0,0x55,0x59,0x5a}};
constexpr GUID kStreaming{0x1a8766a0,0x62ce,0x11cf,{0xa5,0xd6,0x28,0xdb,4,0xc1,0,0}};
constexpr GUID kDevio{0x4747b320,0x62ce,0x11cf,{0xa5,0xd6,0x28,0xdb,4,0xc1,0,0}};

std::wstring KsError(std::wstring_view operation, DWORD code) {
    return std::wstring(operation) + L" failed (Win32 " + std::to_wstring(code) + L")";
}

// An overlapped property request owns both buffers and a duplicate pin/filter
// handle. A non-cooperative miniport cannot leave pointers to a returned stack.
struct PropertyRequest {
    HANDLE handle{INVALID_HANDLE_VALUE};
    OVERLAPPED request{};
    std::vector<std::byte> input, output;
    ~PropertyRequest() {
        if (request.hEvent) CloseHandle(request.hEvent);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

void RetainPropertyUntilComplete(std::shared_ptr<PropertyRequest> request) noexcept {
    try {
        std::thread([request] {
            WaitForSingleObject(request->request.hEvent, INFINITE);
        }).detach();
    } catch (...) {
        // The kernel still owns these pointers. Retaining one request is safer
        // than freeing memory when even the cancellation reaper cannot start.
        auto* retained = new (std::nothrow) std::shared_ptr<PropertyRequest>(request);
        if (!retained) std::terminate();
    }
}

bool KsIo(HANDLE handle, DWORD control, const void* input, DWORD input_bytes,
          void* output, DWORD output_bytes, DWORD* returned = nullptr) {
    auto io = std::make_shared<PropertyRequest>();
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
            &io->handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
    io->request.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!io->request.hEvent) return false;
    io->input.resize(input_bytes);
    io->output.resize(output_bytes);
    if (input_bytes) std::memcpy(io->input.data(), input, input_bytes);
    // KSPROPERTY_TYPE_SET carries its value in the IOCTL output buffer.
    if (output_bytes && output) std::memcpy(io->output.data(), output, output_bytes);
    DWORD transferred{};
    bool success = DeviceIoControl(io->handle, control,
        input_bytes ? io->input.data() : nullptr, input_bytes,
        output_bytes ? io->output.data() : nullptr, output_bytes,
        &transferred, &io->request) != FALSE;
    DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (!success && error == ERROR_IO_PENDING) {
        if (WaitForSingleObject(io->request.hEvent, 500) == WAIT_OBJECT_0) {
            success = GetOverlappedResult(io->handle, &io->request,
                                          &transferred, FALSE) != FALSE;
            error = success ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(io->handle, &io->request);
            if (WaitForSingleObject(io->request.hEvent, 100) != WAIT_OBJECT_0)
                RetainPropertyUntilComplete(io);
            error = ERROR_TIMEOUT;
        }
    }
    if (success && output && transferred)
        std::memcpy(output, io->output.data(), std::min(transferred, output_bytes));
    if (returned) *returned = transferred;
    SetLastError(error);
    return success;
}

template<class T> bool PinProperty(HANDLE filter, ULONG pin, ULONG id, T& result) {
    KSP_PIN property{};
    property.Property = {kPinSet, id, KSPROPERTY_TYPE_GET};
    property.PinId = pin;
    DWORD returned{};
    return KsIo(filter, IOCTL_KS_PROPERTY, &property, sizeof(property),
                &result, sizeof(result), &returned) && returned >= sizeof(result);
}

bool PinIdentifier(HANDLE filter, ULONG pin, ULONG id, const GUID& set,
                   ULONG expected_identifier) {
    KSP_PIN property{};
    property.Property = {kPinSet, id, KSPROPERTY_TYPE_GET};
    property.PinId = pin;
    std::array<std::byte, 65536> data{};
    DWORD returned{};
    if (!KsIo(filter, IOCTL_KS_PROPERTY, &property, sizeof(property),
              data.data(), static_cast<DWORD>(data.size()), &returned) ||
        returned < sizeof(KSMULTIPLE_ITEM)) return false;
    const auto* list = reinterpret_cast<const KSMULTIPLE_ITEM*>(data.data());
    if (list->Count > (returned - sizeof(*list)) / sizeof(KSIDENTIFIER)) return false;
    const auto* entries = reinterpret_cast<const KSIDENTIFIER*>(list + 1);
    for (ULONG i = 0; i < list->Count; ++i)
        if (entries[i].Id == expected_identifier &&
            IsEqualGUID(entries[i].Set, set)) return true;
    return false;
}

bool ValidPcm(const WAVEFORMATEX& format) {
    if (format.wFormatTag != WAVE_FORMAT_PCM || !format.nChannels ||
        !format.nSamplesPerSec || (format.wBitsPerSample != 16 &&
        format.wBitsPerSample != 24 && format.wBitsPerSample != 32)) return false;
    const std::uint64_t align = std::uint64_t{format.nChannels} * format.wBitsPerSample / 8;
    return align == format.nBlockAlign &&
        align * format.nSamplesPerSec == format.nAvgBytesPerSec;
}

class NativeKsTransport final : public KsStreamTransport {
public:
    explicit NativeKsTransport(HANDLE pin) : pin_(pin) {}
    ~NativeKsTransport() override { CloseHandle(pin_); }
    DWORD SetState(KSSTATE state) override {
        const KSPROPERTY property{kConnectionSet, KSPROPERTY_CONNECTION_STATE,
                                  KSPROPERTY_TYPE_SET};
        if (KsIo(pin_, IOCTL_KS_PROPERTY, &property, sizeof(property),
                 &state, sizeof(state))) return ERROR_SUCCESS;
        return GetLastError();
    }
    DWORD ResetStream() override {
        KSRESET reset = KSRESET_BEGIN;
        const bool begin = KsIo(pin_, IOCTL_KS_RESET_STATE, &reset, sizeof(reset), nullptr, 0);
        const DWORD begin_error = begin ? ERROR_SUCCESS : GetLastError();
        reset = KSRESET_END;
        if (!KsIo(pin_, IOCTL_KS_RESET_STATE, &reset, sizeof(reset), nullptr, 0))
            return GetLastError();
        return begin_error;
    }
    DWORD Submit(KSSTREAM_HEADER& header, OVERLAPPED& request) override {
        DWORD transferred{};
        // FUN_004F0A9E passes the header as the output buffer, not input.
        if (DeviceIoControl(pin_, IOCTL_KS_WRITE_STREAM, nullptr, 0, &header,
                            header.Size, &transferred, &request)) return ERROR_SUCCESS;
        return GetLastError();
    }
    DWORD Complete(OVERLAPPED& request) override {
        DWORD transferred{};
        return GetOverlappedResult(pin_, &request, &transferred, FALSE)
            ? ERROR_SUCCESS : GetLastError();
    }
    void Cancel(OVERLAPPED& request) noexcept override { CancelIoEx(pin_, &request); }
    std::optional<std::uint64_t> PositionBytes() override {
        const KSPROPERTY property{kAudioSet, KSPROPERTY_AUDIO_POSITION, KSPROPERTY_TYPE_GET};
        KSAUDIO_POSITION position{};
        DWORD returned{};
        if (!KsIo(pin_, IOCTL_KS_PROPERTY, &property, sizeof(property),
                  &position, sizeof(position), &returned) || returned < sizeof(position))
            return std::nullopt;
        return position.PlayOffset;
    }
private:
    HANDLE pin_{INVALID_HANDLE_VALUE};
};

std::unique_ptr<KsStreamTransport> CreateNativeTransport(const std::wstring& path,
    const WAVEFORMATEX& source, WAVEFORMATEX& selected, std::wstring& error) {
    if (!ValidPcm(source)) { error = L"KS requires valid 16/24/32-bit integer PCM"; return {}; }
    const HANDLE filter = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (filter == INVALID_HANDLE_VALUE) { error = KsError(L"KS filter open", GetLastError()); return {}; }
    struct FilterCloser { HANDLE h; ~FilterCloser() { CloseHandle(h); } } closer{filter};
    HMODULE library = LoadLibraryExW(L"ksuser.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) { error = KsError(L"ksuser.dll", GetLastError()); return {}; }
    struct ModuleCloser { HMODULE h; ~ModuleCloser() { FreeLibrary(h); } } module{library};
    using CreatePin = DWORD (WINAPI*)(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE);
    const auto create_pin = reinterpret_cast<CreatePin>(GetProcAddress(library, "KsCreatePin"));
    if (!create_pin) { error = L"ksuser.dll has no KsCreatePin"; return {}; }
    const KSPROPERTY property{kPinSet, KSPROPERTY_PIN_CTYPES, KSPROPERTY_TYPE_GET};
    ULONG count{};
    if (!KsIo(filter, IOCTL_KS_PROPERTY, &property, sizeof(property), &count, sizeof(count)) ||
        count > 4096) { error = L"KS filter pin catalog is unavailable"; return {}; }
    std::vector<ULONG> pins;
    for (ULONG pin = 0; pin < count; ++pin) {
        KSPIN_DATAFLOW flow{};
        KSPIN_COMMUNICATION communication{};
        if (PinProperty(filter, pin, KSPROPERTY_PIN_DATAFLOW, flow) && flow == KSPIN_DATAFLOW_IN &&
            PinProperty(filter, pin, KSPROPERTY_PIN_COMMUNICATION, communication) &&
            communication == KSPIN_COMMUNICATION_SINK &&
            PinIdentifier(filter, pin, KSPROPERTY_PIN_INTERFACES, kStreaming,
                          KSINTERFACE_STANDARD_STREAMING) &&
            PinIdentifier(filter, pin, KSPROPERTY_PIN_MEDIUMS, kDevio,
                          KSMEDIUM_TYPE_ANYINSTANCE)) pins.push_back(pin);
    }
    DWORD last_error = ERROR_NOT_SUPPORTED;
    // FUN_004E029E / 004F066E: original-width format variants first, then
    // 16-bit variants. Every attempt stays on this selected native filter.
    for (int fallback = 0; fallback != (source.wBitsPerSample > 16 ? 2 : 1); ++fallback) {
        WAVEFORMATEX format = source;
        format.cbSize = 0;
        if (fallback) {
            format.wBitsPerSample = 16;
            format.nBlockAlign = format.nChannels * 2;
            format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        }
        for (bool extended : {false, true}) {
            for (const ULONG pin : pins) {
                const size_t wave_bytes = extended ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);
                std::vector<std::byte> blob(sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT) + wave_bytes);
                auto* connect = reinterpret_cast<KSPIN_CONNECT*>(blob.data());
                connect->Interface = {kStreaming, KSINTERFACE_STANDARD_STREAMING, 0};
                connect->Medium = {kDevio, KSMEDIUM_TYPE_ANYINSTANCE, 0};
                connect->PinId = pin;
                connect->Priority.PriorityClass = KSPRIORITY_NORMAL;
                auto* data = reinterpret_cast<KSDATAFORMAT*>(connect + 1);
                data->FormatSize = static_cast<ULONG>(sizeof(KSDATAFORMAT) + wave_bytes);
                data->SampleSize = format.nBlockAlign;
                data->MajorFormat = kAudioType;
                data->SubFormat = kPcmType;
                data->Specifier = kWaveSpecifier;
                auto* wave = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(data + 1);
                std::memcpy(&wave->Format, &format, sizeof(format));
                if (extended) {
                    wave->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                    wave->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                    wave->Samples.wValidBitsPerSample = format.wBitsPerSample;
                    wave->dwChannelMask = format.nChannels == 1 ? SPEAKER_FRONT_CENTER :
                        format.nChannels == 2 ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT : 0;
                    wave->SubFormat = kPcmType;
                }
                HANDLE opened{INVALID_HANDLE_VALUE};
                last_error = create_pin(filter, connect, GENERIC_WRITE, &opened);
                if (last_error == ERROR_SUCCESS && opened != INVALID_HANDLE_VALUE) {
                    selected = format;
                    return std::make_unique<NativeKsTransport>(opened);
                }
            }
        }
    }
    error = KsError(L"KsCreatePin (selected filter has no usable PCM render pin)", last_error);
    return {};
}
} // namespace

bool ConvertKsPcm(std::span<const std::byte> source, const WAVEFORMATEX& format,
    unsigned output_bits, float left_gain, float right_gain, std::vector<std::byte>& output) {
    if (!ValidPcm(format) || source.size() % format.nBlockAlign ||
        (output_bits != format.wBitsPerSample && output_bits != 16) ||
        !std::isfinite(left_gain) || !std::isfinite(right_gain)) return false;
    const size_t input_bytes = format.wBitsPerSample / 8;
    const size_t output_bytes = output_bits / 8;
    const size_t samples = source.size() / input_bytes;
    output.resize(samples * output_bytes);
    for (size_t sample = 0; sample < samples; ++sample) {
        std::uint32_t raw{};
        for (size_t byte = 0; byte < input_bytes; ++byte)
            raw |= std::uint32_t{std::to_integer<unsigned char>(source[sample * input_bytes + byte])} << (byte * 8);
        if (input_bytes < 4 && (raw & (1u << (format.wBitsPerSample - 1))))
            raw |= ~((1u << format.wBitsPerSample) - 1);
        std::int64_t value = std::bit_cast<std::int32_t>(raw);
        const float gain = std::clamp(sample % format.nChannels == 1 ? right_gain : left_gain, 0.0F, 1.0F);
        if (gain != 1.0F) value = static_cast<std::int64_t>(std::llround(value * static_cast<double>(gain)));
        value >>= (format.wBitsPerSample - output_bits);
        const auto bits = static_cast<std::uint32_t>(value);
        for (size_t byte = 0; byte < output_bytes; ++byte)
            output[sample * output_bytes + byte] = static_cast<std::byte>((bits >> (byte * 8)) & 255);
    }
    return true;
}

struct KernelStreamingSink::State {
    struct Packet {
        std::vector<std::byte> data;
        KSSTREAM_HEADER header{};
        OVERLAPPED request{};
        size_t source_bytes{};
        bool pending{};
        ~Packet() { if (request.hEvent) CloseHandle(request.hEvent); }
    };
    std::unique_ptr<KsStreamTransport> transport;
    std::vector<std::unique_ptr<Packet>> packets;
    WAVEFORMATEX source{}, device{};
    size_t capacity{};
    std::uint64_t completed_source_bytes{}, submitted_source_bytes{};
    bool paused{true};

    bool Poll(Packet& packet, bool cancelling, DWORD& error) {
        if (!packet.pending) return true;
        const DWORD result = transport->Complete(packet.request);
        if (result == ERROR_IO_INCOMPLETE || result == ERROR_IO_PENDING) return false;
        packet.pending = false;
        if (result != ERROR_SUCCESS && !(cancelling && result == ERROR_OPERATION_ABORTED)) error = result;
        if (result == ERROR_SUCCESS) completed_source_bytes += packet.source_bytes;
        return true;
    }
    bool Drained() {
        bool done = true;
        DWORD ignored{};
        for (auto& packet : packets) done = Poll(*packet, true, ignored) && done;
        return done;
    }
    void Cancel() noexcept {
        for (auto& packet : packets) if (packet->pending) transport->Cancel(packet->request);
    }
};

KernelStreamingSink::KernelStreamingSink() = default;
KernelStreamingSink::~KernelStreamingSink() { Close(); }

bool KernelStreamingSink::OpenDevice(const std::wstring& path, const WAVEFORMATEX& source,
    size_t count, size_t bytes) {
    Close();
    error_.clear();
    WAVEFORMATEX actual{};
    auto transport = CreateNativeTransport(path, source, actual, error_);
    return transport && OpenTransport(std::move(transport), source, actual, count, bytes);
}

bool KernelStreamingSink::OpenTransport(std::unique_ptr<KsStreamTransport> transport,
    const WAVEFORMATEX& source, const WAVEFORMATEX& device, size_t count, size_t bytes) {
    Close();
    error_.clear();
    if (!transport || !ValidPcm(source) || !ValidPcm(device) ||
        source.nChannels != device.nChannels || source.nSamplesPerSec != device.nSamplesPerSec ||
        (device.wBitsPerSample != source.wBitsPerSample && device.wBitsPerSample != 16) ||
        count == 0 || count > 64 || bytes == 0 || bytes > 4 * 1024 * 1024 ||
        bytes % source.nBlockAlign) {
        error_ = L"Invalid KS stream format or packet configuration";
        return false;
    }
    auto state = std::make_shared<State>();
    state->transport = std::move(transport);
    state->source = source;
    state->device = device;
    state->capacity = bytes;
    for (size_t i = 0; i < count; ++i) {
        auto packet = std::make_unique<State::Packet>();
        packet->request.hEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!packet->request.hEvent) { error_ = KsError(L"KS completion event", GetLastError()); return false; }
        packet->data.reserve(bytes / source.nBlockAlign * device.nBlockAlign);
        state->packets.push_back(std::move(packet));
    }
    state_ = std::move(state);
    // 004E029E starts/stops the pin clock before resetting its stream.
    DWORD result = state_->transport->SetState(KSSTATE_RUN);
    if (result == ERROR_SUCCESS) result = state_->transport->SetState(KSSTATE_PAUSE);
    if (result == ERROR_SUCCESS) result = state_->transport->ResetStream();
    if (result != ERROR_SUCCESS) {
        error_ = KsError(L"KS initialize pin state", result);
        Close();
        return false;
    }
    return true;
}

bool KernelStreamingSink::Submit(size_t index, std::span<const std::byte> source,
    float left_gain, float right_gain) {
    if (!state_ || index >= state_->packets.size() || source.empty() || source.size() > state_->capacity) {
        error_ = L"Invalid KS packet submission"; return false;
    }
    auto& packet = *state_->packets[index];
    DWORD error{};
    if (!state_->Poll(packet, false, error) || error != ERROR_SUCCESS) {
        error_ = KsError(L"KS packet is not writable", error ? error : ERROR_IO_PENDING); return false;
    }
    if (!ConvertKsPcm(source, state_->source, state_->device.wBitsPerSample,
                       left_gain, right_gain, packet.data)) {
        error_ = L"KS PCM packet conversion failed"; return false;
    }
    packet.source_bytes = source.size();
    const HANDLE event = packet.request.hEvent;
    packet.request = {};
    packet.request.hEvent = event;
    ResetEvent(event);
    packet.header = {};
    packet.header.Size = sizeof(packet.header);
    packet.header.Data = packet.data.data();
    packet.header.FrameExtent = static_cast<ULONG>(packet.data.size());
    packet.header.DataUsed = packet.header.FrameExtent;
    packet.header.PresentationTime.Numerator = 10000000;
    packet.header.PresentationTime.Denominator = state_->device.nAvgBytesPerSec;
    const DWORD result = state_->transport->Submit(packet.header, packet.request);
    if (result != ERROR_SUCCESS && result != ERROR_IO_PENDING) {
        error_ = KsError(L"IOCTL_KS_WRITE_STREAM", result); return false;
    }
    packet.pending = result == ERROR_IO_PENDING;
    state_->submitted_source_bytes += source.size();
    if (!packet.pending) state_->completed_source_bytes += source.size();
    return true;
}

bool KernelStreamingSink::IsComplete(size_t index) {
    if (!state_ || index >= state_->packets.size()) return false;
    DWORD error{};
    const bool done = state_->Poll(*state_->packets[index], false, error);
    if (error) error_ = KsError(L"KS stream completion", error);
    return done;
}

bool KernelStreamingSink::SetPaused(bool paused) {
    if (!state_) return false;
    if (paused == state_->paused) return true;
    const DWORD result = state_->transport->SetState(paused ? KSSTATE_PAUSE : KSSTATE_RUN);
    if (result != ERROR_SUCCESS) {
        error_ = KsError(L"KS playback state", result); return false;
    }
    state_->paused = paused;
    return true;
}

bool KernelStreamingSink::Reset() {
    if (!state_) return false;
    DWORD result = state_->transport->SetState(KSSTATE_STOP);
    if (result != ERROR_SUCCESS) {
        error_ = KsError(L"KS reset stop", result); return false;
    }
    state_->Cancel();
    const ULONGLONG deadline = GetTickCount64() + 500;
    while (!state_->Drained()) {
        if (GetTickCount64() >= deadline) { error_ = L"KS stream cancellation timed out"; return false; }
        Sleep(1);
    }
    result = state_->transport->SetState(KSSTATE_RUN);
    if (result == ERROR_SUCCESS) result = state_->transport->SetState(KSSTATE_PAUSE);
    if (result == ERROR_SUCCESS) result = state_->transport->ResetStream();
    if (result != ERROR_SUCCESS) {
        error_ = KsError(L"KS reset stream", result); return false;
    }
    state_->paused = true;
    state_->submitted_source_bytes = state_->completed_source_bytes = 0;
    return true;
}

void KernelStreamingSink::Close() noexcept {
    auto state = std::move(state_);
    if (!state) return;
    static_cast<void>(state->transport->SetState(KSSTATE_PAUSE));
    static_cast<void>(state->transport->SetState(KSSTATE_STOP));
    state->Cancel();
    const ULONGLONG deadline = GetTickCount64() + 100;
    while (!state->Drained() && GetTickCount64() < deadline) Sleep(1);
    if (!state->Drained()) {
        // A timed-out driver retains the complete transport/header/data/event
        // owner. No callback captures AudioEngine or a player window.
        try {
            std::thread([state] { while (!state->Drained()) Sleep(20); }).detach();
        } catch (...) {
            auto* retained = new (std::nothrow) std::shared_ptr<State>(state);
            if (!retained) std::terminate();
        }
    }
}

std::uint64_t KernelStreamingSink::PositionSourceBytes() {
    if (!state_) return 0;
    const auto native = state_->transport->PositionBytes();
    if (!native) return state_->completed_source_bytes;
    const std::uint64_t frames = *native / state_->device.nBlockAlign;
    if (frames > state_->submitted_source_bytes / state_->source.nBlockAlign)
        return state_->submitted_source_bytes;
    return frames * state_->source.nBlockAlign;
}

const WAVEFORMATEX& KernelStreamingSink::DeviceFormat() const noexcept {
    static const WAVEFORMATEX empty{};
    return state_ ? state_->device : empty;
}

} // namespace ttplayer::audio
