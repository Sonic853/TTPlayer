#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <winioctl.h>
// ks.h redefines GUID_NULL as a C++ __uuidof expression.  COM's cguid.h
// must therefore have been parsed first (audio_engine later includes WRL).
#include <objbase.h>
#include <ks.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ttplayer::audio {

// All methods are called on the owning playback worker, never from the UI.
// The interface makes overlapped ownership and stop/reset independently
// testable without opening a kernel driver.
class KsStreamTransport {
public:
    virtual ~KsStreamTransport() = default;
    // These methods return a Win32 result rather than a bool.  Besides being
    // more useful to callers, this makes the packet/state-machine testable
    // with a mock transport: it must not accidentally read thread-local
    // GetLastError() left by an unrelated call.
    virtual DWORD SetState(KSSTATE state) = 0;
    virtual DWORD ResetStream() = 0;
    // Return Win32 ERROR_SUCCESS / ERROR_IO_PENDING / an error code.
    virtual DWORD Submit(KSSTREAM_HEADER& header, OVERLAPPED& request) = 0;
    virtual DWORD Complete(OVERLAPPED& request) = 0;
    virtual void Cancel(OVERLAPPED& request) noexcept = 0;
    virtual std::optional<std::uint64_t> PositionBytes() = 0;
};

// Signed, interleaved 16/24/32-bit PCM. The 16-bit fallback discards the
// least-significant bytes exactly as FUN_004E088A. Gain is applied to the
// owned packet copy because KS pins need not expose a hardware volume node.
[[nodiscard]] bool ConvertKsPcm(std::span<const std::byte> source,
    const WAVEFORMATEX& source_format, unsigned output_bits,
    float left_gain, float right_gain, std::vector<std::byte>& output);

class KernelStreamingSink {
public:
    KernelStreamingSink();
    ~KernelStreamingSink();
    KernelStreamingSink(const KernelStreamingSink&) = delete;
    KernelStreamingSink& operator=(const KernelStreamingSink&) = delete;

    bool OpenDevice(const std::wstring& filter_path,
        const WAVEFORMATEX& source_format, size_t packet_count,
        size_t source_packet_bytes);
    bool OpenTransport(std::unique_ptr<KsStreamTransport> transport,
        const WAVEFORMATEX& source_format, const WAVEFORMATEX& device_format,
        size_t packet_count, size_t source_packet_bytes);
    bool Submit(size_t packet, std::span<const std::byte> source,
        float left_gain = 1.0F, float right_gain = 1.0F);
    // A completed packet becomes writable; pending packets are never reused.
    bool IsComplete(size_t packet);
    bool SetPaused(bool paused);
    bool Reset();
    void Close() noexcept;
    [[nodiscard]] std::uint64_t PositionSourceBytes();
    [[nodiscard]] const WAVEFORMATEX& DeviceFormat() const noexcept;
    [[nodiscard]] const std::wstring& Error() const noexcept { return error_; }

private:
    struct State;
    std::shared_ptr<State> state_;
    std::wstring error_;
};

} // namespace ttplayer::audio
