#include "ttplayer/audio/replay_gain_scanner.h"

#include "ttplayer/plugins/plugin_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <mmreg.h>
#include <mutex>
#include <objbase.h>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace ttplayer::audio {
namespace {

using SupportsRate = BOOL(__cdecl*)(DWORD);
using CreateAnalyzer = void*(__cdecl*)();
using DestroyAnalyzer = void(__thiscall*)(void*, BYTE);
using InitializeAnalyzer = bool(__thiscall*)(void*, DWORD);
using AnalyzeSamples = bool(__thiscall*)(void*, const double*, DWORD, DWORD);
using FinishTrack = double(__thiscall*)(void*);
using ReadPeak = double(__thiscall*)(void*);

void** Vtable(void* object) noexcept {
    return object ? *static_cast<void***>(object) : nullptr;
}

BOOL InvokeSupportsRate(FARPROC entry, DWORD rate) noexcept {
    if (!entry) return FALSE;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<SupportsRate>(entry)(rate);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
#else
    return reinterpret_cast<SupportsRate>(entry)(rate);
#endif
}

void* InvokeCreate(FARPROC entry) noexcept {
    if (!entry) return nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<CreateAnalyzer>(entry)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    return reinterpret_cast<CreateAnalyzer>(entry)();
#endif
}

bool InvokeInitialize(void* object, DWORD rate) noexcept {
    auto table = Vtable(object);
    if (!table || !table[1]) return false;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<InitializeAnalyzer>(table[1])(object, rate);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return reinterpret_cast<InitializeAnalyzer>(table[1])(object, rate);
#endif
}

bool InvokeAnalyze(void* object, const double* samples, DWORD channels,
                   DWORD frames) noexcept {
    auto table = Vtable(object);
    if (!table || !table[2] || !samples || channels == 0) return false;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<AnalyzeSamples>(table[2])(
            object, samples, channels, frames);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return reinterpret_cast<AnalyzeSamples>(table[2])(
        object, samples, channels, frames);
#endif
}

bool InvokeFinish(void* object, double* gain, double* peak) noexcept {
    auto table = Vtable(object);
    if (!table || !table[3] || !table[4] || !gain || !peak) return false;
#if defined(_MSC_VER)
    __try {
        // Slot 3 merges the current 12,000-bin histogram into the album
        // histogram, resets the current-track filters and returns the track
        // gain in ST(0). Slot 4 reads the absolute peak accumulated by slot 2.
        *gain = reinterpret_cast<FinishTrack>(table[3])(object);
        *peak = reinterpret_cast<ReadPeak>(table[4])(object);
        return std::isfinite(*gain) && std::isfinite(*peak);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *gain = reinterpret_cast<FinishTrack>(table[3])(object);
    *peak = reinterpret_cast<ReadPeak>(table[4])(object);
    return std::isfinite(*gain) && std::isfinite(*peak);
#endif
}

void InvokeDestroy(void* object) noexcept {
    auto table = Vtable(object);
    if (!table || !table[0]) return;
#if defined(_MSC_VER)
    __try {
        reinterpret_cast<DestroyAnalyzer>(table[0])(object, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    reinterpret_cast<DestroyAnalyzer>(table[0])(object, 1);
#endif
}

class Analyzer {
public:
    Analyzer(HMODULE module, DWORD sample_rate) noexcept {
        if (!module) return;
        const FARPROC supports = GetProcAddress(module, MAKEINTRESOURCEA(100));
        const FARPROC create = GetProcAddress(module, MAKEINTRESOURCEA(101));
        if (!InvokeSupportsRate(supports, sample_rate)) return;
        object_ = InvokeCreate(create);
        if (object_ && !InvokeInitialize(object_, sample_rate)) {
            InvokeDestroy(object_);
            object_ = nullptr;
        }
    }
    ~Analyzer() { InvokeDestroy(object_); }
    Analyzer(const Analyzer&) = delete;
    Analyzer& operator=(const Analyzer&) = delete;

    explicit operator bool() const noexcept { return object_ != nullptr; }
    bool Analyze(const double* samples, DWORD channels, DWORD frames) noexcept {
        return InvokeAnalyze(object_, samples, channels, frames);
    }
    bool Finish(double* gain, double* peak) noexcept {
        return InvokeFinish(object_, gain, peak);
    }

private:
    void* object_{};
};

bool ExistingGain(const plugins::LegacyReaderSession& reader) {
    const auto gain = reader.MetadataValue("replaygain_track_gain");
    const auto peak = reader.MetadataValue("replaygain_track_peak");
    return gain && peak && !gain->empty() && !peak->empty();
}

bool ConvertSamples(const std::vector<std::byte>& bytes,
                    const WAVEFORMATEX& format,
                    std::vector<double>& samples, DWORD* frames) {
    if (!frames || format.nChannels == 0 || format.nBlockAlign == 0 ||
        bytes.size() % format.nBlockAlign != 0)
        return false;
    const size_t frame_count = bytes.size() / format.nBlockAlign;
    if (frame_count > std::numeric_limits<DWORD>::max() ||
        frame_count > std::numeric_limits<size_t>::max() / format.nChannels)
        return false;
    const size_t sample_count = frame_count * format.nChannels;
    samples.resize(sample_count);
    const auto* source = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t sample_bytes = (format.wBitsPerSample + 7U) / 8U;
    if (sample_bytes == 0 ||
        static_cast<size_t>(format.nChannels) * sample_bytes >
            format.nBlockAlign)
        return false;

    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
        format.wBitsPerSample == 64) {
        for (size_t frame{}; frame < frame_count; ++frame) {
            for (size_t channel{}; channel < format.nChannels; ++channel) {
                double value{};
                std::memcpy(&value,
                    source + frame * format.nBlockAlign + channel * 8, 8);
                samples[frame * format.nChannels + channel] =
                    std::isfinite(value) ? std::clamp(value, -1.0, 1.0) : 0.0;
            }
        }
    } else if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
               format.wBitsPerSample == 32) {
        for (size_t frame{}; frame < frame_count; ++frame) {
            for (size_t channel{}; channel < format.nChannels; ++channel) {
                float value{};
                std::memcpy(&value,
                    source + frame * format.nBlockAlign + channel * 4, 4);
                samples[frame * format.nChannels + channel] =
                    std::isfinite(value)
                        ? std::clamp(static_cast<double>(value), -1.0, 1.0)
                        : 0.0;
            }
        }
    } else if (format.wFormatTag == WAVE_FORMAT_PCM) {
        for (size_t frame{}; frame < frame_count; ++frame) {
            for (size_t channel{}; channel < format.nChannels; ++channel) {
                const auto* value = source + frame * format.nBlockAlign +
                                    channel * sample_bytes;
                double normalized{};
                if (format.wBitsPerSample == 8) {
                    normalized = (static_cast<int>(*value) - 128) / 128.0;
                } else if (format.wBitsPerSample == 16) {
                    std::int16_t integer{};
                    std::memcpy(&integer, value, sizeof(integer));
                    normalized = integer / 32768.0;
                } else if (format.wBitsPerSample == 24) {
                    std::int32_t integer = static_cast<std::int32_t>(value[0]) |
                        (static_cast<std::int32_t>(value[1]) << 8) |
                        (static_cast<std::int32_t>(value[2]) << 16);
                    if ((integer & 0x00800000) != 0) integer |= ~0x00ffffff;
                    normalized = integer / 8388608.0;
                } else if (format.wBitsPerSample == 32) {
                    std::int32_t integer{};
                    std::memcpy(&integer, value, sizeof(integer));
                    normalized = integer / 2147483648.0;
                } else {
                    return false;
                }
                samples[frame * format.nChannels + channel] = normalized;
            }
        }
    } else {
        return false;
    }
    *frames = static_cast<DWORD>(frame_count);
    return true;
}

std::wstring FormatGain(double gain) {
    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(2) << gain << L" dB";
    return text.str();
}

std::wstring FormatPeak(double peak) {
    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(6) << peak;
    return text.str();
}

ReplayGainScanResult Error(ReplayGainScanStatus status, HRESULT result,
                           std::wstring diagnostic) {
    ReplayGainScanResult value;
    value.status = status;
    value.result = result;
    value.diagnostic = std::move(diagnostic);
    return value;
}

HMODULE RetainModule(HMODULE module) noexcept {
    if (!module) return nullptr;
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return nullptr;
    path.resize(length);
    return LoadLibraryW(path.c_str());
}

bool IsTransientWriterCollision(HRESULT result) noexcept {
    return result == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
           result == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
           result == STG_E_SHAREVIOLATION || result == STG_E_LOCKVIOLATION;
}

struct PendingCommitRegistry {
    std::mutex mutex;
    std::set<std::wstring> paths;
};

PendingCommitRegistry& CommitRegistry() {
    // Background metadata writers may outlive application object teardown.
    // A process-lifetime registry avoids a static-destructor race without
    // imposing a shutdown join on PlayerWindow.
    static auto* registry = new PendingCommitRegistry;
    return *registry;
}

std::wstring CommitKey(const std::filesystem::path& path) {
    auto key = path.lexically_normal().make_preferred().wstring();
    if (!key.empty())
        CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}

void ReleaseCommitKey(const std::wstring& key) noexcept {
    auto& registry = CommitRegistry();
    const std::scoped_lock lock(registry.mutex);
    registry.paths.erase(key);
}

} // namespace

bool LegacyReplayGainAvailable(HMODULE ttpcomm) noexcept {
    return ttpcomm &&
        GetProcAddress(ttpcomm, MAKEINTRESOURCEA(100)) &&
        GetProcAddress(ttpcomm, MAKEINTRESOURCEA(101));
}

ReplayGainScanResult AnalyzeReplayGainTrack(
    const plugins::PluginManager& library, HMODULE ttpcomm,
    const std::filesystem::path& path, bool skip_existing,
    std::stop_token stop, ReplayGainProgress progress) {
    if (!LegacyReplayGainAvailable(ttpcomm))
        return Error(ReplayGainScanStatus::unsupported, E_NOINTERFACE,
                     L"ttpcomm ReplayGain ordinals 100/101 are unavailable");
    if (path.empty())
        return Error(ReplayGainScanStatus::unsupported, E_INVALIDARG,
                     L"empty source path");
    if (stop.stop_requested())
        return Error(ReplayGainScanStatus::cancelled,
                     HRESULT_FROM_WIN32(ERROR_CANCELLED), L"cancelled");

    HRESULT opened{};
    std::wstring diagnostic;
    auto reader = library.OpenReader(path, &opened, &diagnostic);
    if (!reader)
        return Error(ReplayGainScanStatus::unsupported, opened,
                     diagnostic.empty() ? L"no decoder opened the source"
                                        : std::move(diagnostic));
    if (skip_existing && ExistingGain(*reader)) {
        auto result = Error(ReplayGainScanStatus::skipped, S_FALSE,
                            L"existing ReplayGain tags retained");
        return result;
    }

    const WAVEFORMATEX format = reader->Format();
    Analyzer analyzer(ttpcomm, format.nSamplesPerSec);
    if (!analyzer)
        return Error(ReplayGainScanStatus::unsupported,
                     HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                     L"sample rate is not supported by ttpcomm ordinal 101");

    const std::uint64_t expected_frames = format.nSamplesPerSec &&
        reader->DurationMilliseconds()
        ? static_cast<std::uint64_t>(reader->DurationMilliseconds()) *
              format.nSamplesPerSec / 1000U
        : 0;
    std::vector<std::byte> bytes;
    std::vector<double> samples;
    std::uint64_t decoded_frames{};
    bool end{};
    do {
        if (stop.stop_requested())
            return Error(ReplayGainScanStatus::cancelled,
                         HRESULT_FROM_WIN32(ERROR_CANCELLED), L"cancelled");
        const HRESULT read = reader->Read(
            std::max<DWORD>(reader->SuggestedBufferBytes(), 4096U), bytes, end);
        if (FAILED(read))
            return Error(ReplayGainScanStatus::decode_error, read,
                         L"legacy reader Read failed");
        if (!bytes.empty()) {
            DWORD frames{};
            if (!ConvertSamples(bytes, format, samples, &frames))
                return Error(ReplayGainScanStatus::unsupported,
                             HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                             L"decoded PCM sample format is unsupported");
            if (frames && !analyzer.Analyze(samples.data(), format.nChannels,
                                             frames))
                return Error(ReplayGainScanStatus::decode_error, E_FAIL,
                             L"ttpcomm ReplayGain analyzer rejected samples");
            decoded_frames += frames;
            if (progress) progress(decoded_frames, expected_frames);
        }
        if (bytes.empty() && !end)
            return Error(ReplayGainScanStatus::decode_error, E_UNEXPECTED,
                         L"decoder returned an empty non-terminal block");
    } while (!end);
    if (decoded_frames == 0)
        return Error(ReplayGainScanStatus::decode_error, E_UNEXPECTED,
                     L"decoder produced no audio frames");

    double gain{}, peak{};
    if (!analyzer.Finish(&gain, &peak))
        return Error(ReplayGainScanStatus::decode_error, E_FAIL,
                     L"ttpcomm ReplayGain finalization failed");

    ReplayGainScanResult result;
    result.status = ReplayGainScanStatus::completed;
    result.gain_db = gain;
    result.peak = peak;
    result.decoded_frames = decoded_frames;
    result.result = S_OK;
    return result;
}

ReplayGainScanResult CommitReplayGainTrack(
    const plugins::PluginManager& library,
    const std::filesystem::path& path,
    const ReplayGainScanResult& analysis,
    ReplayGainCommitPolicy policy,
    std::stop_token stop) {
    if (analysis.status != ReplayGainScanStatus::completed)
        return analysis;
    if (stop.stop_requested()) {
        auto cancelled = analysis;
        cancelled.status = ReplayGainScanStatus::cancelled;
        cancelled.result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        cancelled.diagnostic = L"cancelled";
        return cancelled;
    }

    // The manual completion path 004A50B5 -> 004C80AC permanently removes
    // FILE_ATTRIBUTE_READONLY.  The live finalizer 004B1A5C does not, so it
    // must reject a read-only source rather than silently changing it.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        if (policy == ReplayGainCommitPolicy::
                          live_playback_preserve_attributes) {
            auto error = Error(ReplayGainScanStatus::write_error,
                HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
                L"live ReplayGain preserves FILE_ATTRIBUTE_READONLY");
            error.gain_db = analysis.gain_db;
            error.peak = analysis.peak;
            error.decoded_frames = analysis.decoded_frames;
            return error;
        }
        if (!SetFileAttributesW(path.c_str(),
                                attributes & ~FILE_ATTRIBUTE_READONLY)) {
            auto error = Error(ReplayGainScanStatus::write_error,
                HRESULT_FROM_WIN32(GetLastError()),
                L"clearing source FILE_ATTRIBUTE_READONLY");
            error.gain_db = analysis.gain_db;
            error.peak = analysis.peak;
            error.decoded_frames = analysis.decoded_frames;
            return error;
        }
    }
    if (stop.stop_requested()) {
        auto cancelled = analysis;
        cancelled.status = ReplayGainScanStatus::cancelled;
        cancelled.result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        cancelled.diagnostic = L"cancelled";
        return cancelled;
    }
    HRESULT opened{};
    std::wstring diagnostic;
    auto metadata = library.OpenReaderForMetadata(path, &opened, &diagnostic);
    if (!metadata) {
        auto error = Error(ReplayGainScanStatus::write_error, opened,
            diagnostic.empty() ? L"opening source metadata writer"
                               : std::move(diagnostic));
        error.gain_db = analysis.gain_db;
        error.peak = analysis.peak;
        error.decoded_frames = analysis.decoded_frames;
        return error;
    }
    const HRESULT set_gain = metadata->SetMetadataValueDirect(
        "replaygain_track_gain", FormatGain(analysis.gain_db));
    const HRESULT set_peak = metadata->SetMetadataValueDirect(
        "replaygain_track_peak", FormatPeak(analysis.peak));
    if (FAILED(set_gain) || FAILED(set_peak)) {
        auto error = Error(ReplayGainScanStatus::write_error,
            FAILED(set_gain) ? set_gain : set_peak,
            L"sound metadata slot 6 rejected ReplayGain tags");
        error.gain_db = analysis.gain_db;
        error.peak = analysis.peak;
        error.decoded_frames = analysis.decoded_frames;
        return error;
    }
    // Several private metadata implementations flush while their reader is
    // released rather than in SetValue itself.
    metadata.reset();

    auto result = analysis;
    result.diagnostic.clear();
    return result;
}

