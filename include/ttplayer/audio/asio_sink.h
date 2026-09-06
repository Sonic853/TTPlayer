#pragma once

#include "ttplayer/audio/native_output_contract.h"

#include <windows.h>
#include <mmsystem.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ttplayer::audio {

// These are the x86 ASIO SDK ABI records used by TTPlayer's private IASIO
// vtable.  Do not substitute IAudioClient/WASAPI: the selected driver owns
// these buffers and invokes this callback from its own realtime thread.
using AsioBool = long;
struct AsioTime;
struct AsioCallbacks {
    void (*buffer_switch)(long double_buffer_index, AsioBool direct_process){};
    void (*sample_rate_did_change)(double sample_rate){};
    long (*asio_message)(long selector, long value, void* message, double* opt){};
    AsioTime* (*buffer_switch_time_info)(AsioTime* parameters,
                                         long double_buffer_index,
                                         AsioBool direct_process){};
};
struct AsioBufferInfo {
    AsioBool is_input{};
    long channel_num{};
    void* buffers[2]{};
};
struct AsioChannelInfo {
    long channel{};
    AsioBool is_input{};
    AsioBool is_active{};
    long channel_group{};
    long type{};
    char name[32]{};
};

// Adapter seam for the private IASIO interface.  Calls correspond exactly to
// x86 vtable offsets +0x0C, +0x1C, +0x20, +0x24, +0x2C, +0x30, +0x38,
// +0x48, +0x4C, +0x50 and +0x5C in TTPlayer.exe.
class AsioDriver {
public:
    virtual ~AsioDriver() = default;
    virtual bool Init(HWND window) = 0;
    virtual bool Start() = 0;
    virtual bool Stop() = 0;
    virtual bool GetChannels(long& inputs, long& outputs) = 0;
    virtual bool GetBufferSize(long& minimum, long& maximum, long& preferred,
                               long& granularity) = 0;
    virtual bool CanSampleRate(double sample_rate) = 0;
    virtual bool SetSampleRate(double sample_rate) = 0;
    virtual bool GetChannelInfo(AsioChannelInfo& info) = 0;
    virtual bool CreateBuffers(AsioBufferInfo* buffers, long count, long frames,
                               AsioCallbacks* callbacks) = 0;
    virtual bool DisposeBuffers() = 0;
    virtual bool OutputReady() = 0;
};

// FUN_004E207B fills the four cached fields displayed by the output-device
// options page.  The sample-rate table is DAT_00541714 verbatim; ASIOError
// ASE_OK is zero, so only rates for which canSampleRate returns zero appear.
inline constexpr std::array<long, 13> kAsioCapabilitySampleRates{
    8000, 11025, 16000, 22050, 24000, 32000, 44100,
    48000, 64000, 88200, 96000, 176400, 192000};

struct AsioDriverCapabilities {
    long output_channels{};
    std::vector<long> supported_sample_rates;
    int first_output_valid_bits{};
    long preferred_buffer_frames{};
};

// Read-only options-page probe.  This deliberately stops before setSampleRate,
// createBuffers or start: FUN_004E207B only initializes the driver, queries
// channels/buffer size/channel 0, and runs the fixed canSampleRate table.
[[nodiscard]] bool QueryAsioDriverCapabilities(
    AsioDriver& driver, AsioDriverCapabilities& capabilities,
    HWND window = nullptr) noexcept;

// Same-bitness activation used by the isolated output-device helper.  Generic
// drivers use CLSID as both class and interface ID; the named Kernel-Streaming
// ASIO shim follows the original DllGetClassObject exception.
[[nodiscard]] bool ProbeAsioDriverCapabilities(
    const std::wstring& name, const std::wstring& module,
    const GUID& class_id, AsioDriverCapabilities& capabilities,
    std::wstring& error, HWND window = nullptr) noexcept;

class AsioSink {
public:
    AsioSink();
    ~AsioSink();
    AsioSink(const AsioSink&) = delete;
    AsioSink& operator=(const AsioSink&) = delete;

    // Opens the registry-selected IASIO driver. Generic drivers use the
    // original CoCreateInstance(CLSID, ..., IID=CLSID) form; the named
    // ASIO Kernel-Streaming driver uses its DllGetClassObject exception.
    bool OpenDevice(const std::wstring& name, const std::wstring& module,
                    const GUID& class_id, const WAVEFORMATEX& source_format,
                    std::uint32_t requested_source_bytes, HWND window = nullptr);
    bool OpenDriver(std::unique_ptr<AsioDriver> driver,
                    const WAVEFORMATEX& source_format,
                    std::uint32_t requested_source_bytes, HWND window = nullptr);
    // Queue signed interleaved PCM. A callback consumes whole source frames,
    // converts each output channel, and zero-fills an underrun period.
    bool Submit(std::span<const std::byte> source, float left_gain = 1.0F,
                float right_gain = 1.0F);
    bool SetPaused(bool paused);
    bool Reset();
    void Close() noexcept;
    [[nodiscard]] std::uint32_t PeriodFrames() const noexcept;
    [[nodiscard]] std::uint32_t CapacitySourceBytes() const noexcept;
    [[nodiscard]] std::uint64_t ConsumedSourceBytes() const noexcept;
    // Auto-reset event signalled after each driver callback so the owning
    // decoder worker can refill promptly without polling at device-period
    // granularity.
    [[nodiscard]] HANDLE ActivityEvent() const noexcept;
    [[nodiscard]] const std::wstring& Error() const noexcept { return error_; }

private:
    struct State;
    static void BufferSwitch(long index, AsioBool direct_process) noexcept;
    static void SampleRateDidChange(double) noexcept;
    static long AsioMessage(long, long, void*, double*) noexcept;
    static AsioTime* BufferSwitchTimeInfo(AsioTime*, long, AsioBool) noexcept;
    std::unique_ptr<State> state_;
    std::wstring error_;
};

} // namespace ttplayer::audio
