#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>
#include <mmsystem.h>

struct IDirectSoundBuffer8;

namespace ttplayer::plugins { class PluginManager; }

namespace ttplayer::audio {
class WinampDspChain;
enum class PlaybackState { stopped, opening, playing, paused, failed };

// CSound::ReadThreadProc (004AC605) uses FadeDuration[0] for an ordinary
// play/resume transition and FadeDuration[2] only when its seek-transition
// flag is set.  Keep the recovered offset choice independently testable.
[[nodiscard]] constexpr size_t RecoveredFadeInDurationIndex(
    bool seek_transition) noexcept {
    return seek_transition ? 2U : 0U;
}

// The original generic seek arms its transition only after additional
// private fade-helper, volume and queued-audio checks at 004ABDE4.  The
// reconstruction cannot observe that private buffer contract, so callers
// whose captured original behavior is a direct seek may explicitly bypass
// the otherwise broader reconstructed gate.
[[nodiscard]] constexpr bool ShouldBeginRecoveredSeekFade(
    bool allow_fade, int sound_fade_mode, int duration_ms,
    PlaybackState state, bool native_output_supports_fade) noexcept {
    return allow_fade && state == PlaybackState::playing &&
           (sound_fade_mode & 0x04) != 0 && duration_ms > 0 &&
           native_output_supports_fade;
}

// FUN_0045B78E creates CSoundFadeOut only for an actively playing native
// wave/DirectSound output when SoundFadeMode bit 3 and FadeDuration[3] are
// both enabled. Keep the close-path gate independently testable.
[[nodiscard]] constexpr bool ShouldBeginRecoveredStopFade(
    int sound_fade_mode, int duration_ms, PlaybackState state,
    bool native_output_supports_fade) noexcept {
    return state == PlaybackState::playing &&
           (sound_fade_mode & 0x08) != 0 && duration_ms > 0 &&
           native_output_supports_fade;
}

// CPlayerWnd::OnDestroy (00461ADB) gives CSoundFadeOut an additional 500 ms
// after FadeDuration[3] before forcing teardown.
[[nodiscard]] constexpr std::uint64_t RecoveredStopFadeCloseBudgetMs(
    int duration_ms) noexcept {
    return static_cast<std::uint64_t>(duration_ms > 0 ? duration_ms : 0) +
           500U;
}

struct AudioFormat {
    uint16_t format_tag{};
    uint16_t channels{};
    uint32_t sample_rate{};
    uint32_t bytes_per_second{};
    uint16_t block_align{};
    uint16_t bits_per_sample{};
    std::wstring codec_name;
};

struct AudioMetadata {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::optional<double> replay_gain_db;
    std::optional<double> replay_peak;
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    bool thumbnail_interface{};
    std::vector<unsigned char> thumbnail;
};

// The original visualization pipeline asks the sound player for two blocks
// of 512 signed 16-bit samples.  Keep that ABI-shaped snapshot at the audio
// boundary so UI renderers never have to inspect decoder or waveOut buffers.
struct VisualizationSamples {
    static constexpr size_t capacity = 512;