ReplayGainScanResult ScanReplayGainTrack(
    const plugins::PluginManager& library, HMODULE ttpcomm,
    const std::filesystem::path& path, bool skip_existing,
    std::stop_token stop, ReplayGainProgress progress) {
    auto analysis = AnalyzeReplayGainTrack(
        library, ttpcomm, path, skip_existing, stop, std::move(progress));
    if (analysis.status != ReplayGainScanStatus::completed) return analysis;
    return CommitReplayGainTrack(
        library, path, analysis,
        ReplayGainCommitPolicy::manual_scan_clear_read_only, stop);
}

struct PlaybackReplayGainAnalyzer::Impl {
    struct ModuleReference {
        HMODULE value{};
        ~ModuleReference() { if (value) FreeLibrary(value); }
    };

    Impl(HMODULE retained_module, DWORD sample_rate, WORD channel_count)
        : module{retained_module}, analyzer(module.value, sample_rate),
          channels(channel_count) {}

    // Reverse member destruction destroys Analyzer's private vtable object
    // before releasing the DLL which contains that vtable.
    ModuleReference module;
    Analyzer analyzer;
    WORD channels{};
    std::uint64_t frames{};
};

PlaybackReplayGainAnalyzer::PlaybackReplayGainAnalyzer(
    std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

PlaybackReplayGainAnalyzer::~PlaybackReplayGainAnalyzer() = default;

std::unique_ptr<PlaybackReplayGainAnalyzer>
PlaybackReplayGainAnalyzer::Create(
    HMODULE ttpcomm, DWORD sample_rate, WORD channels) noexcept {
    if (!ttpcomm || sample_rate == 0 || channels == 0) return {};
    HMODULE retained = RetainModule(ttpcomm);
    if (!retained) return {};
    try {
        auto impl = std::make_unique<Impl>(retained, sample_rate, channels);
        retained = nullptr;
        if (!impl->analyzer) return {};
        return std::unique_ptr<PlaybackReplayGainAnalyzer>(
            new PlaybackReplayGainAnalyzer(std::move(impl)));
    } catch (...) {
        if (retained) FreeLibrary(retained);
        return {};
    }
}

bool PlaybackReplayGainAnalyzer::Analyze(
    const double* samples, size_t sample_count) noexcept {
    if (!impl_ || !samples || impl_->channels == 0 ||
        sample_count % impl_->channels != 0)
        return false;
    const size_t frames = sample_count / impl_->channels;
    if (frames > std::numeric_limits<DWORD>::max()) return false;
    if (!impl_->analyzer.Analyze(
            samples, impl_->channels, static_cast<DWORD>(frames))) {
        Cancel();
        return false;
    }
    impl_->frames += frames;
    return true;
}

std::optional<ReplayGainScanResult>
PlaybackReplayGainAnalyzer::Finish() noexcept {
    if (!impl_ || impl_->frames == 0) return std::nullopt;
    double gain{};
    double peak{};
    if (!impl_->analyzer.Finish(&gain, &peak)) {
        Cancel();
        return std::nullopt;
    }
    ReplayGainScanResult result;
    result.status = ReplayGainScanStatus::completed;
    result.gain_db = gain;
    result.peak = peak;
    result.decoded_frames = impl_->frames;
    result.result = S_OK;
    impl_.reset();
    return result;
}

void PlaybackReplayGainAnalyzer::Cancel() noexcept { impl_.reset(); }

void QueueReplayGainCommit(
    std::shared_ptr<plugins::PluginManager> retained_library,
    std::filesystem::path path, ReplayGainScanResult analysis) noexcept {
    if (!retained_library || path.empty() ||
        analysis.status != ReplayGainScanStatus::completed)
        return;
    const auto key = CommitKey(path);
    auto& registry = CommitRegistry();
    {
        const std::scoped_lock lock(registry.mutex);
        if (!registry.paths.insert(key).second) return;
    }
    try {
        std::thread([
            library = std::move(retained_library), path = std::move(path),
            analysis = std::move(analysis), key]() mutable {
            const HRESULT initialized =
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                // Fifty 100 ms sharing retries cap each writer at roughly five
                // seconds.  Repeat-one cannot accumulate one permanent task
                // per pass, because the process-lifetime registry admits only
                // one pending transaction for a normalized source path.
                constexpr unsigned kMaximumAttempts = 50;
                for (unsigned attempt{}; attempt < kMaximumAttempts; ++attempt) {
                    auto committed = CommitReplayGainTrack(
                        *library, path, analysis,
                        ReplayGainCommitPolicy::
                            live_playback_preserve_attributes);
                    if (committed.status != ReplayGainScanStatus::write_error ||
                        !IsTransientWriterCollision(committed.result))
                        break;
                    if (attempt + 1 < kMaximumAttempts)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(100));
                }
            } catch (...) {
            }
            if (SUCCEEDED(initialized)) CoUninitialize();
            ReleaseCommitKey(key);
        }).detach();
    } catch (...) {
        // Failure to create the optional metadata writer must not affect
        // playback or force PlayerWindow to synchronously recover it.
        ReleaseCommitKey(key);
    }
}

} // namespace ttplayer::audio
