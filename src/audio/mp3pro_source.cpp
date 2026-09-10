#include "ttplayer/audio/mp3pro_source.h"
#include "ttplayer/app/worker_process.h"
#include "winamp_input_abi.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <mmreg.h>

namespace ttplayer::audio {
namespace {
using namespace winamp_input;
using namespace std::chrono_literals;

struct MpegHeader {
    unsigned version{}; // MPEG header version bits: 2 = MPEG-2, 3 = MPEG-1.
    unsigned layer{};
    unsigned rate{};
    unsigned bitrate{};
    unsigned frame_bytes{};
};

std::optional<MpegHeader> ParseHeader(const unsigned char* bytes) {
    const auto header = (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
    const unsigned version = (header >> 19) & 3;
    const unsigned layer_bits = (header >> 17) & 3;
    const unsigned rate_index = (header >> 10) & 3;
    const unsigned bitrate_index = (header >> 12) & 15;
    if ((header & 0xffe00000U) != 0xffe00000U || version == 1 ||
        layer_bits == 0 || rate_index == 3 ||
        bitrate_index == 0 || bitrate_index == 15) return {};
    // Only Layer III is relevant to the enhancement gate at 004E6FB6.
    if (layer_bits != 1) return {};
    constexpr unsigned rates[]{44100, 48000, 32000};
    constexpr unsigned mpeg1[]{0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    constexpr unsigned mpeg2[]{0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    const unsigned rate = rates[rate_index] / (version == 3 ? 1 : version == 2 ? 2 : 4);
    const unsigned bitrate = (version == 3 ? mpeg1 : mpeg2)[bitrate_index] * 1000;
    return MpegHeader{version, 3, rate, bitrate,
        (version == 3 ? 144U : 72U) * bitrate / rate + ((header >> 9) & 1)};
}

std::optional<MpegHeader> ProbeHeader(const std::filesystem::path& path) {
    // Never send a URL, archive-member pseudo path, or directory to the
    // ANSI filename-only Winamp decoder. The factory excludes those sources.
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::array<unsigned char, 10> tag{};
    file.read(reinterpret_cast<char*>(tag.data()), tag.size());
    const auto read = file.gcount();
    file.clear();
    std::uint64_t start{};
    if (read == 10 && std::memcmp(tag.data(), "ID3", 3) == 0) {
        if ((tag[6] | tag[7] | tag[8] | tag[9]) & 0x80) return {};
        start = 10 + (static_cast<std::uint64_t>(tag[6]) << 21) +
            (static_cast<unsigned>(tag[7]) << 14) +
            (static_cast<unsigned>(tag[8]) << 7) + tag[9];
        if (tag[3] == 4 && (tag[5] & 0x10)) start += 10;
    }
    file.seekg(static_cast<std::streamoff>(start));
    std::vector<unsigned char> bytes(2 * 1024 * 1024);
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    bytes.resize(static_cast<size_t>(file.gcount()));
    for (size_t offset = 0; offset + 4 <= bytes.size(); ++offset) {
        const auto header = ParseHeader(bytes.data() + offset);
        if (!header) continue;
        // Confirm a consecutive frame, so arbitrary tag/image bytes cannot
        // select a legacy executable decoder just by resembling one header.
        const size_t next = offset + header->frame_bytes;
        if (next + 4 > bytes.size()) continue;
        const auto following = ParseHeader(bytes.data() + next);
        if (following && following->version == header->version &&
            following->rate == header->rate) return header;
    }
    return {};
}

std::string AnsiPath(const std::filesystem::path& path) {
    auto convert = [](const std::wstring& wide) -> std::string {
        const UINT page = GetACP();
        BOOL replaced{};
        // An UTF-8 system ACP forbids lpUsedDefaultChar/WC_NO_BEST_FIT_CHARS.
        const DWORD flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        BOOL* replacement = page == CP_UTF8 ? nullptr : &replaced;
        const int count = WideCharToMultiByte(page, flags, wide.c_str(), -1,
            nullptr, 0, nullptr, replacement);
        if (count <= 1 || count > MAX_PATH || replaced) return {};
        std::string result(static_cast<size_t>(count), '\0');
        if (!WideCharToMultiByte(page, flags, wide.c_str(), -1,
                result.data(), count, nullptr, replacement) || replaced) return {};
        result.pop_back();
        return result;
    };
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) return {};
    auto result = convert(absolute.native());
    if (!result.empty()) return result;
    std::array<wchar_t, 32768> short_path{};
    const DWORD count = GetShortPathNameW(absolute.c_str(), short_path.data(),
                                         static_cast<DWORD>(short_path.size()));
    return count && count < short_path.size() ? convert(short_path.data()) : std::string{};
}

struct Bridge {
    std::mutex mutex;
    std::condition_variable changed;
    WAVEFORMATEX format{};
    std::vector<std::byte> ring;
    size_t head{};
    size_t size{};
    bool stopping{};
    bool accepting{};
    bool seeking{};
    bool finished{};
    bool invalid_format{};
    std::atomic<int> priority{THREAD_PRIORITY_IDLE};

    void Clear() { head = size = 0; }
};

// In_Module is a process-global singleton in the original DLL. There must
// never be two readers changing its callbacks or filename simultaneously.
std::atomic<std::shared_ptr<Bridge>> active_bridge;
std::atomic_flag decoder_owned = ATOMIC_FLAG_INIT;

int __cdecl OutputOpen(int rate, int channels, int bits, int, int) {
    const auto bridge = active_bridge.load();
    if (!bridge) return -1;
    std::scoped_lock lock(bridge->mutex);
    if (rate <= 0 || rate > 384000 || channels < 1 || channels > 8 ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        bridge->invalid_format = true;
        bridge->changed.notify_all();
        return -1;
    }
    const WAVEFORMATEX format{WAVE_FORMAT_PCM, static_cast<WORD>(channels),
        static_cast<DWORD>(rate), static_cast<DWORD>(rate * channels * (bits / 8)),
        static_cast<WORD>(channels * (bits / 8)), static_cast<WORD>(bits), 0};
    if (bridge->accepting && std::memcmp(&format, &bridge->format, sizeof(format)) != 0) {
        bridge->invalid_format = true;
        bridge->changed.notify_all();
        return -1; // Do not reinterpret queued samples after a format change.
    }
    bridge->format = format;
    bridge->changed.notify_all();
    return 0;
}

int __cdecl OutputWrite(char* bytes, int count) {
    const auto bridge = active_bridge.load();
    if (!bridge || count < 0 || (count && !bytes)) return 1;
    const int priority = bridge->priority.exchange(THREAD_PRIORITY_IDLE);
    if (priority != THREAD_PRIORITY_IDLE) SetThreadPriority(GetCurrentThread(), priority);
    std::unique_lock lock(bridge->mutex);
    if (bridge->format.nBlockAlign && count % bridge->format.nBlockAlign != 0) {
        bridge->invalid_format = true;
        bridge->changed.notify_all();
        return 1;
    }
    size_t offset{};
    while (offset < static_cast<size_t>(count)) {
        // Discard the in-flight old block on seek; don't wait for a reader
        // while Stop is joining this very producer thread (004E732E).
        if (bridge->stopping || !bridge->accepting || bridge->seeking || bridge->invalid_format)
            return 0;
        if (bridge->size == bridge->ring.size()) {
            bridge->changed.wait(lock);
            continue;
        }
        const size_t tail = (bridge->head + bridge->size) % bridge->ring.size();
        const size_t size = std::min({static_cast<size_t>(count) - offset,
            bridge->ring.size() - bridge->size, bridge->ring.size() - tail});
        std::memcpy(bridge->ring.data() + tail, bytes + offset, size);
        bridge->size += size;
        offset += size;
        bridge->changed.notify_all();
    }
    return 0;
}

int __cdecl OutputCanWrite() {
    const auto bridge = active_bridge.load();
    if (!bridge) return 0;
    std::scoped_lock lock(bridge->mutex);
    // Let the decoder finish an old block and reach its seek/stop poll.
    if (bridge->stopping || bridge->seeking) return 0x48000;
    return bridge->accepting && !bridge->invalid_format
        ? static_cast<int>(bridge->ring.size() - bridge->size) : 0;
}

int __cdecl OutputIsPlaying() {
    const auto bridge = active_bridge.load();
    if (!bridge) return 1;
    std::scoped_lock lock(bridge->mutex);
    // 004E6EAF marks EOF, but returns 1 so the plug-in's drain loop stays
    // alive for a later seek. Read reports EOF only after consuming the ring.
    if (!bridge->seeking) bridge->finished = true;
    bridge->changed.notify_all();
    return 1;
}

void __cdecl OutputFlush(int) {
    const auto bridge = active_bridge.load();
    if (!bridge) return;
    std::scoped_lock lock(bridge->mutex);
    bridge->Clear();
    bridge->seeking = false;
    bridge->finished = false;
    bridge->changed.notify_all();
}

void __cdecl NoOp() {}
void __cdecl IgnoreInt(int) {}
void __cdecl IgnorePair(int, int) {}
void __cdecl IgnorePcm(void*, int, int, int) {}
void __cdecl IgnoreSpectrum(void*, int, int) {}
void __cdecl IgnoreWave(void*, int) {}
void __cdecl IgnoreEq(int, char*, int) {}
void __cdecl IgnoreInfo(int, int, int, int) {}
int __cdecl Zero() { return 0; }
int __cdecl OutputPause(int) { return 0; }
int __cdecl VisualMode(int* spectrum, int* waveform) {
    if (spectrum) *spectrum = 2;
    if (waveform) *waveform = 2;
    return 0;
}
int __cdecl PassSamples(short*, int samples, int, int, int) { return samples; }

OutputModule output_module{0x10, const_cast<char*>("TTPlayer PCM bridge"),
    0x7fffffff, nullptr, nullptr, nullptr, nullptr, NoOp, NoOp,
    OutputOpen, NoOp, OutputWrite, OutputCanWrite, OutputIsPlaying,
    OutputPause, IgnoreInt, IgnoreInt, OutputFlush, Zero, Zero};

// SEH is restricted to the foreign ABI entry boundary; no C++ objects with
// destructors are created inside these guards. It cannot sandbox a bad DLL.
InputModule* GetValidatedModule(GetInputModule getter) noexcept {
    __try {
        auto* module = getter();
        return module && module->version == 0x100 && module->Play &&
            module->Pause && module->UnPause && module->Stop &&
            module->GetLength && module->SetOutputTime ? module : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool InstallCallbacks(InputModule* module, HMODULE library) noexcept {
    __try {
        module->instance = library;
        module->window = nullptr;
        module->SAVSAInit = IgnorePair;
        module->SAVSADeInit = NoOp;
        module->SAAddPCMData = IgnorePcm;
        module->SAGetMode = Zero;
        module->SAAdd = IgnoreSpectrum;
        module->VSAAddPCMData = IgnorePcm;
        module->VSAGetMode = VisualMode;
        module->VSAAdd = IgnoreWave;
        module->VSASetInfo = IgnorePair;
        module->DspIsActive = Zero;
        module->DspDoSamples = PassSamples;
        module->EQSet = IgnoreEq;
        module->SetInfo = IgnoreInfo;
        output_module.instance = library;
        module->output = &output_module;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallVoid(void (__cdecl* function)()) noexcept {
    __try { function(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSeek(InputModule* module, int position) noexcept {
    __try { module->SetOutputTime(position); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int CallPlay(InputModule* module, char* filename) noexcept {
    __try { return module->Play(filename); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int CallLength(InputModule* module) noexcept {
    __try { return module->GetLength(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

InputModule* LoadDecoder() {
    // 004E6CBD caches a single load attempt. Pin the valid module until exit;
    // unloading it while a decoder callback exists would invalidate code.
    static InputModule* cached = []() -> InputModule* {
        const auto exe = app::CurrentExecutablePath();
        if (exe.empty()) return nullptr;
        const auto path = exe.parent_path() / L"mp3PRO.dll";
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return nullptr;
        const HMODULE library = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) return nullptr;
        const auto getter = reinterpret_cast<GetInputModule>(GetProcAddress(library, "winampGetInModule2"));
        auto* module = getter ? GetValidatedModule(getter) : nullptr;
        if (!module || !InstallCallbacks(module, library)) {
            FreeLibrary(library);
            return nullptr;
        }
        // The original wrapper does not call Winamp Init/Quit/Config.
        return module;
    }();
    return cached;
}

class Mp3ProSource final : public DecodedAudioSource {
public:
    explicit Mp3ProSource(std::unique_ptr<DecodedAudioSource> fallback)
        : fallback_(std::move(fallback)) {}
    ~Mp3ProSource() override { CloseDecoder(); }

    bool Open(const std::filesystem::path& path, const PlaybackOptions& options) override {
        CloseDecoder();
        error_.clear();
        if (!fallback_ || !fallback_->Open(path, options)) return false;
        const auto format = fallback_->DisplayFormat();
        if (format.format_tag != WAVE_FORMAT_MPEG && format.format_tag != WAVE_FORMAT_MPEGLAYER3)
            return true;
        const auto header = ProbeHeader(path);
        if (!header || header->version != 2 || header->layer != 3 || header->rate >= 32000)
            return true;
        filename_ = AnsiPath(path);
        if (filename_.empty() || decoder_owned.test_and_set()) return true;
        owns_decoder_ = true;
        try {
            module_ = LoadDecoder();
            if (!module_) { CloseDecoder(); return true; }
            bridge_ = std::make_shared<Bridge>();
            active_bridge.store(bridge_);
            // Match Play -> Pause -> sample-rate comparison at 004E6FB6.
            played_ = true;
            const int played = CallPlay(module_, filename_.data());
            const bool paused = CallVoid(module_->Pause);
            if (played != 0 || !paused) { CloseDecoder(); return true; }
            {
                std::unique_lock lock(bridge_->mutex);
                bridge_->changed.wait_for(lock, 500ms, [&] {
                    return bridge_->format.nSamplesPerSec || bridge_->invalid_format;
                });
                wave_format_ = bridge_->format;
                if (!bridge_->invalid_format && wave_format_.nSamplesPerSec &&
                    wave_format_.nSamplesPerSec != header->rate) {
                    // 004E7112 uses the *base* PCM format before replacing
                    // it: max(0x48000, 2 * avgBytes + 576 * blockAlign).
                    // Its MulDiv(x, block, block) does not round to 1152.
                    const auto& base_pcm = fallback_->OutputFormat();
                    const size_t capacity = std::max<size_t>(0x48000,
                        static_cast<size_t>(base_pcm.nAvgBytesPerSec) * 2 +
                        static_cast<size_t>(base_pcm.nBlockAlign) * 576);
                    const size_t align = wave_format_.nBlockAlign;
                    bridge_->ring.resize((capacity + align - 1) / align * align);
                    bridge_->accepting = true;
                }
            }
            if (bridge_->ring.empty()) { CloseDecoder(); return true; }
            duration_ = std::chrono::milliseconds(std::max(0, CallLength(module_)));
            if (duration_.count() == 0) duration_ = fallback_->Duration();
            // 004E4B71's encoded rate is saved before replacing the PCM
            // format. Prefer the base reader's VBR average, not frame #1.
            const auto encoded_bytes = format.bytes_per_second &&
                format.bytes_per_second != fallback_->OutputFormat().nAvgBytesPerSec
                ? format.bytes_per_second : header->bitrate / 8;
            display_format_ = {wave_format_.wFormatTag, wave_format_.nChannels,
                wave_format_.nSamplesPerSec, encoded_bytes,
                wave_format_.nBlockAlign, wave_format_.wBitsPerSample, L"mp3PRO Audio"};
            host_paused_ = false;
            decoder_paused_ = true;
            if (!Seek(0ms)) { CloseDecoder(); error_.clear(); return true; }
            return true;
        } catch (...) {
            CloseDecoder();
            error_.clear();
            return true; // An optional enhancement must not break base playback.
        }
    }

    bool Read(size_t requested, std::vector<std::byte>& output, bool& eof) override {
        if (!bridge_) return fallback_->Read(requested, output, eof);
        output.clear();
        eof = false;
        if (requested == 0) return true;
        const size_t count = std::max<size_t>(wave_format_.nBlockAlign,
            requested - requested % wave_format_.nBlockAlign);
        // Prefill is also used while the output device is paused after a
        // seek. Temporarily run the producer, then restore its pause state.
        if (!PauseDecoder(false)) return false;
        {
            std::unique_lock lock(bridge_->mutex);
            if (!bridge_->changed.wait_for(lock, 1000ms, [&] {
                    return bridge_->size || bridge_->finished || bridge_->stopping || bridge_->invalid_format;
                })) error_ = L"mp3PRO decoder timed out waiting for PCM";
            if (bridge_->invalid_format) error_ = L"mp3PRO decoder returned an invalid PCM format";
            if (error_.empty()) {
                const size_t amount = std::min(count, bridge_->size);
                output.resize(amount);
                const size_t first = std::min(amount, bridge_->ring.size() - bridge_->head);
                if (first) std::memcpy(output.data(), bridge_->ring.data() + bridge_->head, first);
                if (amount > first) std::memcpy(output.data() + first, bridge_->ring.data(), amount - first);
                bridge_->head = (bridge_->head + amount) % bridge_->ring.size();
                bridge_->size -= amount;
                eof = bridge_->finished && bridge_->size == 0;
                bridge_->changed.notify_all();
            }
        }
        if (host_paused_ && !PauseDecoder(true)) return false;
        return error_.empty();
    }

    bool Seek(std::chrono::milliseconds position) override {
        if (!bridge_) return fallback_->Seek(position);
        const int target = static_cast<int>(std::clamp<std::int64_t>(position.count(), 0, INT_MAX));
        {
            std::scoped_lock lock(bridge_->mutex);
            bridge_->Clear();
            bridge_->seeking = true;
            bridge_->finished = false;
            bridge_->changed.notify_all();
        }
        if (!CallSeek(module_, target)) {
            error_ = L"mp3PRO decoder failed to seek";
            return false;
        }
        // The shipped decoder handles SetOutputTime asynchronously and
        // calls Out.Flush before emitting the new position (10002AB4).
        // Resume temporarily for other ABI-compatible builds that check
        // pause before seek. Without this acknowledgement old PCM can leak.
        if (!PauseDecoder(false)) return false;
        {
            std::unique_lock lock(bridge_->mutex);
            if (!bridge_->changed.wait_for(lock, 1000ms, [&] {
                    return !bridge_->seeking || bridge_->invalid_format;
                })) error_ = L"mp3PRO decoder timed out acknowledging seek";
            if (bridge_->invalid_format) error_ = L"mp3PRO decoder changed PCM format during seek";
        }
        if (!PauseDecoder(true)) return false;
        return error_.empty();
    }

    void SetPaused(bool paused) override {
        if (!bridge_) { fallback_->SetPaused(paused); return; }
        host_paused_ = paused;
        PauseDecoder(paused);
    }
    const WAVEFORMATEX& OutputFormat() const override {
        return bridge_ ? wave_format_ : fallback_->OutputFormat();
    }
    AudioFormat DisplayFormat() const override {
        return bridge_ ? display_format_ : fallback_->DisplayFormat();
    }
    std::chrono::milliseconds Duration() const override {
        return bridge_ ? duration_ : fallback_->Duration();
    }
    std::wstring Error() const override {
        return error_.empty() ? fallback_->Error() : error_;
    }
    AudioMetadata Metadata() const override { return fallback_->Metadata(); }

private:
    bool PauseDecoder(bool paused) {
        if (decoder_paused_ == paused) return true;
        bridge_->priority = paused ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_TIME_CRITICAL;
        if (!CallVoid(paused ? module_->Pause : module_->UnPause)) {
            error_ = L"mp3PRO decoder failed to change playback state";
            return false;
        }
        decoder_paused_ = paused;
        return true;
    }
    void CloseDecoder() noexcept {
        if (!owns_decoder_) return;
        if (bridge_) {
            std::scoped_lock lock(bridge_->mutex);
            bridge_->stopping = true;
            bridge_->changed.notify_all();
        }
        // Release blocked Write before Stop joins the DLL's decoder thread.
        const bool stopped = !module_ || !played_ || CallVoid(module_->Stop);
        // If foreign Stop faults, its worker may still exist. Quarantine
        // the singleton and retain its stopping bridge until process exit;
        // never let a surviving old callback write into another song.
        if (stopped) active_bridge.store(nullptr);
        bridge_.reset();
        module_ = nullptr;
        played_ = false;
        owns_decoder_ = false;
        if (stopped) decoder_owned.clear();
    }
    std::unique_ptr<DecodedAudioSource> fallback_;
    InputModule* module_{};
    std::shared_ptr<Bridge> bridge_;
    bool owns_decoder_{};
    bool played_{};
    bool host_paused_{};
    bool decoder_paused_{};
    std::string filename_;
    WAVEFORMATEX wave_format_{};
    AudioFormat display_format_{};
    std::chrono::milliseconds duration_{};
    std::wstring error_;
};
} // namespace

std::unique_ptr<DecodedAudioSource> WrapMp3ProSource(std::unique_ptr<DecodedAudioSource> fallback) {
    return std::make_unique<Mp3ProSource>(std::move(fallback));
}
} // namespace ttplayer::audio
