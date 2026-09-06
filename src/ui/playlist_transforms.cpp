#include "ttplayer/ui/playlist_transforms.h"

#include "modern_file_dialog.h"

#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/audio/cue_sheet.h"
#include "ttplayer/audio/pcm_output_transform.h"
#include "ttplayer/audio/replay_gain_scanner.h"
#include "ttplayer/core/text.h"
#include "ttplayer/plugins/plugin_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <commctrl.h>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <mfapi.h>
#include <mmreg.h>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ttplayer::ui {
namespace {

constexpr UINT kScanDialog = 222;
constexpr UINT kConvertConfigDialog = 220;
constexpr UINT kConvertProgressDialog = 221;
constexpr int kScanList = 0x428;
constexpr int kScanStatus = 0x41c;
constexpr int kScanButton = 0x88b;
constexpr UINT kScanRefresh = WM_APP + 0x2a0;
constexpr UINT kConvertComplete = WM_APP + 0x2a1;
constexpr UINT kConvertRowComplete = WM_APP + 0x2a2;

constexpr int kConvertEncoder = 0x802;
constexpr int kConvertConfigure = 0x3f7;
constexpr int kConvertBits = 0x7f9;
constexpr int kConvertResample = 0x7f4;
constexpr int kConvertRate = 0x7fa;
constexpr int kConvertReplayGain = 0x851;
constexpr int kConvertEqualizer = 0x80c;
constexpr int kConvertSurround = 0x80d;
constexpr int kConvertFolder = 0x404;
constexpr int kConvertBrowse = 0x3ff;
constexpr int kConvertPriority = 0x44c;
constexpr int kConvertSaveMode = 0x411;
constexpr int kConvertAddToPlaylist = 0x7e1;
constexpr int kConvertAddNumber = 0x89f;

WORD OfflinePcmTag(const WAVEFORMATEX& format) noexcept {
    if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return format.wFormatTag;
    const auto& extended =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    static constexpr std::array<BYTE, 8> wave_tail{
        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    if (extended.SubFormat.Data2 != 0 ||
        extended.SubFormat.Data3 != 0x0010 ||
        !std::equal(wave_tail.begin(), wave_tail.end(),
                    extended.SubFormat.Data4))
        return format.wFormatTag;
    if (extended.SubFormat.Data1 == WAVE_FORMAT_PCM ||
        extended.SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT)
        return static_cast<WORD>(extended.SubFormat.Data1);
    return format.wFormatTag;
}

enum class RowState { waiting, scanning, completed, skipped, error };

struct ScanDialogState {
    HMODULE resources{};
    HMODULE ttpcomm{};
    std::shared_ptr<plugins::PluginManager> library;
    std::vector<playlist::Track> tracks;
    bool skip_existing{};
    std::vector<RowState> rows;
    std::vector<std::wstring> diagnostics;
    std::mutex mutex;
    std::atomic_size_t active{};
    std::atomic_uint progress_percent{};
    std::atomic_bool finished{};
    std::atomic_bool had_error{};
    std::atomic<HWND> dialog{};
    std::stop_source cancellation;

    ~ScanDialogState() {
        if (ttpcomm) FreeLibrary(ttpcomm);
    }
};

using ScanDialogLifetime = std::shared_ptr<ScanDialogState>;

HWND g_scan_dialog{};

std::wstring LoadText(HMODULE module, UINT identifier) {
    if (!module) return {};
    wchar_t* pointer{};
    const int length = LoadStringW(module, identifier,
        reinterpret_cast<wchar_t*>(&pointer), 0);
    return length > 0 && pointer ? std::wstring(pointer, length) : std::wstring{};
}

std::wstring WindowCaption(HWND window) {
    const int length = window ? GetWindowTextLengthW(window) : 0;
    if (length <= 0) return {};
    std::wstring caption(static_cast<size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(
        window, caption.data(), static_cast<int>(caption.size()));
    if (copied <= 0) return {};
    caption.resize(static_cast<size_t>(copied));
    return caption;
}

void ShowResourceError(HWND owner, HMODULE resources, UINT identifier) {
    const auto message = LoadText(resources, identifier);
    if (message.empty()) return;
    const auto caption = WindowCaption(owner);
    MessageBoxW(owner, message.c_str(),
                caption.empty() ? nullptr : caption.c_str(),
                MB_OK | MB_ICONEXCLAMATION);
}

std::vector<std::wstring> Split(std::wstring_view value) {
    std::vector<std::wstring> result;
    size_t begin{};
    do {
        const size_t end = value.find(L'|', begin);
        result.emplace_back(value.substr(begin,
            end == std::wstring_view::npos ? value.size() - begin : end - begin));
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    } while (begin <= value.size());
    return result;
}

std::wstring TrackTitle(const playlist::Track& track) {
    if (!track.title.empty()) {
        try {
            return core::Utf8ToWide(track.title);
        } catch (...) {
            // A damaged legacy tag must not abort creation of the modeless
            // scanner.  CPlayItem falls back to its source file in this case.
        }
    }
    auto title = track.path.filename().wstring();
    return title.empty() ? track.path.wstring() : title;
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

void PostScanRefresh(ScanDialogState& state, bool completed = false) noexcept {
    const HWND dialog = state.dialog.load(std::memory_order_acquire);
    if (dialog) {
        // Carry the state identity so a stale post cannot mutate a newer scan
        // dialog if Windows recycles the old HWND after an early close.
        PostMessageW(dialog, kScanRefresh,
                     reinterpret_cast<WPARAM>(&state), completed ? 1 : 0);
    }
}

std::wstring FormatProgress(HMODULE resources, UINT identifier,
                            size_t item, size_t count) {
    const auto format = LoadText(resources, identifier);
    if (format.empty()) return {};
    wchar_t text[512]{};
    _snwprintf_s(text, _TRUNCATE, format.c_str(),
                 static_cast<int>(item), static_cast<int>(count));
    return text;
}

void PopulateList(HWND dialog, ScanDialogState& state) {
    const HWND list = GetDlgItem(dialog, kScanList);
    if (!list) return;
    ListView_SetExtendedListViewStyleEx(list,
        LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP,
        LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP);
    const auto columns = Split(LoadText(state.resources, 0x8155));
    const int widths[] = {200, 70, 280};
    for (int index{}; index < 3; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        auto label = index < static_cast<int>(columns.size())
            ? columns[static_cast<size_t>(index)] : std::wstring{};
        column.pszText = label.data();
        column.cx = widths[index];
        ListView_InsertColumn(list, index, &column);
    }
    for (size_t index{}; index < state.tracks.size(); ++index) {
        auto title = TrackTitle(state.tracks[index]);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = title.data();
        ListView_InsertItem(list, &item);
        auto source = state.tracks[index].path.wstring();
        ListView_SetItemText(list, static_cast<int>(index), 2, source.data());
    }
}

void RefreshDialog(HWND dialog, ScanDialogState& state) {
    const HWND list = GetDlgItem(dialog, kScanList);
    auto statuses = Split(LoadText(state.resources, 0x8160));
    while (statuses.size() < 3) statuses.emplace_back();
    std::vector<RowState> rows;
    {
        const std::scoped_lock lock(state.mutex);
        rows = state.rows;
    }
    if (list) {
        for (size_t index{}; index < rows.size(); ++index) {
            std::wstring text;
            switch (rows[index]) {
            case RowState::scanning:
                text = std::to_wstring(state.progress_percent.load()) + L"%";
                break;
            case RowState::completed: text = statuses[0]; break;
            case RowState::skipped: text = statuses[1]; break;
            case RowState::error: text = statuses[2]; break;
            default: break;
            }
            ListView_SetItemText(list, static_cast<int>(index), 1, text.data());
        }
    }
    const size_t active = state.active.load();
    const size_t ordinal = state.tracks.empty()
        ? 0 : std::min(active + 1, state.tracks.size());
    auto status = FormatProgress(
        state.resources, 0x8162, ordinal, state.tracks.size());
    SetDlgItemTextW(dialog, kScanStatus, status.c_str());
    if (state.finished.load()) {
        SetDlgItemTextW(dialog, kScanButton,
                        LoadText(state.resources, 8).c_str());
    }
}

void RunScan(ScanDialogLifetime lifetime, std::stop_token stop) {
    auto* state = lifetime.get();
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (size_t index{}; index < state->tracks.size(); ++index) {
        if (stop.stop_requested()) break;
        state->active.store(index);
        state->progress_percent.store(0);
        {
            const std::scoped_lock lock(state->mutex);
            state->rows[index] = RowState::scanning;
        }
        PostScanRefresh(*state);

        audio::ReplayGainScanResult result;
        if (state->tracks[index].subtrack != 0) {
            result.status = audio::ReplayGainScanStatus::unsupported;
            result.result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            result.diagnostic = L"CUE sub-track ReplayGain scan requires the "
                                L"unrecovered segmented writer path";
        } else {
            result = audio::ScanReplayGainTrack(
                *state->library, state->ttpcomm, state->tracks[index].path,
                state->skip_existing, stop,
                [state](std::uint64_t current, std::uint64_t total) {
                    if (total) state->progress_percent.store(
                        static_cast<unsigned>(std::min<std::uint64_t>(
                            current * 100U / total, 100U)));
                });
        }
        {
            const std::scoped_lock lock(state->mutex);
            state->diagnostics[index] = std::move(result.diagnostic);
            if (result.status == audio::ReplayGainScanStatus::completed)
                state->rows[index] = RowState::completed;
            else if (result.status == audio::ReplayGainScanStatus::skipped ||
                     result.status == audio::ReplayGainScanStatus::cancelled)
                state->rows[index] = RowState::skipped;
            else {
                state->rows[index] = RowState::error;
                state->had_error.store(true);
            }
        }
        state->progress_percent.store(100);
        PostScanRefresh(*state);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    state->finished.store(true);
    PostScanRefresh(*state, true);
}

INT_PTR CALLBACK ScanDialogProc(HWND dialog, UINT message,
                                WPARAM wparam, LPARAM lparam) {
    auto* holder = reinterpret_cast<ScanDialogLifetime*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    auto* state = holder ? holder->get() : nullptr;
    switch (message) {
    case WM_INITDIALOG: {
        holder = reinterpret_cast<ScanDialogLifetime*>(lparam);
        state = holder ? holder->get() : nullptr;
        if (!state) return FALSE;
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(holder));
        g_scan_dialog = dialog;
        state->dialog.store(dialog, std::memory_order_release);
        state->rows.assign(state->tracks.size(), RowState::waiting);
        state->diagnostics.resize(state->tracks.size());
        PopulateList(dialog, *state);
        ShowWindow(dialog, SW_SHOWNORMAL);
        try {
            auto lifetime = *holder;
            const auto stop = state->cancellation.get_token();
            std::thread([lifetime = std::move(lifetime), stop] {
                RunScan(lifetime, stop);
            }).detach();
        } catch (...) {
            {
                const std::scoped_lock lock(state->mutex);
                std::fill(state->rows.begin(), state->rows.end(),
                          RowState::error);
            }
            state->had_error.store(true);
            state->finished.store(true);
            RefreshDialog(dialog, *state);
        }
        return TRUE;
    }
    case kScanRefresh:
        if (!state || reinterpret_cast<ScanDialogState*>(wparam) != state)
            return TRUE;
        RefreshDialog(dialog, *state);
        if (lparam == 1 && state->finished.load() &&
            !state->had_error.load()) {
            // FUN_004A58F3 closes the modeless dialog automatically after the
            // last successful row, but retains it when any row is in error.
            PostMessageW(dialog, WM_CLOSE, 0, 0);
        }
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL || LOWORD(wparam) == kScanButton) {
            DestroyWindow(dialog);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        if (state) state->cancellation.request_stop();
        DestroyWindow(dialog);
        return TRUE;
    case WM_DESTROY:
        if (state) {
            state->cancellation.request_stop();
            HWND expected = dialog;
            state->dialog.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel);
        }
        if (g_scan_dialog == dialog) g_scan_dialog = nullptr;
        return TRUE;
    case WM_NCDESTROY:
        SetWindowLongPtrW(dialog, DWLP_USER, 0);
        delete holder;
        return TRUE;
    }
    return FALSE;
}

struct ConvertProgressState {
    HMODULE resources{};
    HMODULE ttpcomm{};
    std::shared_ptr<plugins::PluginManager> library;
    std::vector<playlist::Track> tracks;
    size_t encoder_index{};
    std::vector<std::filesystem::path> destinations;
    settings::ConvertSettings settings;
    settings::EqualizerSettings equalizer;
    std::mutex mutex;
    std::vector<PlaylistConversionResult> results;
    std::jthread worker;
};

constexpr HRESULT kConversionPending =
    HRESULT_FROM_WIN32(ERROR_IO_PENDING);

bool ConversionCancelled(HRESULT result) noexcept {
    return result == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
           result == E_ABORT;
}

size_t ConversionStateIndex(HRESULT result) noexcept {
    if (result == S_FALSE || ConversionCancelled(result)) return 1U;
    return FAILED(result) ? 2U : 0U;
}

void SetConversionRowState(HWND dialog, HMODULE resources, size_t row,
                           HRESULT result) {
    const HWND list = GetDlgItem(dialog, kScanList);
    if (!list || row > static_cast<size_t>(std::numeric_limits<int>::max()))
        return;
    const auto row_states = Split(LoadText(resources, 0x8160));
    const size_t state_index = ConversionStateIndex(result);
    if (state_index >= row_states.size()) return;
    auto text = row_states[state_index];
    ListView_SetItemText(list, static_cast<int>(row), 1, text.data());
}

void PopulateConvertProgress(HWND dialog, ConvertProgressState& state) {
    const HWND list = GetDlgItem(dialog, kScanList);
    if (!list) return;
    ListView_SetExtendedListViewStyleEx(
        list, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
    const auto columns = Split(LoadText(state.resources, 0x8155));
    const int widths[] = {180, 70, 250, 250};
    for (int index{}; index < 4; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        std::wstring label = index < static_cast<int>(columns.size())
            ? columns[static_cast<size_t>(index)] : std::wstring{};
        column.pszText = label.data();
        column.cx = widths[index];
        ListView_InsertColumn(list, index, &column);
    }
    for (size_t index{}; index < state.tracks.size(); ++index) {
        auto title = TrackTitle(state.tracks[index]);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = title.data();
        ListView_InsertItem(list, &item);
        auto source = state.tracks[index].path.wstring();
        ListView_SetItemText(list, static_cast<int>(index), 2, source.data());
        auto destination = state.destinations[index].wstring();
        ListView_SetItemText(list, static_cast<int>(index), 3,
                             destination.data());
        if (index < state.results.size() &&
            state.results[index].result != kConversionPending) {
            SetConversionRowState(dialog, state.resources, index,
                                  state.results[index].result);
        }
    }
}

int ThreadPriorityFromSetting(int value) noexcept {
    // DAT_0054131C is the three-entry table {2, 0, -15}; the persisted value
    // is the native Win32 priority itself, not a combo-box ordinal.
    switch (value) {
    case THREAD_PRIORITY_HIGHEST:
    case THREAD_PRIORITY_NORMAL:
    case THREAD_PRIORITY_IDLE:
        return value;
    default:
        return THREAD_PRIORITY_NORMAL;
    }
}

std::wstring PrimaryEncoderExtension(std::wstring extension) {
    while (!extension.empty() &&
           (extension.front() == L'*' || extension.front() == L'.'))
        extension.erase(extension.begin());
    const size_t separator = extension.find_first_of(L";|, ");
    if (separator != std::wstring::npos) extension.resize(separator);
    return extension;
}

class ConversionRuntime {
public:
    ConversionRuntime() noexcept {
        const HRESULT initialized =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_com_ = SUCCEEDED(initialized);
        // RPC_E_CHANGED_MODE means the UI conversion worker already entered
        // its original STA with CoInitialize; COM is nevertheless usable.
        com_available_ = owns_com_ || initialized == RPC_E_CHANGED_MODE;
        media_result_ = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        owns_media_foundation_ = SUCCEEDED(media_result_);
    }

    ~ConversionRuntime() {
        if (owns_media_foundation_) MFShutdown();
        if (owns_com_) CoUninitialize();
    }

    [[nodiscard]] bool Available() const noexcept {
        return com_available_ && owns_media_foundation_;
    }
    [[nodiscard]] HRESULT Result() const noexcept {
        if (!com_available_) return CO_E_NOTINITIALIZED;
        return media_result_;
    }

private:
    HRESULT media_result_{E_FAIL};
    bool owns_com_{};
    bool com_available_{};
    bool owns_media_foundation_{};
};

class OfflinePcmProcessor {
public:
    OfflinePcmProcessor(const WAVEFORMATEX& format, HMODULE ttpcomm) noexcept
        : format_(format), ttpcomm_(ttpcomm) {
        // Preserve the session's complete extensible format for the shared
        // output transform, while this recovered double-PCM stage works on
        // its explicitly identified PCM/IEEE-float sample representation.
        format_.wFormatTag = OfflinePcmTag(format);
    }

    OfflinePcmProcessor(const OfflinePcmProcessor&) = delete;
    OfflinePcmProcessor& operator=(const OfflinePcmProcessor&) = delete;

    ~OfflinePcmProcessor() {
#if defined(_MSC_VER) && defined(_M_IX86)
        if (surround_) Destroy(surround_);
        if (equalizer_) Destroy(equalizer_);
#endif
    }

    __declspec(noinline) bool Open(
        const settings::ConvertSettings& convert,
        const settings::EqualizerSettings* equalizer,
        const audio::AudioMetadata& metadata) {
        if (convert.replay_gain) {
            // FUN_004B107E/FUN_004B180A activate per-track ReplayGain only
            // when both values form a complete tag pair.
            if (metadata.replay_gain_db && metadata.replay_peak &&
                std::isfinite(*metadata.replay_gain_db) &&
                std::isfinite(*metadata.replay_peak) &&
                *metadata.replay_peak > 0.0)
                replay_gain_ = std::pow(
                    10.0, *metadata.replay_gain_db * 0.05);
        }

#if defined(_MSC_VER) && defined(_M_IX86)
        if (convert.equalizer && equalizer && equalizer->profile != -2) {
            if (!ttpcomm_) return Fail(L"ttpcomm equalizer is unavailable");
            equalizer_ = Create(ttpcomm_, 103);
            if (!equalizer_ || !EqInitialize(
                    equalizer_, format_.nSamplesPerSec, format_.nChannels) ||
                !EqSet(equalizer_, equalizer->current.data()))
                return Fail(L"ttpcomm equalizer initialization failed");
        }
        if (convert.surround && equalizer && equalizer->surround != 0) {
            if (!ttpcomm_) return Fail(L"ttpcomm surround is unavailable");
            surround_ = Create(ttpcomm_, 104);
            if (!surround_ || !SurroundInitialize(
                    surround_, format_.nSamplesPerSec, format_.nChannels,
                    std::clamp(equalizer->surround, 0, 16)))
                return Fail(L"ttpcomm surround initialization failed");
        }
#else
        if ((convert.equalizer && equalizer && equalizer->profile != -2) ||
            (convert.surround && equalizer && equalizer->surround != 0))
            return Fail(L"legacy PCM processors require the x86 host");
#endif
        return true;
    }

    bool Process(std::vector<std::byte>& bytes) {
        if (!Active() || bytes.empty()) return true;
        std::vector<double> samples;
        if (!Decode(bytes, samples))
            return Fail(L"offline processor does not support the PCM format");

        if (replay_gain_ != 1.0) {
            for (auto& sample : samples) {
                double value = sample * replay_gain_;
                if (value > 0.5)
                    value = (std::tan((value - 0.5) * 2.0) + 1.0) * 0.5;
                else if (value < -0.5)
                    value = std::tan((value + 0.5) * 2.0) * 0.5 - 0.5;
                sample = value;
            }
        }
#if defined(_MSC_VER) && defined(_M_IX86)
        int count = static_cast<int>(std::min<size_t>(
            samples.size(), static_cast<size_t>(
                std::numeric_limits<int>::max())));
        if (equalizer_ && !EqProcess(equalizer_, samples.data(), &count))
            return Fail(L"ttpcomm equalizer processing failed");
        count = std::clamp(count, 0, static_cast<int>(samples.size()));
        if (format_.nChannels > 1)
            count -= count % format_.nChannels;
        samples.resize(static_cast<size_t>(count));
        if (surround_ && !SurroundProcess(
                surround_, samples.data(), count))
            return Fail(L"ttpcomm surround processing failed");
#endif
        if (!Encode(samples, bytes))
            return Fail(L"offline processor could not encode PCM");
        return true;
    }

    [[nodiscard]] const std::wstring& Error() const noexcept { return error_; }

private:
    [[nodiscard]] bool Active() const noexcept {
        return replay_gain_ != 1.0 || equalizer_ || surround_;
    }

    bool Fail(std::wstring message) {
        error_ = std::move(message);
        return false;
    }

    bool Decode(const std::vector<std::byte>& bytes,
                std::vector<double>& samples) const {
        const size_t sample_bytes = (format_.wBitsPerSample + 7U) / 8U;
        if (sample_bytes == 0 || bytes.size() % sample_bytes != 0)
            return false;
        samples.resize(bytes.size() / sample_bytes);
        const auto* input = reinterpret_cast<const unsigned char*>(bytes.data());
        for (size_t index{}; index < samples.size(); ++index) {
            const auto* value = input + index * sample_bytes;
            if (format_.wFormatTag == 3 && format_.wBitsPerSample == 32) {
                float number{};
                std::memcpy(&number, value, sizeof(number));
                samples[index] = static_cast<double>(number) * 0.5;
            } else if (format_.wFormatTag == 3 &&
                       format_.wBitsPerSample == 64) {
                double number{};
                std::memcpy(&number, value, sizeof(number));
                samples[index] = number * 0.5;
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 8) {
                samples[index] = (static_cast<int>(value[0]) - 128) / 256.0;
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 16) {
                std::int16_t number{};
                std::memcpy(&number, value, sizeof(number));
                samples[index] = number / 65536.0;
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 24) {
                std::int32_t number = static_cast<std::int32_t>(value[0]) |
                    (static_cast<std::int32_t>(value[1]) << 8) |
                    (static_cast<std::int32_t>(value[2]) << 16);
                if ((number & 0x800000) != 0) number |= ~0xffffff;
                samples[index] = number / 16777216.0;
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 32) {
                std::int32_t number{};
                std::memcpy(&number, value, sizeof(number));
                samples[index] = number / 4294967296.0;
            } else {
                return false;
            }
        }
        return true;
    }

    bool Encode(const std::vector<double>& samples,
                std::vector<std::byte>& bytes) const {
        const size_t sample_bytes = (format_.wBitsPerSample + 7U) / 8U;
        bytes.resize(samples.size() * sample_bytes);
        auto* output = reinterpret_cast<unsigned char*>(bytes.data());
        for (size_t index{}; index < samples.size(); ++index) {
            auto* value = output + index * sample_bytes;
            const double sample = std::isfinite(samples[index])
                ? samples[index] : 0.0;
            if (format_.wFormatTag == 3 && format_.wBitsPerSample == 32) {
                const float number = static_cast<float>(std::clamp(
                    sample * 2.0, -1.0, 1.0));
                std::memcpy(value, &number, sizeof(number));
            } else if (format_.wFormatTag == 3 &&
                       format_.wBitsPerSample == 64) {
                const double number = std::clamp(sample * 2.0, -1.0, 1.0);
                std::memcpy(value, &number, sizeof(number));
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 8) {
                const int number = static_cast<int>(std::llround(
                    std::clamp(sample, -0.5, 127.0 / 256.0) * 256.0 + 128.0));
                value[0] = static_cast<unsigned char>(
                    std::clamp(number, 0, 255));
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 16) {
                const auto number = static_cast<std::int16_t>(std::llround(
                    std::clamp(sample, -0.5, 32767.0 / 65536.0) * 65536.0));
                std::memcpy(value, &number, sizeof(number));
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 24) {
                const auto number = static_cast<std::int32_t>(std::llround(
                    std::clamp(sample, -0.5, 8388607.0 / 16777216.0) *
                    16777216.0));
                value[0] = static_cast<unsigned char>(number);
                value[1] = static_cast<unsigned char>(number >> 8);
                value[2] = static_cast<unsigned char>(number >> 16);
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 32) {
                const auto number = static_cast<std::int32_t>(std::llround(
                    std::clamp(sample, -0.5,
                               2147483647.0 / 4294967296.0) *
                    4294967296.0));
                std::memcpy(value, &number, sizeof(number));
            } else {
                return false;
            }
        }
        return true;
    }

#if defined(_MSC_VER) && defined(_M_IX86)
    static void* Create(HMODULE module, WORD ordinal) noexcept {
        const auto entry = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
        if (!entry) return nullptr;
        __try {
            return reinterpret_cast<void* (__cdecl*)()>(entry)();
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static void Destroy(void* object) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, int)>(table[0])(
                object, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static bool EqInitialize(void* object, DWORD rate, WORD channels) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            return reinterpret_cast<unsigned char (__thiscall*)(
                void*, DWORD, WORD)>(table[1])(object, rate, channels) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool EqSet(void* object, const int* values) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, const int*)>(
                table[2])(object, values);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool EqProcess(void* object, double* samples, int* count) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, double*, int)>(
                table[4])(object, samples, *count);
            reinterpret_cast<void (__thiscall*)(void*, double*, int*)>(
                table[5])(object, samples, count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool SurroundInitialize(void* object, DWORD rate, WORD channels,
                                   int amount) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, DWORD, WORD, int)>(
                table[1])(object, rate, channels, amount);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool SurroundProcess(void* object, double* samples,
                                int count) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, double*, int)>(
                table[2])(object, samples, count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
#endif

    WAVEFORMATEX format_{};
    HMODULE ttpcomm_{};
    double replay_gain_{1.0};
    void* equalizer_{};
    void* surround_{};
    std::wstring error_;
};

void DeleteIncompleteOutput(const std::filesystem::path& path) noexcept {
    if (!path.empty()) static_cast<void>(DeleteFileW(path.c_str()));
}

std::wstring SafeOutputStem(const playlist::Track& track) {
    std::wstring stem;
    if (track.subtrack != 0 && !track.title.empty()) {
        try {
            stem = core::Utf8ToWide(track.title);
        } catch (...) {
        }
    }
    if (stem.empty()) stem = track.path.stem().wstring();
    if (stem.empty()) stem = L"converted";
    if (track.subtrack != 0 && track.title.empty()) {
        stem.push_back(L'-');
        stem.append(std::to_wstring(track.subtrack));
    }
    for (auto& character : stem) {
        if (character < L' ' || wcschr(L"<>:\"/\\|?*", character))
            character = L'_';
    }
    while (!stem.empty() && (stem.back() == L'.' || stem.back() == L' '))
        stem.pop_back();
    return stem.empty() ? std::wstring(L"converted") : stem;
}

std::filesystem::path MakeOutputPath(
    const std::filesystem::path& folder, const playlist::Track& track,
    std::wstring_view extension, bool add_number, size_t ordinal) {
    std::wstring file_name = SafeOutputStem(track);
    if (add_number) {
        wchar_t prefix[16]{};
        _snwprintf_s(prefix, _TRUNCATE, L"%03d.",
                     static_cast<int>(std::min<size_t>(
                         ordinal, std::numeric_limits<int>::max())));
        file_name.insert(0, prefix);
    }
    file_name.push_back(L'.');
    file_name.append(extension);
    return folder / file_name;
}

std::wstring FormatPathQuestion(HMODULE resources, UINT identifier,
                                const std::filesystem::path& path) {
    const auto format = LoadText(resources, identifier);
    if (format.empty()) return {};
    wchar_t text[32768]{};
    _snwprintf_s(text, _TRUNCATE, format.c_str(), path.c_str());
    return text;
}

struct ConvertConfigState {
    HMODULE resources{};
    const plugins::PluginManager* library{};
    settings::ConvertSettings* settings{};
};

int AddComboItem(HWND combo, std::wstring_view text, LPARAM data) {
    if (!combo) return CB_ERR;
    const std::wstring terminated(text);
    const LRESULT index = SendMessageW(
        combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(terminated.c_str()));
    if (index >= 0)
        SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), data);
    return static_cast<int>(index);
}

int SelectComboData(HWND combo, LPARAM wanted, int fallback = 0) {
    if (!combo) return CB_ERR;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    int selected = CB_ERR;
    for (int index{}; index < count; ++index) {
        if (SendMessageW(combo, CB_GETITEMDATA, index, 0) == wanted) {
            selected = index;
            break;
        }
    }
    if (selected == CB_ERR && fallback >= 0 && fallback < count)
        selected = fallback;
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
    return selected;
}

LPARAM SelectedComboData(HWND combo, LPARAM fallback = 0) {
    if (!combo) return fallback;
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) return fallback;
    const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, selected, 0);
    return data == CB_ERR ? fallback : static_cast<LPARAM>(data);
}

