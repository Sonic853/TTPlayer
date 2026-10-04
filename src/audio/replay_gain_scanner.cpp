#include "ttplayer/audio/replay_gain_scanner.h"

#include "ttplayer/plugins/plugin_manager.h"
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/audio/builtin_file_info.h"
#include "ttplayer/audio/cue_sheet.h"
#include "ttplayer/audio/format_probe.h"
#include "ttplayer/platform/optional_windows_api.h"
#include "ttplayer/update/update.h"
#include <map>
#include <cwctype>
#include <system_error>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <mfapi.h>
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

// Shared by manual analysis using unverified readers and every ReplayGain
// metadata transaction, including playback's deferred writer. No UI thread waits.
std::timed_mutex& ReplayGainLegacyGate() {
    static std::timed_mutex gate;
    return gate;
}

bool EnterReplayGainGate(std::unique_lock<std::timed_mutex>& lock,
                        std::stop_token stop, const ReplayGainCheckpoint& checkpoint) {
    while (!stop.stop_requested() && (!checkpoint || checkpoint())) {
        if (lock.try_lock_for(std::chrono::milliseconds(50)))
            return !stop.stop_requested();
    }
    return false;
}

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
    Analyzer(HMODULE module, DWORD sample_rate) noexcept
        : minimum_frames_((sample_rate + 19ULL) / 20ULL) {
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
        if (!samples || !channels ||
            frames > std::numeric_limits<size_t>::max() / channels)
            return false;
        double peak = peak_;
        for (size_t i = 0; i < static_cast<size_t>(frames) * channels; ++i) {
            if (!std::isfinite(samples[i])) return false;
            peak = std::max(peak, std::abs(samples[i]));
        }
        if (!InvokeAnalyze(object_, samples, channels, frames)) return false;
        peak_ = peak;
        frames_ += frames;
        return true;
    }
    bool Finish(double* gain, double* peak) noexcept {
        // An empty 50 ms histogram also returns zero; it is not a valid
        // measured 0 dB result. Do not commit such incomplete measurements.
        if (frames_ < minimum_frames_ || !InvokeFinish(object_, gain, peak))
            return false;
        // 60004450 -> 60003B80 only measures the left analysis buffer.
        // Keep its loudness calculation, but measure source sample peak
        // across every input channel, before the DLL's channel downmix.
        *peak = peak_;
        return true;
    }

private:
    void* object_{};
    std::uint64_t minimum_frames_{};
    std::uint64_t frames_{};
    double peak_{};
};

bool ExistingGain(const plugins::LegacyReaderSession& reader) {
    const auto gain = reader.MetadataValue("replaygain_track_gain");
    const auto peak = reader.MetadataValue("replaygain_track_peak");
    return gain && peak && !gain->empty() && !peak->empty();
}

bool ExistingGain(const AudioMetadata& metadata) {
    bool gain{}, peak{};
    for (const auto& [key, value] : metadata.entries) {
        if (value.empty()) continue;
        if (_wcsicmp(key.c_str(), L"replaygain_track_gain") == 0) gain = true;
        if (_wcsicmp(key.c_str(), L"replaygain_track_peak") == 0) peak = true;
    }
    return gain && peak;
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
                    std::isfinite(value) ? value : 0.0;
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
                        ? static_cast<double>(value)
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
    // 004A50B5 / 004B1A5C use "%g dB" (six significant digits).
    text << std::defaultfloat << std::setprecision(6) << gain << L" dB";
    return text.str();
}

std::wstring FormatPeak(double peak) {
    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << std::defaultfloat << std::setprecision(6) << peak;
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
    return result == HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH) ||
           result == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
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

ReplayGainScanConcurrency::ReplayGainScanConcurrency(
    const plugins::PluginManager& library, HMODULE ttpcomm) {
    try {
        wchar_t filename[32768]{};
        const DWORD count = GetModuleFileNameW(ttpcomm, filename, 32768);
        analyzer_verified_ = count && count < 32768 && update::Sha256(filename) ==
            "e349ef73e8a1d35b2c5debd2ddaeb5cc2513a90647766b7339d2605b34483b33";
        std::map<std::filesystem::path, bool> modules;
        for (const auto& format : library.ReaderFormats()) {
            auto [entry, inserted] = modules.try_emplace(format.module_path, false);
            if (inserted) {
                try {
                    const auto hash = update::Sha256(format.module_path);
                    // Local FLAC / Vorbis builds: multi-instance PCM
                    // and gain parity are covered by the parallel regression.
                    entry->second = hash == "e2833c023377787fa9705d52215d3a02b7f5a742bdda4012f9d639c2e1f64dbd" ||
                                    hash == "ee58d0603a5c4c7596219590d5e92e12748187b395e14dc1275256e0904dcdc5";
                } catch (...) { }
            }
            formats_.push_back({format.pattern, entry->second});
        }
    } catch (...) { analyzer_verified_ = false; }
}

