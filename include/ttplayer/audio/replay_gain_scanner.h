#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include <windows.h>
#include <mmreg.h>

namespace ttplayer::plugins { class PluginManager; }

namespace ttplayer::audio {

enum class ReplayGainScanStatus {
    completed,
    skipped,
    cancelled,
    unsupported,
    decode_error,
    write_error,
};

struct ReplayGainScanResult {
    std::wstring source_identity; // Actual opened CD; checked again before cache writes.
    ReplayGainScanStatus status{ReplayGainScanStatus::decode_error};
    double gain_db{};
    double peak{};
    std::uint64_t decoded_frames{};
    HRESULT result{E_FAIL};
    std::wstring diagnostic;
};

enum class ReplayGainCommitPolicy {
    // CScanGainDlg completion (004A50B5 -> 004C80AC) deliberately removes
    // FILE_ATTRIBUTE_READONLY before opening the metadata writer.
    manual_scan_clear_read_only,
    // The live playback finalizer at 004B1A5C has no corresponding attribute
    // mutation.  A read-only source must therefore remain untouched.
    live_playback_preserve_attributes,
};

using ReplayGainProgress =
    std::function<void(std::uint64_t decoded_frames,
                       std::uint64_t expected_frames)>;

// Called before opening/reading the source and before committing metadata.
// A UI may wait here while paused; false requests cooperative cancellation.
using ReplayGainCheckpoint = std::function<bool()>;

// Snapshot verified reader binaries once per batch. Unknown/replaced plug-ins
// retain serial scanning; matching a filename or extension is not sufficient.
class ReplayGainScanConcurrency {
public:
    ReplayGainScanConcurrency(const plugins::PluginManager& library, HMODULE ttpcomm);
    [[nodiscard]] bool Allows(const std::filesystem::path& path, int subtrack) const;
private:
    struct Format { std::wstring pattern; bool verified{}; };
    std::vector<Format> formats_;
    bool analyzer_verified_{};
    [[nodiscard]] bool AllowsFile(const std::filesystem::path& path) const;
};

// Ordinals 100/101 are the exact sample-rate predicate and ReplayGain object
// used by CScanGainDlg::CWorkThread::Run (004A5251).  This query only verifies
// that the two entry points exist; individual rates are checked when a reader
// has supplied its WAVEFORMATEX.
[[nodiscard]] bool LegacyReplayGainAvailable(HMODULE ttpcomm) noexcept;

// The manual scanner worker at 004A5251 and completion handler at 004A50B5
// are deliberately separate in the original.  Preserve that boundary for
// the modeless scan dialog; live playback analysis uses the stream wrapper
// below and does not open a second decoder.
[[nodiscard]] ReplayGainScanResult AnalyzeReplayGainTrack(
    const plugins::PluginManager& library, HMODULE ttpcomm,
    const std::filesystem::path& path, bool skip_existing,
    std::stop_token stop = {}, ReplayGainProgress progress = {}, int subtrack = 0,
    ReplayGainCheckpoint checkpoint = {},
    const ReplayGainScanConcurrency* concurrency = nullptr);

[[nodiscard]] ReplayGainScanResult CommitReplayGainTrack(
    const plugins::PluginManager& library,
    const std::filesystem::path& path,
    const ReplayGainScanResult& analysis,
    ReplayGainCommitPolicy policy,
    std::stop_token stop = {}, int subtrack = 0,
    ReplayGainCheckpoint checkpoint = {});

// FUN_004B107E creates ordinal 101 beside the playback processor and
// FUN_004B1950 feeds it the already-decoded double PCM before ReplayGain/EQ.
// This wrapper owns an extra ttpcomm module reference so an in-flight audio
// worker never calls through an unloaded private vtable.
class PlaybackReplayGainAnalyzer {
public:
    ~PlaybackReplayGainAnalyzer();
    PlaybackReplayGainAnalyzer(const PlaybackReplayGainAnalyzer&) = delete;
    PlaybackReplayGainAnalyzer& operator=(
        const PlaybackReplayGainAnalyzer&) = delete;

    [[nodiscard]] static std::unique_ptr<PlaybackReplayGainAnalyzer> Create(
        HMODULE ttpcomm, DWORD sample_rate, WORD channels) noexcept;
    // Interleaved, finite source samples, with integer PCM full scale at 1.0.
    [[nodiscard]] bool Analyze(
        const double* samples, size_t sample_count) noexcept;
    // Shares the manual scanner's full-scale conversion. Feed source PCM
    // before gain/EQ and before private processor-domain scaling.
    [[nodiscard]] bool AnalyzePcm(
        const std::vector<std::byte>& bytes, const WAVEFORMATEX& format) noexcept;
    [[nodiscard]] std::optional<ReplayGainScanResult> Finish() noexcept;
    void Cancel() noexcept;

private:
    struct Impl;
    explicit PlaybackReplayGainAnalyzer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Queue the 004B1A5C metadata transaction after the playback source object
// has been destroyed.  The retained Sound AddIn registry makes the detached
// writer independent from PlayerWindow shutdown.  A playback reader opens
// with STGM_SHARE_DENY_WRITE, so a bounded set of sharing retries happens off
// the UI thread and live decoder input is never modified concurrently.
void QueueReplayGainCommit(
    std::shared_ptr<plugins::PluginManager> retained_library,
    std::filesystem::path path, ReplayGainScanResult analysis, int subtrack = 0) noexcept;

// Decode one physical sound file through the registered private reader ABI,
// feed normalized double samples to ttpcomm ordinal 101, then store the two
// canonical lower-case tags through ISoundMetadata slot 6.  Work is entirely
// synchronous at this boundary so callers can own cancellation/threading.
[[nodiscard]] ReplayGainScanResult ScanReplayGainTrack(
    const plugins::PluginManager& library, HMODULE ttpcomm,
    const std::filesystem::path& path, bool skip_existing,
    std::stop_token stop = {}, ReplayGainProgress progress = {}, int subtrack = 0,
    ReplayGainCheckpoint checkpoint = {});

} // namespace ttplayer::audio