std::wstring DialogItemText(HWND dialog, int identifier) {
    const HWND item = GetDlgItem(dialog, identifier);
    const int length = item ? GetWindowTextLengthW(item) : 0;
    if (length <= 0) return {};
    std::wstring value(static_cast<size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(item, value.data(),
                                      static_cast<int>(value.size()));
    if (copied <= 0) return {};
    value.resize(static_cast<size_t>(copied));
    return value;
}

void UpdateEncoderConfigurationButton(HWND dialog,
                                      const ConvertConfigState& state) {
    const LPARAM selected = SelectedComboData(
        GetDlgItem(dialog, kConvertEncoder), -1);
    bool configurable{};
    if (selected >= 0 &&
        static_cast<size_t>(selected) < state.library->EncoderFactories().size())
        configurable = state.library->EncoderFactories()[
            static_cast<size_t>(selected)].configurable;
    EnableWindow(GetDlgItem(dialog, kConvertConfigure), configurable);
}

void PopulateConvertConfiguration(HWND dialog, ConvertConfigState& state) {
    const auto& factories = state.library->EncoderFactories();
    HWND combo = GetDlgItem(dialog, kConvertEncoder);
    for (size_t index{}; index < factories.size(); ++index) {
        AddComboItem(combo, factories[index].name,
                     static_cast<LPARAM>(index));
    }
    SelectComboData(combo, state.settings->writer_index);
    UpdateEncoderConfigurationButton(dialog, state);

    combo = GetDlgItem(dialog, kConvertBits);
    AddComboItem(combo, LoadText(state.resources, 0x8151), 0);
    for (const int bits : {8, 16, 24, 32})
        AddComboItem(combo, std::to_wstring(bits), bits);
    SelectComboData(combo, state.settings->output_bits);

    static constexpr int rates[] = {
        8000, 11025, 16000, 22050, 24000, 32000, 44100,
        48000, 64000, 88200, 96000, 176400, 192000};
    combo = GetDlgItem(dialog, kConvertRate);
    for (const int rate : rates)
        AddComboItem(combo, std::to_wstring(rate), rate);
    const int rate_selection = SelectComboData(
        combo, state.settings->resample_rate, 6);
    const bool resample = state.settings->resample_rate != 0 &&
                          rate_selection != CB_ERR;
    CheckDlgButton(dialog, kConvertResample,
                   resample ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(combo, resample);

    combo = GetDlgItem(dialog, kConvertPriority);
    const auto priorities = Split(LoadText(state.resources, 0x44c));
    static constexpr int priority_values[] = {
        THREAD_PRIORITY_HIGHEST, THREAD_PRIORITY_NORMAL,
        THREAD_PRIORITY_IDLE};
    for (size_t index{}; index < std::size(priority_values); ++index) {
        AddComboItem(combo,
            index < priorities.size() ? priorities[index] : std::wstring{},
            priority_values[index]);
    }
    SelectComboData(combo,
                    ThreadPriorityFromSetting(state.settings->thread_priority),
                    1);

    combo = GetDlgItem(dialog, kConvertSaveMode);
    const auto save_modes = Split(LoadText(state.resources, 0x411));
    for (size_t index{}; index < save_modes.size(); ++index)
        AddComboItem(combo, save_modes[index], static_cast<LPARAM>(index));
    SelectComboData(combo, std::clamp(state.settings->save_mode, 0, 2), 1);

    SetDlgItemTextW(dialog, kConvertFolder,
                    state.settings->folder.c_str());
    CheckDlgButton(dialog, kConvertAddNumber,
                   state.settings->add_number ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, kConvertAddToPlaylist,
                   state.settings->add_to_playlist
                       ? BST_CHECKED : BST_UNCHECKED);

    // 004122CD applies these switches to the same double-PCM stages recovered
    // for real-time playback (ReplayGain, ttpcomm ordinal 103 and 104).  They
    // are processor-chain switches, not Sound AddIn registry categories.
    CheckDlgButton(dialog, kConvertReplayGain,
        state.settings->replay_gain ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, kConvertEqualizer,
        state.settings->equalizer ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, kConvertSurround,
        state.settings->surround ? BST_CHECKED : BST_UNCHECKED);
}

bool CommitConvertConfiguration(HWND dialog, ConvertConfigState& state) {
    auto& settings = *state.settings;
    const LPARAM writer = SelectedComboData(
        GetDlgItem(dialog, kConvertEncoder), -1);
    if (writer < 0 ||
        static_cast<size_t>(writer) >= state.library->EncoderFactories().size())
        return false;
    settings.writer_index = static_cast<int>(writer);
    settings.output_bits = static_cast<int>(SelectedComboData(
        GetDlgItem(dialog, kConvertBits), 0));
    settings.resample_rate =
        IsDlgButtonChecked(dialog, kConvertResample) == BST_CHECKED
            ? static_cast<int>(SelectedComboData(
                  GetDlgItem(dialog, kConvertRate), 0))
            : 0;
    settings.folder = DialogItemText(dialog, kConvertFolder);
    settings.thread_priority = static_cast<int>(SelectedComboData(
        GetDlgItem(dialog, kConvertPriority), THREAD_PRIORITY_NORMAL));
    settings.save_mode = std::clamp(static_cast<int>(SelectedComboData(
        GetDlgItem(dialog, kConvertSaveMode), 1)), 0, 2);
    settings.add_number =
        IsDlgButtonChecked(dialog, kConvertAddNumber) == BST_CHECKED;
    settings.add_to_playlist =
        IsDlgButtonChecked(dialog, kConvertAddToPlaylist) == BST_CHECKED;
    settings.replay_gain =
        IsDlgButtonChecked(dialog, kConvertReplayGain) == BST_CHECKED;
    settings.equalizer =
        IsDlgButtonChecked(dialog, kConvertEqualizer) == BST_CHECKED;
    settings.surround =
        IsDlgButtonChecked(dialog, kConvertSurround) == BST_CHECKED;
    return true;
}

INT_PTR CALLBACK ConvertConfigProc(HWND dialog, UINT message,
                                   WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ConvertConfigState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        state = reinterpret_cast<ConvertConfigState*>(lparam);
        if (!state || !state->library || !state->settings) return FALSE;
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        PopulateConvertConfiguration(dialog, *state);
        return TRUE;
    case WM_COMMAND:
        if (!state) return FALSE;
        switch (LOWORD(wparam)) {
        case kConvertEncoder:
            if (HIWORD(wparam) == CBN_SELCHANGE)
                UpdateEncoderConfigurationButton(dialog, *state);
            return TRUE;
        case kConvertConfigure: {
            const LPARAM selected = SelectedComboData(
                GetDlgItem(dialog, kConvertEncoder), -1);
            if (selected >= 0)
                static_cast<void>(state->library->ConfigureEncoder(
                    static_cast<size_t>(selected), dialog));
            return TRUE;
        }
        case kConvertResample: {
            const bool checked = IsDlgButtonChecked(
                dialog, kConvertResample) == BST_CHECKED;
            EnableWindow(GetDlgItem(dialog, kConvertRate), checked);
            if (checked && SendDlgItemMessageW(
                    dialog, kConvertRate, CB_GETCURSEL, 0, 0) == CB_ERR)
                SendDlgItemMessageW(dialog, kConvertRate,
                                    CB_SETCURSEL, 6, 0);
            return TRUE;
        }
        case kConvertBrowse: {
            detail::ModernFolderOptions options;
            options.owner = dialog;
            options.initial_path = DialogItemText(dialog, kConvertFolder);
            options.title = LoadText(state->resources, 0x8152);
            if (const auto selected = detail::ModernPickFolder(options))
                SetDlgItemTextW(dialog, kConvertFolder,
                                selected->path.c_str());
            return TRUE;
        }
        case IDOK:
            if (CommitConvertConfiguration(dialog, *state))
                EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK ConvertProgressProc(HWND dialog, UINT message,
                                     WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ConvertProgressState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG: {
        state = reinterpret_cast<ConvertProgressState*>(lparam);
        if (!state || !state->library) return FALSE;
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(state));
        if (state->tracks.empty() ||
            state->tracks.size() != state->destinations.size() ||
            state->tracks.size() != state->results.size()) return FALSE;
        PopulateConvertProgress(dialog, *state);
        const auto status = FormatProgress(
            state->resources, 0x8161, 1, state->tracks.size());
        SetDlgItemTextW(dialog, kScanStatus, status.c_str());
        try {
            state->worker = std::jthread([state, dialog](std::stop_token stop) {
                // CConvertDlg::CWorkThread::Run (004128D4) brackets the whole
                // conversion transaction with CoInitialize/CoUninitialize.
                const HRESULT ole = CoInitialize(nullptr);
                SetThreadPriority(GetCurrentThread(),
                    ThreadPriorityFromSetting(state->settings.thread_priority));
                for (size_t index{}; index < state->tracks.size(); ++index) {
                    if (stop.stop_requested()) break;
                    bool pending{};
                    {
                        const std::scoped_lock lock(state->mutex);
                        pending = state->results[index].result ==
                                  kConversionPending;
                    }
                    if (pending) {
                        PlaylistConversionResult converted;
                        try {
                            converted = ConvertPlaylistTrack(
                                *state->library, state->tracks[index],
                                state->encoder_index,
                                state->destinations[index], state->settings,
                                state->ttpcomm, &state->equalizer, stop);
                        } catch (...) {
                            converted.result = E_OUTOFMEMORY;
                            converted.diagnostic =
                                L"conversion worker exception";
                        }
                        const std::scoped_lock lock(state->mutex);
                        state->results[index] = std::move(converted);
                    }
                    PostMessageW(dialog, kConvertRowComplete,
                                 static_cast<WPARAM>(index), 0);
                }
                if (SUCCEEDED(ole)) CoUninitialize();
                PostMessageW(dialog, kConvertComplete, 0, 0);
            });
        } catch (...) {
            for (auto& result : state->results) {
                if (result.result != kConversionPending) continue;
                result.result = E_OUTOFMEMORY;
                result.diagnostic = L"unable to start conversion worker";
            }
            PostMessageW(dialog, kConvertComplete, 0, 0);
        }
        return TRUE;
    }
    case kConvertRowComplete: {
        if (!state) return TRUE;
        const size_t index = static_cast<size_t>(wparam);
        if (index >= state->results.size()) return TRUE;
        HRESULT result{};
        {
            const std::scoped_lock lock(state->mutex);
            result = state->results[index].result;
        }
        SetConversionRowState(dialog, state->resources, index, result);
        const size_t next = std::min(index + 2U, state->tracks.size());
        const auto status = FormatProgress(
            state->resources, 0x8161, next, state->tracks.size());
        SetDlgItemTextW(dialog, kScanStatus, status.c_str());
        return TRUE;
    }
    case kConvertComplete: {
        if (!state) return TRUE;
        if (state->worker.joinable()) state->worker.join();
        std::vector<PlaylistConversionResult> results;
        {
            const std::scoped_lock lock(state->mutex);
            results = state->results;
        }
        bool failed{};
        bool cancelled{};
        for (size_t index{}; index < results.size(); ++index) {
            if (results[index].result == kConversionPending) {
                results[index].result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                cancelled = true;
            }
            SetConversionRowState(dialog, state->resources, index,
                                  results[index].result);
            cancelled = cancelled ||
                        ConversionCancelled(results[index].result);
            failed = failed || (FAILED(results[index].result) &&
                                !ConversionCancelled(results[index].result));
        }
        if (failed)
            ShowResourceError(dialog, state->resources, 0x814e);
        EndDialog(dialog, failed || cancelled ? IDCANCEL : IDOK);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL || LOWORD(wparam) == kScanButton) {
            if (state && state->worker.joinable()) {
                state->worker.request_stop();
                EnableWindow(GetDlgItem(dialog, kScanButton), FALSE);
            } else {
                EndDialog(dialog, IDCANCEL);
            }
            return TRUE;
        }
        break;
    case WM_CLOSE:
        if (state && state->worker.joinable()) {
            state->worker.request_stop();
            EnableWindow(GetDlgItem(dialog, kScanButton), FALSE);
            return TRUE;
        }
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

} // namespace

bool ReplayGainScanCommandAvailable(const plugins::PluginManager* library,
                                    HMODULE ttpcomm) noexcept {
    return library && library->RegisteredPluginCount() != 0 &&
           audio::LegacyReplayGainAvailable(ttpcomm);
}

bool PlaylistConvertCommandAvailable(
    const plugins::PluginManager* library) noexcept {
    return library && std::ranges::any_of(
        library->EncoderFactories(),
        [](const plugins::EncoderFactoryInfo& factory) {
            return !factory.extension.empty();
        });
}

PlaylistConversionResult ConvertPlaylistTrack(
    plugins::PluginManager& library, const playlist::Track& track,
    size_t encoder_index, const std::filesystem::path& destination,
    const settings::ConvertSettings& settings, HMODULE ttpcomm,
    const settings::EqualizerSettings* equalizer,
    std::stop_token stop) {
    PlaylistConversionResult converted;
    const auto& factories = library.EncoderFactories();
    if (encoder_index >= factories.size()) {
        converted.result = E_INVALIDARG;
        converted.diagnostic = L"encoder index is out of range";
        return converted;
    }
    if (destination.empty()) {
        converted.result = E_INVALIDARG;
        converted.diagnostic = L"conversion destination is empty";
        return converted;
    }

    const DWORD existing_attributes = GetFileAttributesW(destination.c_str());
    if (existing_attributes != INVALID_FILE_ATTRIBUTES) {
        if ((existing_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            converted.result = HRESULT_FROM_WIN32(ERROR_DIRECTORY);
            converted.diagnostic = L"conversion destination is a directory";
            return converted;
        }
        // FUN_004124F0: 0 skips, 1 asks in the owning dialog and reaches the
        // worker only after deletion, and 2 unconditionally replaces.
        if (settings.save_mode == 0) {
            converted.result = S_FALSE;
            converted.diagnostic = L"conversion destination was skipped";
            return converted;
        }
        if (settings.save_mode != 2) {
            converted.result = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
            converted.diagnostic = L"conversion destination requires prompt";
            return converted;
        }
        if (!DeleteFileW(destination.c_str())) {
            converted.result = HRESULT_FROM_WIN32(GetLastError());
            converted.diagnostic = L"unable to replace conversion destination";
            return converted;
        }
    }

    // CConvertDlg uses the same decoded stream selection as CSound: AddIn
    // reader+decoder first, then built-in AIFF/AU and CD-DA, with Media
    // Foundation covering WAV/MPEG/URL.  Keeping this lifetime local also
    // makes the callable conversion service safe outside the dialog worker.
    ConversionRuntime runtime;
    auto source = audio::CreateDecodedAudioSource(
        track.path, track.subtrack, &library, ttpcomm);
    if (!source) {
        converted.result = E_OUTOFMEMORY;
        converted.diagnostic = L"unable to create decoded audio source";
        return converted;
    }
    audio::PlaybackOptions source_options;
    source_options.file_buffer_bytes = 64 * 1024;
    if (!source->Open(track.path, source_options)) {
        converted.result = !runtime.Available()
            ? runtime.Result() : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        converted.diagnostic = source->Error();
        if (converted.diagnostic.empty())
            converted.diagnostic = L"decoded audio source open failed";
        return converted;
    }
    const WAVEFORMATEX& source_format = source->OutputFormat();
    if (source_format.nChannels == 0 || source_format.nSamplesPerSec == 0 ||
        source_format.nAvgBytesPerSec == 0 ||
        source_format.nBlockAlign == 0) {
        converted.result = E_INVALIDARG;
        converted.diagnostic = L"decoded audio source returned an invalid PCM format";
        return converted;
    }

    HRESULT result{};
    auto encoder = library.CreateEncoder(
        encoder_index, &result, &converted.diagnostic);
    if (!encoder) {
        converted.result = FAILED(result) ? result : E_NOINTERFACE;
        return converted;
    }

    audio::PcmOutputTransform transform;
    audio::PcmOutputTransformOptions transform_options;
    const WORD source_bits = source_format.wBitsPerSample;
    transform_options.output_bits = settings.output_bits != 0
        ? settings.output_bits
        : ((source_bits == 8 || source_bits == 16 || source_bits == 24 ||
            source_bits == 32) ? source_bits : 16);
    transform_options.resample_rate = settings.resample_rate;
    if (!transform.Open(source_format, transform_options, ttpcomm)) {
        converted.result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        converted.diagnostic = transform.Error();
        return converted;
    }
    OfflinePcmProcessor processors(source_format, ttpcomm);
    if (!processors.Open(settings, equalizer, source->Metadata())) {
        converted.result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        converted.diagnostic = processors.Error();
        return converted;
    }

    const WAVEFORMATEX output_format = transform.OutputFormat();
    result = encoder->Open(destination, output_format);
    if (FAILED(result)) {
        converted.result = result;
        converted.diagnostic = L"ISoundEncoder destination open failed";
        encoder.reset();
        DeleteIncompleteOutput(destination);
        return converted;
    }
    result = encoder->Start();
    if (FAILED(result)) {
        converted.result = result;
        converted.diagnostic = L"ISoundEncoder slot 4 start failed";
        encoder.reset();
        DeleteIncompleteOutput(destination);
        return converted;
    }

    std::vector<std::byte> pcm;
    std::vector<std::byte> transformed;
    const size_t requested = std::max<size_t>(
        static_cast<size_t>(source_options.file_buffer_bytes),
        static_cast<size_t>(source_format.nBlockAlign));
    while (!stop.stop_requested()) {
        bool end_of_stream{};
        if (!source->Read(requested, pcm, end_of_stream)) {
            result = E_FAIL;
            converted.diagnostic = source->Error();
            if (converted.diagnostic.empty())
                converted.diagnostic = L"decoded audio source PCM read failed";
            break;
        }
        pcm.resize(pcm.size() - pcm.size() % source_format.nBlockAlign);
        converted.pcm_bytes += pcm.size();
        if (!processors.Process(pcm)) {
            result = E_FAIL;
            converted.diagnostic = processors.Error();
            break;
        }
        if (!transform.Process(pcm, transformed, end_of_stream)) {
            result = E_FAIL;
            converted.diagnostic = transform.Error();
            break;
        }
        if (!transformed.empty()) {
            DWORD encoded_bytes{};
            result = encoder->WritePcm(transformed, &encoded_bytes);
            if (FAILED(result)) {
                converted.diagnostic = L"ISoundEncoder slot 6 write failed";
                break;
            }
        } else if (pcm.empty() && !end_of_stream) {
            result = E_UNEXPECTED;
            converted.diagnostic = L"reader returned an empty non-final buffer";
            break;
        }
        if (end_of_stream) {
            result = S_OK;
            break;
        }
    }
    if (stop.stop_requested() && SUCCEEDED(result)) {
        result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        converted.diagnostic = L"conversion cancelled";
    }

    const HRESULT finalized = encoder->Finalize();
    if (SUCCEEDED(result) && FAILED(finalized)) {
        result = finalized;
        converted.diagnostic = L"ISoundEncoder slot 5 finalize failed";
    }
    encoder.reset();
    if (FAILED(result)) DeleteIncompleteOutput(destination);
    converted.result = result;
    if (SUCCEEDED(result)) converted.diagnostic.clear();
    return converted;
}

std::optional<std::vector<std::filesystem::path>> ShowPlaylistConverter(
    HWND owner, HMODULE resources, HMODULE ttpcomm,
    const plugins::PluginManager* library,
    std::vector<playlist::Track> tracks,
    settings::ConvertSettings& settings,
    const settings::EqualizerSettings& equalizer) {
    if (!owner || !resources || !library || tracks.empty() ||
        !PlaylistConvertCommandAvailable(library)) return std::nullopt;

    ConvertConfigState configuration{resources, library, &settings};
    if (DialogBoxParamW(resources, MAKEINTRESOURCEW(kConvertConfigDialog),
                        owner, ConvertConfigProc,
                        reinterpret_cast<LPARAM>(&configuration)) != IDOK)
        return std::nullopt;

    const auto& factories = library->EncoderFactories();
    size_t encoder_index = settings.writer_index >= 0
        ? static_cast<size_t>(settings.writer_index) : factories.size();
    if (encoder_index >= factories.size() ||
        PrimaryEncoderExtension(factories[encoder_index].extension).empty()) {
        const auto available = std::ranges::find_if(
            factories, [](const plugins::EncoderFactoryInfo& factory) {
                return !PrimaryEncoderExtension(factory.extension).empty();
            });
        if (available == factories.end()) return std::nullopt;
        encoder_index = static_cast<size_t>(available - factories.begin());
    }

    std::wstring extension = PrimaryEncoderExtension(
        factories[encoder_index].extension);
    if (extension.empty()) {
        ShowResourceError(owner, resources, 0x814e);
        return std::nullopt;
    }

    if (settings.save_mode < 0 || settings.save_mode > 2)
        settings.save_mode = 1;

    const std::filesystem::path folder = settings.folder;
    std::vector<std::filesystem::path> destinations;
    destinations.reserve(tracks.size());
    for (size_t index{}; index < tracks.size(); ++index) {
        const auto item_folder = folder.empty()
            ? tracks[index].path.parent_path() : folder;
        destinations.push_back(MakeOutputPath(
            item_folder, tracks[index], extension,
            settings.add_number != 0, index + 1));
    }

    settings.writer_index = static_cast<int>(encoder_index);
    auto retained = library->RetainForBackground();
    if (!retained) {
        ShowResourceError(owner, resources, 0x814e);
        return std::nullopt;
    }

    ConvertProgressState progress;
    progress.resources = resources;
    progress.ttpcomm = ttpcomm;
    progress.library = std::move(retained);
    progress.tracks = std::move(tracks);
    progress.encoder_index = encoder_index;
    progress.destinations = std::move(destinations);
    progress.settings = settings;
    progress.equalizer = equalizer;
    progress.results.resize(progress.tracks.size());
    for (auto& result : progress.results) result.result = kConversionPending;

    for (size_t index{}; index < progress.destinations.size(); ++index) {
        const auto& destination = progress.destinations[index];
        const DWORD attributes = GetFileAttributesW(destination.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            progress.results[index].result =
                HRESULT_FROM_WIN32(ERROR_DIRECTORY);
            continue;
        }
        if (settings.save_mode == 0) {
            progress.results[index].result = S_FALSE;
            continue;
        }
        if (settings.save_mode == 1) {
            const auto question = FormatPathQuestion(
                resources, 0x814d, destination);
            const auto caption = WindowCaption(owner);
            const int answer = MessageBoxW(
                owner, question.empty() ? nullptr : question.c_str(),
                caption.empty() ? nullptr : caption.c_str(),
                MB_YESNO | MB_ICONQUESTION);
            if (answer != IDYES) {
                progress.results[index].result = S_FALSE;
                continue;
            }
        }
        // The single-file mode-1 prompt was already owned by IFileSaveDialog;
        // batch mode asked with ttpres 0x814D above.  004124F0 removes the old
        // file before the encoder's STGM_CREATE stream open.
        if (!DeleteFileW(destination.c_str())) {
            progress.results[index].result =
                HRESULT_FROM_WIN32(GetLastError());
        }
    }

    const INT_PTR converted = DialogBoxParamW(
        resources, MAKEINTRESOURCEW(kConvertProgressDialog), owner,
        ConvertProgressProc, reinterpret_cast<LPARAM>(&progress));
    if (converted != IDOK) return std::nullopt;
    std::vector<std::filesystem::path> outputs;
    for (size_t index{}; index < progress.results.size(); ++index) {
        if (progress.results[index].result == S_OK)
            outputs.push_back(progress.destinations[index]);
    }
    return outputs;
}

bool ShowPlaylistReplayGainScanner(
    HWND owner, HMODULE resources, HMODULE ttpcomm,
    const plugins::PluginManager* library,
    std::vector<playlist::Track> tracks, bool skip_existing) {
    if (g_scan_dialog && IsWindow(g_scan_dialog)) {
        ShowWindow(g_scan_dialog, SW_RESTORE);
        BringWindowToTop(g_scan_dialog);
        return true;
    }
    if (!owner || !resources || tracks.empty() ||
        !ReplayGainScanCommandAvailable(library, ttpcomm))
        return false;
    const auto retained_library = library->RetainForBackground();
    HMODULE retained_ttpcomm = RetainModule(ttpcomm);
    if (!retained_library || !retained_ttpcomm) {
        if (retained_ttpcomm) FreeLibrary(retained_ttpcomm);
        return false;
    }
    ScanDialogLifetime state;
    ScanDialogLifetime* holder{};
    try {
        state = std::make_shared<ScanDialogState>();
        state->resources = resources;
        state->ttpcomm = retained_ttpcomm;
        retained_ttpcomm = nullptr;
        state->library = retained_library;
        state->tracks = std::move(tracks);
        state->skip_existing = skip_existing;
        holder = new ScanDialogLifetime(state);
    } catch (...) {
        if (retained_ttpcomm) FreeLibrary(retained_ttpcomm);
        return false;
    }
    const HWND dialog = CreateDialogParamW(
        resources, MAKEINTRESOURCEW(kScanDialog), owner,
        ScanDialogProc, reinterpret_cast<LPARAM>(holder));
    if (!dialog) {
        delete holder;
        return false;
    }
    return true;
}

} // namespace ttplayer::ui