bool ReplayGainScanConcurrency::AllowsFile(const std::filesystem::path& path) const {
    auto extension = AudioExtensionHint(path);
    if (FAILED(ProbeAudioFormatHint(path, extension))) return false;
    // The same FLAC DLL also advertises TTA; that decoder was not verified.
    if (extension != L".flac" && extension != L".fla" &&
        extension != L".ogg" && extension != L".oga") return false;
    const auto needle = L"*" + extension;
    bool found{};
    for (const auto& format : formats_) {
        bool matches = format.pattern.empty();
        size_t begin{};
        while (!matches && begin <= format.pattern.size()) {
            const auto end = format.pattern.find(L';', begin);
            auto item = std::wstring_view(format.pattern).substr(begin,
                end == std::wstring::npos ? end : end - begin);
            while (!item.empty() && iswspace(item.front())) item.remove_prefix(1);
            while (!item.empty() && iswspace(item.back())) item.remove_suffix(1);
            matches = _wcsicmp(std::wstring(item).c_str(), needle.c_str()) == 0 ||
                      item == L"*.*";
            if (end == std::wstring::npos) break;
            begin = end + 1;
        }
        if (matches) {
            if (!format.verified) return false;
            found = true;
        }
    }
    return found;
}

bool ReplayGainScanConcurrency::Allows(const std::filesystem::path& path, int subtrack) const {
    if (!analyzer_verified_) return false;
    try {
        if (subtrack > 0 && _wcsicmp(path.extension().c_str(), L".cue") == 0) {
            const auto sheet = CueSheet::Load(path);
            const auto* track = sheet.FindTrack(subtrack);
            if (!track) return false;
            bool found{};
            for (const auto& candidate : sheet.AudioCandidates(*track)) {
                if (!std::filesystem::exists(candidate)) continue;
                if (!AllowsFile(candidate)) return false;
                found = true;
            }
            return found;
        }
        return AllowsFile(path);
    } catch (...) { return false; }
}

bool LegacyReplayGainAvailable(HMODULE ttpcomm) noexcept {
    return ttpcomm &&
        GetProcAddress(ttpcomm, MAKEINTRESOURCEA(100)) &&
        GetProcAddress(ttpcomm, MAKEINTRESOURCEA(101));
}