    std::array<int16_t, capacity> left{};
    std::array<int16_t, capacity> right{};
    size_t count{};
    uint32_t sample_rate{};
    uint64_t revision{};
};

// TTPlayer.xml /Playback and /Device values consumed by the recovered sound
// pipeline.  Keeping them at the engine boundary prevents UI code from
// silently substituting hard-coded buffering and output-format defaults.
struct PlaybackOptions {
    int file_buffer_bytes{16384};
    int output_buffer_ms{1000};
    int output_bits{16};
    int resample_rate{};
    int thread_priority{15};
    int balance{};
    bool auto_gain{};
    bool auto_scan_gain{};
    bool skip_scan_gain{};
    int equalizer_profile{-2};
    int surround{};
    std::array<int, 11> equalizer_values{};
    // CSettings::Device/@DeviceType is the textual form of the original
    // 16-byte output key.  A zero-tail key uses Data1.low as waveOut id + 1
    // and Data1.high as the backend discriminator (0 waveOut, 2 KS, 3 ASIO);
    // a non-zero tail denotes DirectSound.  Empty keeps the reconstruction's
    // backwards-compatible WAVE_MAPPER default.
    std::wstring device_type;
    bool hardware_buffer{true};
    bool create_primary{};
    int ssrc_mode{1};
    int dither{};
    // CSettings::Plugin retains an ordered list of enabled Winamp DSP DLLs.
    // Relative Modules_N values resolve against Folder; the window handle is
    // runtime-only and becomes winampDSPModule::hwndParent.
    std::filesystem::path dsp_folder;
    std::vector<std::wstring> dsp_modules;
    HWND dsp_parent_window{};
    // CSound::ReadThreadProc at 004AC605 tests bits 0..4 independently:
    // play, pause, seek, explicit stop and natural track-end fade.
    int sound_fade_mode{15};
    std::array<int, 4> fade_duration{300, 500, 800, 800};
    int track_fade_duration{5000};
};

// Read-only decoded PCM boundary shared by playback and offline conversion.
// Implementations may be backed by a Sound AddIn reader/decoder, Media
// Foundation, the built-in AIFF/AU parser, or the raw CD-DA reader.  The
// source owns the returned WAVEFORMATEX object; callers must keep the source
// alive while inspecting cbSize's WAVEFORMATEXTENSIBLE tail.
class DecodedAudioSource {
public:
    virtual ~DecodedAudioSource() = default;
    virtual bool Open(const std::filesystem::path& path,
                      const PlaybackOptions& options) = 0;
    virtual bool Read(size_t requested_bytes,
                      std::vector<std::byte>& output,
                      bool& end_of_stream) = 0;
    virtual bool Seek(std::chrono::milliseconds position) = 0;
    [[nodiscard]] virtual const WAVEFORMATEX& OutputFormat() const = 0;
    [[nodiscard]] virtual AudioFormat DisplayFormat() const = 0;
    [[nodiscard]] virtual std::chrono::milliseconds Duration() const = 0;
    [[nodiscard]] virtual std::wstring Error() const = 0;
    [[nodiscard]] virtual AudioMetadata Metadata() const { return {}; }
};

// Creates (but does not open) the recovered source selected for `path`.
// A positive subtrack selects a CUE entry when path names a CUE sheet.
// Media Foundation sources require their calling thread to have initialized
// COM and Media Foundation, matching CSound/CConvertDlg worker lifetimes.
[[nodiscard]] std::unique_ptr<DecodedAudioSource> CreateDecodedAudioSource(
    const std::filesystem::path& path, int subtrack,
    const plugins::PluginManager* plugin_manager, HMODULE ttpcomm_module);

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Play(const std::filesystem::path& path, int subtrack = 0);
    bool PlayWave(const std::filesystem::path& path) { return Play(path); }
    // Device-page changes reopen the current source with a new output object.
    // Restore the old clock/state without routing through the user-facing
    // seek/pause fades a second time.
    void RestoreAfterOutputRestart(std::chrono::milliseconds position,
                                   bool paused);
    void Pause();
    void Resume();
    void Seek(std::chrono::milliseconds position);
    // CLyricCtrl's drag-release callback (00442671) enters generic seek, but
    // the supplied original runtime does not pass 004ABDE4's extra private
    // fade-helper/buffer gate for this interaction. Preserve that observable
    // result without changing seek fades for the main playback controls.
    void SeekWithoutFade(std::chrono::milliseconds position);
    void Stop();
    // Explicit UI stop follows bit 3 of SoundFadeMode.  Stop() remains the
    // immediate lifetime/cancellation primitive used before Play and during
    // destruction.
    void StopWithFade();
    // Close starts the same fade but must not synchronously stop when the
    // option/backend/state gate is false. The window message loop polls the
    // lock-free flag while continuing to dispatch paint/timer messages.
    [[nodiscard]] bool BeginStopFade();
    [[nodiscard]] bool StopFadePending() const noexcept {
        return stop_fade_pending_.load(std::memory_order_acquire);
    }
    void Configure(const PlaybackOptions& options);
    void SetPluginManager(const plugins::PluginManager* manager) noexcept {
        plugin_manager_ = manager;
    }
    void SetTtpCommModule(HMODULE module) noexcept { ttpcomm_module_ = module; }
    void SetDspParentWindow(HWND window);
    void SetVolume(float volume);
    void SetBalance(int balance);
    void SetEqualizer(int profile, int surround,
                      const std::array<int, 11>& values);
    [[nodiscard]] static const std::wstring& RecoveredFilePattern();
    [[nodiscard]] static bool IsRecoveredFormatPath(const std::filesystem::path& path);
    [[nodiscard]] static bool IsNetworkMediaLocation(
        const std::filesystem::path& path) noexcept;
    [[nodiscard]] PlaybackState State() const noexcept { return state_.load(); }
    [[nodiscard]] std::chrono::milliseconds Position() const noexcept {
        return std::chrono::milliseconds(position_ms_.load());
    }
    [[nodiscard]] std::chrono::milliseconds Duration() const noexcept {
        return std::chrono::milliseconds(duration_ms_.load());
    }
    [[nodiscard]] std::wstring LastError() const;
    // Successful fallback is intentionally separate from LastError: choosing
    // an unavailable/unsupported legacy backend must not make playback look
    // failed, but callers and diagnostic probes still need an audit trail.
    [[nodiscard]] std::wstring LastDiagnostic() const;
    [[nodiscard]] AudioFormat Format() const;
    [[nodiscard]] AudioMetadata Metadata() const;
    [[nodiscard]] VisualizationSamples Visualization() const;

private:
    enum class Backend { none, wave_out, direct_sound, kernel_streaming, asio, mci };
    enum class FadeCompletion { none, pause, seek, stop };
    void PlaybackWorker(std::filesystem::path path, int subtrack);
    void WaveOutWorker(const std::filesystem::path& path, int subtrack);
    void MciWorker(const std::filesystem::path& path);
    [[nodiscard]] bool PublishOpened(Backend backend, const AudioFormat& format,
                                     std::chrono::milliseconds duration);
    void SignalOpenComplete();
    void RequestStop() noexcept;
    [[nodiscard]] bool ReapWorker(std::chrono::milliseconds timeout) noexcept;
    void SeekImpl(std::chrono::milliseconds position, bool allow_fade);
    void ApplyVolumeLocked();
    void QueueFadeLocked(float target, std::chrono::milliseconds duration,
                         FadeCompletion completion,
                         int64_t seek_target_ms = -1);
    void FadeWorker(std::stop_token stop_token);
    void UpdateTrackFade(int64_t position_ms);
    void PublishVisualization(VisualizationSamples samples);
    void ClearVisualization();
    void RecordDiagnostic(std::wstring message);
    void SetError(std::wstring message);

    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<int64_t> position_ms_{};
    std::atomic<int64_t> duration_ms_{};
    std::atomic<int64_t> seek_request_ms_{-1};
    std::atomic<bool> stop_requested_{true};
    mutable std::mutex mutex_;
    std::condition_variable open_condition_;
    std::condition_variable fade_condition_;
    bool open_complete_{};
    std::wstring error_;
    std::wstring diagnostic_;
    AudioFormat format_{};
    AudioMetadata metadata_{};
    mutable std::mutex visualization_mutex_;
    VisualizationSamples visualization_{};
    PlaybackOptions options_{};
    std::atomic<uint64_t> processor_revision_{};
    std::atomic<Backend> backend_{Backend::none};
    HWAVEOUT device_{};
    IDirectSoundBuffer8* direct_sound_buffer_{};
    MCIDEVICEID mci_device_{};
    HANDLE completion_event_{};
    std::jthread worker_;
    std::jthread fade_worker_;
    float volume_{1.0F};
    float transition_gain_{1.0F};
    float track_gain_{1.0F};
    bool fade_pending_{};
    std::atomic<bool> stop_fade_pending_{};
    bool fade_shutdown_{};
    uint64_t fade_generation_{};
    float fade_from_{1.0F};
    float fade_target_{1.0F};
    std::chrono::steady_clock::time_point fade_started_{};
    std::chrono::milliseconds fade_duration_{};
    FadeCompletion fade_completion_{FadeCompletion::none};
    int64_t fade_seek_target_ms_{-1};
    int balance_{};
    const plugins::PluginManager* plugin_manager_{}; // borrowed for app session
    HMODULE ttpcomm_module_{}; // borrowed from TtpCommRuntime
    // Kept for the full AudioEngine lifetime, as DAT_00546C64 is in the
    // original. DSP Init/Quit therefore does not repeat at every track.
    std::unique_ptr<WinampDspChain> dsp_chain_;
};
}