ReplayGainScanResult AnalyzeReplayGainTrack(
    const plugins::PluginManager& library, HMODULE ttpcomm,
    const std::filesystem::path& path, bool skip_existing,
    std::stop_token stop, ReplayGainProgress progress, int subtrack,
    ReplayGainCheckpoint checkpoint, const ReplayGainScanConcurrency* concurrency) {
    if (!LegacyReplayGainAvailable(ttpcomm))
        return Error(ReplayGainScanStatus::unsupported, E_NOINTERFACE,
                     L"ttpcomm ReplayGain ordinals 100/101 are unavailable");
    if (path.empty())
        return Error(ReplayGainScanStatus::unsupported, E_INVALIDARG,
                     L"empty source path");
    if (stop.stop_requested() || (checkpoint && !checkpoint()))
        return Error(ReplayGainScanStatus::cancelled,
                     HRESULT_FROM_WIN32(ERROR_CANCELLED), L"cancelled");

    std::unique_lock<std::timed_mutex> legacy_lock(ReplayGainLegacyGate(), std::defer_lock);
    if ((!concurrency || !concurrency->Allows(path, subtrack)) &&
        !EnterReplayGainGate(legacy_lock, stop, checkpoint))
        return Error(ReplayGainScanStatus::cancelled,
                     HRESULT_FROM_WIN32(ERROR_CANCELLED), L"cancelled before decoder open");

    HRESULT opened{};
    std::wstring diagnostic;
    const bool cue = subtrack > 0 && _wcsicmp(path.extension().c_str(), L".cue") == 0;
    // 004A4E8C uses the common reader/decoder factory, including the host's
    // MPEG and PCM readers. MF must outlive any system source opened here.
    struct MediaLifetime {
        HRESULT result{E_FAIL};
        void Start() { result = platform::MFStartup(MF_VERSION, MFSTARTUP_LITE); }
        ~MediaLifetime() { if (SUCCEEDED(result)) platform::MFShutdown(); }
    } media;
    std::unique_ptr<DecodedAudioSource> segment;
    std::unique_ptr<plugins::LegacyReaderSession> reader;
    if (!cue) reader = library.OpenReader(path, &opened, &diagnostic);
    if (!reader) {
        if (!cue && IsTerminalAudioOpenError(opened))
            return Error(ReplayGainScanStatus::decode_error, opened, std::move(diagnostic));
        media.Start();
        segment = CreateDecodedAudioSource(path, subtrack, &library, ttpcomm);
        if (!segment->Open(path, {})) return Error(ReplayGainScanStatus::decode_error,
            segment->ErrorResult(), segment->Error());
    }
    if (skip_existing && (segment ? ExistingGain(segment->Metadata())
                                  : ExistingGain(*reader))) {
        auto result = Error(ReplayGainScanStatus::skipped, S_FALSE,
                            L"existing ReplayGain tags retained");
        return result;
    }

    const WAVEFORMATEX format = segment ? segment->OutputFormat() : reader->Format();
    Analyzer analyzer(ttpcomm, format.nSamplesPerSec);
    if (!analyzer)
        return Error(ReplayGainScanStatus::unsupported,
                     HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                     L"sample rate is not supported by ttpcomm ordinal 101");

    const auto duration = segment ? segment->Duration().count() : reader->DurationMilliseconds();
    const std::uint64_t expected_frames = duration > 0
        ? static_cast<std::uint64_t>(duration) * format.nSamplesPerSec / 1000U : 0;
    std::vector<std::byte> bytes;
    std::vector<double> samples;
    std::uint64_t decoded_frames{};
    unsigned empty_reads{};
    bool end{};
    do {
        if (stop.stop_requested() || (checkpoint && !checkpoint()))
            return Error(ReplayGainScanStatus::cancelled,
                         HRESULT_FROM_WIN32(ERROR_CANCELLED), L"cancelled");
        const HRESULT read = segment ? (segment->Read(16384, bytes, end) ? S_OK : E_FAIL)
            : reader->Read(std::max<DWORD>(reader->SuggestedBufferBytes(), 4096U), bytes, end);
        if (FAILED(read))
            return Error(ReplayGainScanStatus::decode_error,
                         segment ? segment->ErrorResult() : read,
                         segment ? segment->Error() : L"legacy reader Read failed");
        if (!bytes.empty()) {
            empty_reads = 0;
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
        if (bytes.empty() && !end) {
            // 004E3CF3 can consume compressed input without producing PCM
            // in the same turn (AAC/ASF). Only persistent stalls are errors.
            if (++empty_reads >= 1024)
                return Error(ReplayGainScanStatus::decode_error, E_UNEXPECTED,
                             L"decoder repeatedly returned no audio frames");
            Sleep(1);
        }
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
    std::stop_token stop, int subtrack, ReplayGainCheckpoint checkpoint) {
    if (analysis.status != ReplayGainScanStatus::completed)
        return analysis;
    if (!std::isfinite(analysis.gain_db) || !std::isfinite(analysis.peak) ||
        analysis.peak < 0)
        return Error(ReplayGainScanStatus::write_error, E_INVALIDARG,
                     L"invalid ReplayGain measurement");
    if (stop.stop_requested()) {
        auto cancelled = analysis;
        cancelled.status = ReplayGainScanStatus::cancelled;
        cancelled.result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        cancelled.diagnostic = L"cancelled";
        return cancelled;
    }

    std::unique_lock<std::timed_mutex> write_lock(ReplayGainLegacyGate(), std::defer_lock);
    if (!EnterReplayGainGate(write_lock, stop, checkpoint))
        return Error(ReplayGainScanStatus::cancelled,
                     HRESULT_FROM_WIN32(ERROR_CANCELLED), L"cancelled before metadata write");

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
    if (subtrack > 0 && _wcsicmp(path.extension().c_str(), L".cue") == 0) {
        try {
            CueSheet::Load(path).WriteTrackMetadata(subtrack, {
                {L"replaygain_track_gain", FormatGain(analysis.gain_db)},
                {L"replaygain_track_peak", FormatPeak(analysis.peak)}});
            return analysis;
        } catch (const std::system_error& error) {
            return Error(ReplayGainScanStatus::write_error,
                HRESULT_FROM_WIN32(error.code().value()), L"writing CUE ReplayGain");
        } catch (const std::exception&) {
            return Error(ReplayGainScanStatus::write_error,
                HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"writing CUE ReplayGain");
        }
    }
    HRESULT opened{};
    std::wstring diagnostic;
    auto metadata = library.OpenReaderForMetadata(path, &opened, &diagnostic);
    if (!metadata) {
        BuiltinFileInfo info;
        if (!IsTerminalAudioOpenError(opened) &&
            SUCCEEDED(ReadBuiltinMpegFileInfo(path, {}, info))) {
            const HRESULT written = WriteBuiltinMpegReplayGain(
                path, FormatGain(analysis.gain_db), FormatPeak(analysis.peak));
            if (SUCCEEDED(written)) return analysis;
            opened = written;
            diagnostic = L"writing built-in MPEG ReplayGain tags";
        }
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
    const HRESULT committed = metadata->CommitMetadata();
    if (FAILED(committed)) {
        auto error = analysis;
        error.status = ReplayGainScanStatus::write_error;
        error.result = committed;
        error.diagnostic = L"committing ReplayGain tags to disk";
        return error;
    }
    // Old private metadata implementations still flush on final Release.
    metadata.reset();

    // A legacy metadata setter can return S_OK while ignoring an unknown
    // field (observed with ASF). Verify persistence instead of marking the
    // row complete merely because both calls returned success.
    auto saved = library.OpenReaderForInspection(path, &opened, &diagnostic);
    const auto gain = saved ? saved->MetadataValue("replaygain_track_gain") : std::nullopt;
    const auto peak = saved ? saved->MetadataValue("replaygain_track_peak") : std::nullopt;
    if (!gain || !peak || *gain != FormatGain(analysis.gain_db) ||
        *peak != FormatPeak(analysis.peak)) {
        auto error = analysis;
        error.status = ReplayGainScanStatus::write_error;
        error.result = FAILED(opened) ? opened : STG_E_WRITEFAULT;
        error.diagnostic = L"ReplayGain tags were not persisted by the metadata writer";
        return error;
    }

    auto result = analysis;
    result.diagnostic.clear();
    return result;
}

ReplayGainScanResult ScanReplayGainTrack(
    const plugins::PluginManager& library, HMODULE ttpcomm,
    const std::filesystem::path& path, bool skip_existing,
    std::stop_token stop, ReplayGainProgress progress, int subtrack,
    ReplayGainCheckpoint checkpoint) {
    auto analysis = AnalyzeReplayGainTrack(
        library, ttpcomm, path, skip_existing, stop, std::move(progress), subtrack,
        checkpoint);
    if (analysis.status != ReplayGainScanStatus::completed) return analysis;
    if (stop.stop_requested() || (checkpoint && !checkpoint()))
        return Error(ReplayGainScanStatus::cancelled,
                     HRESULT_FROM_WIN32(ERROR_CANCELLED), L"cancelled");
    return CommitReplayGainTrack(
        library, path, analysis,
        ReplayGainCommitPolicy::manual_scan_clear_read_only, stop, subtrack, checkpoint);
}

struct PlaybackReplayGainAnalyzer::Impl {
    struct ModuleReference {
        HMODULE value{};
        ~ModuleReference() { if (value) FreeLibrary(value); }
    };

    Impl(HMODULE retained_module, DWORD sample_rate, WORD channel_count)
        : module{retained_module}, analyzer(module.value, sample_rate),
          channels(channel_count), rate(sample_rate) {}

    // Reverse member destruction destroys Analyzer's private vtable object
    // before releasing the DLL which contains that vtable.
    ModuleReference module;
    Analyzer analyzer;
    WORD channels{};
    DWORD rate{};
    std::vector<double> pcm;
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

bool PlaybackReplayGainAnalyzer::AnalyzePcm(
    const std::vector<std::byte>& bytes, const WAVEFORMATEX& format) noexcept {
    if (!impl_ || format.nChannels != impl_->channels ||
        format.nSamplesPerSec != impl_->rate)
        return false;
    try {
        DWORD frames{};
        if (!ConvertSamples(bytes, format, impl_->pcm, &frames)) return false;
        if (frames == 0) return true;
        return Analyze(impl_->pcm.data(), impl_->pcm.size());
    } catch (...) {
        Cancel();
        return false;
    }
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
    std::filesystem::path path, ReplayGainScanResult analysis, int subtrack) noexcept {
    if (!retained_library || path.empty() ||
        analysis.status != ReplayGainScanStatus::completed)
        return;
    const auto key = CommitKey(path) + L"|" + std::to_wstring(subtrack);
    auto& registry = CommitRegistry();
    {
        const std::scoped_lock lock(registry.mutex);
        if (!registry.paths.insert(key).second) return;
    }
    try {
        std::thread([
            library = std::move(retained_library), path = std::move(path),
            analysis = std::move(analysis), key, subtrack]() mutable {
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
                            live_playback_preserve_attributes, {}, subtrack);
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
