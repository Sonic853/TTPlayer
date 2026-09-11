#include "ttplayer/ui/playlist_transforms.h"

#include "modern_file_dialog.h"

#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/audio/builtin_file_info.h"
#include "ttplayer/audio/file_encoder.h"
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
#include <condition_variable>
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
constexpr UINT kConvertPrompt = WM_APP + 0x2a3;
constexpr UINT kConvertRefresh = WM_APP + 0x2a4;
HWND g_convert_dialog{};

LRESULT CALLBACK ConvertButtonImageProc(HWND button, UINT message, WPARAM wparam,
    LPARAM lparam, UINT_PTR subclass, DWORD_PTR data) {
    if (message != WM_NCDESTROY)
        return DefSubclassProc(button,message,wparam,lparam);
    RemoveWindowSubclass(button,ConvertButtonImageProc,subclass);
    const LRESULT result=DefSubclassProc(button,message,wparam,lparam);
    if (data) ImageList_Destroy(reinterpret_cast<HIMAGELIST>(data));
    return result;
}

void InstallConvertBitmap(HWND dialog, int control, HMODULE resources, UINT resource) {
    const HWND button=GetDlgItem(dialog,control);
    const HBITMAP bitmap=static_cast<HBITMAP>(LoadImageW(resources,
        MAKEINTRESOURCEW(resource),IMAGE_BITMAP,0,0,LR_CREATEDIBSECTION));
    if (!button || !bitmap) { if (bitmap) DeleteObject(bitmap); return; }
    BITMAP details{};
    GetObjectW(bitmap,sizeof(details),&details);
    const HIMAGELIST images=ImageList_Create(details.bmWidth,details.bmHeight,
                                            ILC_COLOR24|ILC_MASK,1,0);
    const int added=images ? ImageList_AddMasked(images,bitmap,RGB(192,192,192)) : -1;
    DeleteObject(bitmap);
    if (added<0) { if (images) ImageList_Destroy(images); return; }
    BUTTON_IMAGELIST layout{};
    layout.himl=images;
    layout.margin={3,0,3,0};
    layout.uAlign=GetWindowTextLengthW(button) ? BUTTON_IMAGELIST_ALIGN_LEFT
                                            : BUTTON_IMAGELIST_ALIGN_CENTER;
    if (!SetWindowSubclass(button,ConvertButtonImageProc,0x54544342,
                           reinterpret_cast<DWORD_PTR>(images))) {
        ImageList_Destroy(images); return;
    }
    SendMessageW(button,BCM_SETIMAGELIST,0,reinterpret_cast<LPARAM>(&layout));
}

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
    std::vector<std::wstring> display_titles;
    size_t encoder_index{};
    std::vector<std::filesystem::path> destinations;
    settings::ConvertSettings settings;
    settings::EqualizerSettings equalizer;
    std::mutex mutex;
    std::vector<PlaylistConversionResult> results;
    std::stop_source cancellation;
    std::condition_variable condition;
    bool paused{}, finished{};
    std::atomic<HWND> dialog{};
    std::atomic_size_t active{};
    std::atomic_uint percent{};
    std::wstring prompt_path;
    std::wstring pause_text, resume_text;
    bool dialog_initialized{};
    SIZE initial_client{}, minimum_window{};
    std::array<RECT,4> controls{};
    HIMAGELIST images{};
    std::function<void(const std::filesystem::path&)> completed;
    bool Checkpoint() {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] {
            return !paused || cancellation.stop_requested();
        });
        return !cancellation.stop_requested();
    }
    void Cancel() {
        cancellation.request_stop();
        { const std::scoped_lock lock(mutex); paused = false; }
        condition.notify_all();
    }
    ~ConvertProgressState() { if (ttpcomm) FreeLibrary(ttpcomm); }
};
using ConvertLifetime = std::shared_ptr<ConvertProgressState>;

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
        list, 0, 0x4130 | LVS_EX_INFOTIP | LVS_EX_DOUBLEBUFFER);
    const auto columns = Split(LoadText(state.resources, 0x8155));
    const int widths[] = {130, 68, 180, 180};
    for (int index{}; index < 4; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.fmt = index == 1 ? LVCFMT_CENTER : LVCFMT_LEFT;
        std::wstring label = index < static_cast<int>(columns.size())
            ? columns[static_cast<size_t>(index)] : std::wstring{};
        column.pszText = label.data();
        column.cx = widths[index];
        ListView_InsertColumn(list, index, &column);
    }
    for (size_t index{}; index < state.tracks.size(); ++index) {
        // 00412B48 uses CPlayItem::GetDisplayTitle (004AE7FD), including
        // the playlist's configured title pattern, not the raw tag Title.
        auto title = index < state.display_titles.size() ? state.display_titles[index]
                                                        : TrackTitle(state.tracks[index]);
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
    bool lame{};
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
    if (selected > 0 &&
        static_cast<size_t>(selected) <= state.library->EncoderFactories().size())
        configurable = state.library->EncoderFactories()[
            static_cast<size_t>(selected)-1].configurable;
    if (state.lame && selected ==
        static_cast<LPARAM>(state.library->EncoderFactories().size()+1)) configurable = true;
    EnableWindow(GetDlgItem(dialog, kConvertConfigure), configurable);
    EnableWindow(GetDlgItem(dialog, kConvertBits), selected == 0);
}

void PopulateConvertConfiguration(HWND dialog, ConvertConfigState& state) {
    const auto& factories = state.library->EncoderFactories();
    HWND combo = GetDlgItem(dialog, kConvertEncoder);
    AddComboItem(combo, LoadText(state.resources, 0x811a), 0);
    for (size_t index{}; index < factories.size(); ++index) {
        AddComboItem(combo, factories[index].name,
                     static_cast<LPARAM>(index+1));
    }
    state.lame = audio::LameEncoderAvailable();
    if (state.lame) AddComboItem(combo, L"MP3 (LAME DLL)",
                                static_cast<LPARAM>(factories.size()+1));
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
        AddComboItem(combo, std::to_wstring(rate)+L" Hz", rate);
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
    if (writer < 0 || static_cast<size_t>(writer) >
        state.library->EncoderFactories().size() + (state.lame ? 1U : 0U))
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

INT_PTR CALLBACK LameConfigProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* settings = reinterpret_cast<settings::ConvertSettings*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if (message == WM_INITDIALOG) {
        settings = reinterpret_cast<settings::ConvertSettings*>(lparam);
        SetWindowLongPtrW(dialog,DWLP_USER,lparam);
        AddComboItem(GetDlgItem(dialog,100),L"CBR",0);
        AddComboItem(GetDlgItem(dialog,100),L"VBR",1);
        AddComboItem(GetDlgItem(dialog,100),L"ABR",2);
        SelectComboData(GetDlgItem(dialog,100),settings->lame_mode);
        for (int rate : {8,16,24,32,40,48,56,64,80,96,112,128,144,160,192,224,256,320})
            AddComboItem(GetDlgItem(dialog,101),std::to_wstring(rate),rate);
        SelectComboData(GetDlgItem(dialog,101),settings->lame_bitrate,14);
        for (int quality=0;quality<10;++quality)
            AddComboItem(GetDlgItem(dialog,102),std::to_wstring(quality),quality);
        SelectComboData(GetDlgItem(dialog,102),settings->lame_quality);
    } else if (message == WM_COMMAND && LOWORD(wparam) == IDOK && settings) {
        settings->lame_mode = static_cast<int>(SelectedComboData(GetDlgItem(dialog,100)));
        settings->lame_bitrate = static_cast<int>(SelectedComboData(GetDlgItem(dialog,101),192));
        settings->lame_quality = static_cast<int>(SelectedComboData(GetDlgItem(dialog,102),2));
        EndDialog(dialog,IDOK); return TRUE;
    } else if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
        EndDialog(dialog,IDCANCEL); return TRUE;
    } else if (!(message == WM_COMMAND && LOWORD(wparam) == 100)) return FALSE;
    const auto mode = SelectedComboData(GetDlgItem(dialog,100));
    EnableWindow(GetDlgItem(dialog,101),mode != 1);
    EnableWindow(GetDlgItem(dialog,102),mode == 1);
    return TRUE;
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
        InstallConvertBitmap(dialog,kConvertConfigure,state->resources,0x160);
        InstallConvertBitmap(dialog,kConvertBrowse,state->resources,kConvertBrowse);
        InstallConvertBitmap(dialog,IDOK,state->resources,IDOK);
        InstallConvertBitmap(dialog,IDCANCEL,state->resources,IDCANCEL);
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
            if (selected > 0 && static_cast<size_t>(selected) <=
                state->library->EncoderFactories().size()) {
                std::wstring diagnostic;
                const HRESULT result=state->library->ConfigureEncoder(
                    static_cast<size_t>(selected)-1, dialog, &diagnostic);
                // Native Nero reports cancellation as a failed HRESULT too.
                // 0047D9A4 ignores that value; only surface an actual loader
                // diagnostic, never turn closing the plugin UI into an alert.
                if (FAILED(result) && !diagnostic.empty()) {
                    wchar_t code[32]{};
                    swprintf_s(code,L"\n0x%08lX",static_cast<unsigned long>(result));
                    diagnostic+=code;
                    MessageBoxW(dialog,diagnostic.c_str(),WindowCaption(dialog).c_str(),
                                MB_OK|MB_ICONERROR);
                }
            }
            else if (state->lame && selected ==
                     static_cast<LPARAM>(state->library->EncoderFactories().size()+1))
                DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(4090),
                    dialog,LameConfigProc,reinterpret_cast<LPARAM>(state->settings));
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

void PostConvert(ConvertProgressState& state, UINT message, LPARAM parameter = 0) {
    if (const HWND window = state.dialog.load())
        PostMessageW(window,message,reinterpret_cast<WPARAM>(&state),parameter);
}

void RunConversion(ConvertLifetime lifetime) {
    auto& state = *lifetime;
    const HRESULT ole = CoInitialize(nullptr);
    SetThreadPriority(GetCurrentThread(), ThreadPriorityFromSetting(state.settings.thread_priority));
    ConversionCallbacks callbacks;
    callbacks.checkpoint = [&] { return state.Checkpoint(); };
    callbacks.progress = [&](unsigned value) {
        state.percent.store(value);
        PostConvert(state,kConvertRefresh);
    };
    for (const auto& track : state.tracks) callbacks.protected_sources.push_back(track.path);
    callbacks.confirm_replace = [&](const auto& path) {
        { const std::scoped_lock lock(state.mutex); state.prompt_path = path.wstring(); }
        if (const HWND window = state.dialog.load())
            return SendMessageW(window,kConvertPrompt,
                reinterpret_cast<WPARAM>(&state),0) == IDYES;
        return false;
    };
    for (size_t index=0; index<state.tracks.size(); ++index) {
        if (!state.Checkpoint()) break;
        state.active.store(index);
        state.percent.store(0);
        PostConvert(state,kConvertRefresh);
        PlaylistConversionResult converted;
        try {
            converted = ConvertPlaylistTrack(*state.library,state.tracks[index],
                state.encoder_index,state.destinations[index],state.settings,state.ttpcomm,
                &state.equalizer,state.cancellation.get_token(),callbacks);
        } catch (...) {
            converted.result = E_FAIL;
            converted.diagnostic = L"conversion worker exception";
        }
        { const std::scoped_lock lock(state.mutex); state.results[index] = std::move(converted); }
        PostConvert(state,kConvertRowComplete,static_cast<LPARAM>(index));
    }
    if (SUCCEEDED(ole)) CoUninitialize();
    PostConvert(state,kConvertComplete);
}

void DrawConversionProgress(HDC dc, RECT bounds, unsigned percent) {
    // 004132B8 -> 00411ECD: blue fill, white remainder, inverted percentage.
    InflateRect(&bounds,0,-1);
    const COLORREF blue = RGB(0,128,255);
    const int saved = SaveDC(dc);
    HBRUSH brush = CreateSolidBrush(blue);
    FrameRect(dc,&bounds,brush);
    InflateRect(&bounds,-1,-1);
    RECT filled=bounds, empty=bounds;
    filled.right=filled.left+MulDiv(bounds.right-bounds.left,percent,100);
    empty.left=filled.right;
    FillRect(dc,&filled,brush);
    FillRect(dc,&empty,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc,TRANSPARENT);
    const auto text=std::to_wstring(percent)+L"%";
    IntersectClipRect(dc,filled.left,filled.top,filled.right,filled.bottom);
    SetTextColor(dc,RGB(255,255,255));
    DrawTextW(dc,text.c_str(),-1,&bounds,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    RestoreDC(dc,saved);
    const int second=SaveDC(dc);
    IntersectClipRect(dc,empty.left,empty.top,empty.right,empty.bottom);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,blue);
    DrawTextW(dc,text.c_str(),-1,&bounds,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    RestoreDC(dc,second);
    DeleteObject(brush);
}

INT_PTR CALLBACK ConvertProgressProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* holder=reinterpret_cast<ConvertLifetime*>(GetWindowLongPtrW(dialog,DWLP_USER));
    auto* state=holder ? holder->get() : nullptr;
    if (message >= kConvertComplete && message <= kConvertRefresh &&
        (!state || reinterpret_cast<ConvertProgressState*>(wparam) != state)) return TRUE;
    switch (message) {
    case WM_INITDIALOG: {
        holder=reinterpret_cast<ConvertLifetime*>(lparam);
        if (!holder || !*holder) return FALSE;
        state=holder->get();
        state->dialog_initialized=true;
        SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(holder));
        g_convert_dialog=dialog;
        state->dialog.store(dialog);
        const auto captions=Split(DialogItemText(dialog,kScanButton));
        state->pause_text=captions.empty() ? L"" : captions[0];
        state->resume_text=captions.size()>1 ? captions[1] : L"";
        SetDlgItemTextW(dialog,kScanButton,state->pause_text.c_str());
        InstallConvertBitmap(dialog,IDCANCEL,state->resources,IDCANCEL);
        PopulateConvertProgress(dialog,*state);
        RECT client{}, bounds{};
        GetClientRect(dialog,&client);
        GetWindowRect(dialog,&bounds);
        state->initial_client={client.right,client.bottom};
        state->minimum_window={bounds.right-bounds.left,bounds.bottom-bounds.top};
        constexpr int ids[]{kScanList,kScanStatus,kScanButton,IDCANCEL};
        for (size_t index=0;index<state->controls.size();++index) {
            GetWindowRect(GetDlgItem(dialog,ids[index]),&state->controls[index]);
            MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&state->controls[index]),2);
        }
        const auto bitmap=static_cast<HBITMAP>(LoadImageW(state->resources,
            MAKEINTRESOURCEW(0x164),IMAGE_BITMAP,0,0,LR_CREATEDIBSECTION));
        if (bitmap) {
            state->images=ImageList_Create(16,16,ILC_COLOR32|ILC_MASK,3,0);
            if (state->images) {
                ImageList_AddMasked(state->images,bitmap,RGB(255,255,255));
                ListView_SetImageList(GetDlgItem(dialog,kScanList),state->images,LVSIL_SMALL);
            }
            DeleteObject(bitmap);
        }
        try { std::thread([lifetime=*holder] { RunConversion(lifetime); }).detach(); }
        catch (...) {
            for (auto& result:state->results) result.result=E_OUTOFMEMORY;
            PostConvert(*state,kConvertComplete);
        }
        return TRUE;
    }
    case kConvertPrompt: {
        std::wstring path;
        { const std::scoped_lock lock(state->mutex); path=state->prompt_path; }
        if (state->cancellation.stop_requested()) {
            SetWindowLongPtrW(dialog,DWLP_MSGRESULT,IDNO);
            return TRUE;
        }
        const auto question=FormatPathQuestion(state->resources,0x814d,path);
        const int answer = MessageBoxW(dialog,question.c_str(),WindowCaption(dialog).c_str(),
                                      MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2);
        SetWindowLongPtrW(dialog,DWLP_MSGRESULT,answer);
        return TRUE;
    }
    case kConvertRefresh: {
        if (state->finished) return TRUE;
        const size_t active=state->active.load();
        const auto text=FormatProgress(state->resources,0x8161,active+1,state->tracks.size());
        SetDlgItemTextW(dialog,kScanStatus,text.c_str());
        const HWND list=GetDlgItem(dialog,kScanList);
        ListView_EnsureVisible(list,static_cast<int>(active),FALSE);
        ListView_RedrawItems(list,static_cast<int>(active),static_cast<int>(active));
        return TRUE;
    }
    case kConvertRowComplete: {
        const size_t index=static_cast<size_t>(lparam);
        if (index>=state->results.size()) return TRUE;
        PlaylistConversionResult result;
        { const std::scoped_lock lock(state->mutex); result=state->results[index]; }
        SetConversionRowState(dialog,state->resources,index,result.result);
        LVITEMW item{};
        item.mask=LVIF_IMAGE;
        item.iItem=static_cast<int>(index);
        item.iImage=FAILED(result.result) ? 2 : 1;
        const HWND list=GetDlgItem(dialog,kScanList);
        ListView_SetItem(list,&item);
        auto destination=result.destination.wstring();
        ListView_SetItemText(list,static_cast<int>(index),3,destination.data());
        if (result.result==S_OK && state->settings.add_to_playlist && state->completed)
            state->completed(result.destination);
        return TRUE;
    }
    case kConvertComplete: {
        bool failed{};
        {
            const std::scoped_lock lock(state->mutex);
            state->finished=true;
            state->paused=false;
            for (size_t index=0;index<state->results.size();++index) {
                auto& result=state->results[index];
                if (result.result==kConversionPending)
                    result.result=HRESULT_FROM_WIN32(ERROR_CANCELLED);
                SetConversionRowState(dialog,state->resources,index,result.result);
                failed=failed || FAILED(result.result);
            }
        }
        EnableWindow(GetDlgItem(dialog,kScanButton),FALSE);
        SetDlgItemTextW(dialog,IDCANCEL,LoadText(state->resources,8).c_str());
        InvalidateRect(GetDlgItem(dialog,kScanList),nullptr,FALSE);
        // 00412E61 keeps failures visible, without a blocking generic alert.
        if (!failed) DestroyWindow(dialog);
        return TRUE;
    }
    case WM_NOTIFY: {
        if (!state) return FALSE;
        auto* header=reinterpret_cast<NMHDR*>(lparam);
        if (header->idFrom!=kScanList) return FALSE;
        if (header->code==LVN_GETINFOTIPW) {
            auto* tip=reinterpret_cast<NMLVGETINFOTIPW*>(lparam);
            const std::scoped_lock lock(state->mutex);
            if (tip->iItem>=0 && static_cast<size_t>(tip->iItem)<state->results.size())
                wcsncpy_s(tip->pszText,tip->cchTextMax,
                    state->results[tip->iItem].diagnostic.c_str(),_TRUNCATE);
            return TRUE;
        }
        if (header->code!=NM_CUSTOMDRAW) return FALSE;
        auto* draw=reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
        LRESULT result=CDRF_DODEFAULT;
        if (draw->nmcd.dwDrawStage==CDDS_PREPAINT ||
            draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT) result=CDRF_NOTIFYSUBITEMDRAW;
        else if (draw->nmcd.dwDrawStage==(CDDS_ITEMPREPAINT|CDDS_SUBITEM) &&
                 draw->iSubItem==1 && !state->finished &&
                 draw->nmcd.dwItemSpec==state->active.load()) {
            bool pending{};
            { const std::scoped_lock lock(state->mutex);
              pending=state->results[state->active.load()].result==kConversionPending; }
            if (pending) {
                RECT bounds{};
                ListView_GetSubItemRect(header->hwndFrom,
                    static_cast<int>(draw->nmcd.dwItemSpec),1,LVIR_BOUNDS,&bounds);
                DrawConversionProgress(draw->nmcd.hdc,bounds,state->percent.load());
                result=CDRF_SKIPDEFAULT;
            }
        }
        SetWindowLongPtrW(dialog,DWLP_MSGRESULT,result);
        return TRUE;
    }
    case WM_SIZE: {
        if (!state || !state->initial_client.cx || wparam==SIZE_MINIMIZED) return FALSE;
        RECT client{}; GetClientRect(dialog,&client);
        const int dx=client.right-state->initial_client.cx;
        const int dy=client.bottom-state->initial_client.cy;
        constexpr int ids[]{kScanList,kScanStatus,kScanButton,IDCANCEL};
        for (size_t index=0;index<state->controls.size();++index) {
            auto r=state->controls[index];
            if (index==0) { r.right+=dx; r.bottom+=dy; }
            else {
                OffsetRect(&r,index>1 ? dx : 0,dy);
                if (index==1) r.right+=dx;
            }
            MoveWindow(GetDlgItem(dialog,ids[index]),r.left,r.top,
                       r.right-r.left,r.bottom-r.top,TRUE);
        }
        return TRUE;
    }
    case WM_GETMINMAXINFO:
        if (state && state->minimum_window.cx)
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize=
                {state->minimum_window.cx,state->minimum_window.cy};
        return FALSE;
    case WM_COMMAND:
        if (!state) return FALSE;
        if (LOWORD(wparam)==kScanButton && !state->finished) {
            { const std::scoped_lock lock(state->mutex); state->paused=!state->paused; }
            state->condition.notify_all();
            SetDlgItemTextW(dialog,kScanButton,
                state->paused ? state->resume_text.c_str() : state->pause_text.c_str());
            return TRUE;
        }
        if (LOWORD(wparam)!=IDCANCEL) break;
        [[fallthrough]];
    case WM_CLOSE:
        if (state) state->Cancel();
        DestroyWindow(dialog);
        return TRUE;
    case WM_DESTROY:
        if (state) {
            state->dialog.store(nullptr);
            state->Cancel();
            state->completed={};
            if (state->images) {
                ListView_SetImageList(GetDlgItem(dialog,kScanList),nullptr,LVSIL_SMALL);
                ImageList_Destroy(state->images);
                state->images=nullptr;
            }
        }
        if (g_convert_dialog==dialog) g_convert_dialog=nullptr;
        return TRUE;
    case WM_NCDESTROY:
        SetWindowLongPtrW(dialog,DWLP_USER,0);
        delete holder;
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

bool PlaylistConvertCommandAvailable(const plugins::PluginManager* library) noexcept {
    // 004CA3BE is registered before AddIns: Wave never requires a plugin DLL.
    return library != nullptr;
}

namespace {
bool SameConversionFile(const std::filesystem::path& left,
                        const std::filesystem::path& right) {
    if (left.empty() || right.empty()) return false;
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error) && !error) return true;
    error.clear();
    auto a = std::filesystem::weakly_canonical(left, error);
    if (error) a = left.lexically_normal();
    error.clear();
    auto b = std::filesystem::weakly_canonical(right, error);
    if (error) b = right.lexically_normal();
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool ConversionTargetsSource(const std::filesystem::path& output,
                             const playlist::Track& track) {
    if (SameConversionFile(output, track.path)) return true;
    audio::ArchiveMemberPath member;
    if (audio::ParseArchiveMemberPath(track.path.wstring(), member))
        return SameConversionFile(output, member.archive);
    if (_wcsicmp(track.path.extension().c_str(), L".cue") == 0) {
        try {
            const auto sheet = audio::CueSheet::Load(track.path);
            for (const auto& part : sheet.Tracks())
                if (SameConversionFile(output, part.audio_path)) return true;
        } catch (...) {
            // Failure to inspect a CUE must not authorize replacement.
            return true;
        }
    }
    return false;
}

struct ConversionOutput {
    std::filesystem::path directory;
    std::filesystem::path temporary;
    ~ConversionOutput() {
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        if (!directory.empty()) RemoveDirectoryW(directory.c_str());
    }
    HRESULT Create(const std::filesystem::path& destination) {
        std::error_code error;
        const auto folder = destination.parent_path();
        if (!folder.empty()) std::filesystem::create_directories(folder, error);
        if (error) return HRESULT_FROM_WIN32(error.value());
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) return E_FAIL;
        wchar_t unique[40]{};
        StringFromGUID2(id, unique, 40);
        const auto candidate = folder / (std::wstring(L".ttconvert-") + unique);
        // 00412575 removes an existing output before 004CD30C opens it. Nero's
        // Aac.dll treats an empty placeholder as an existing MP4 to read and
        // leaks its failed-parser read handle (Aac+5F37B -> +61D3C). Finalize
        // closes the writer, not that handle. Reserve a private directory, NOT
        // an empty media file: preserve both the original absent-file contract
        // and our atomic, same-volume replacement of the real destination.
        if (!CreateDirectoryW(candidate.c_str(), nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        directory = candidate;
        temporary = directory / (L"output" + destination.extension().wstring());
        return S_OK;
    }
    HRESULT Commit(const std::filesystem::path& destination, bool replace) {
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                        MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
            return HRESULT_FROM_WIN32(GetLastError());
        temporary.clear();
        return S_OK;
    }
};

std::vector<plugins::MetadataEntry> ConversionMetadata(
    const audio::AudioMetadata& source, const playlist::Track& track) {
    std::vector<plugins::MetadataEntry> entries;
    for (const auto& [name, value] : source.entries) entries.push_back({name,value});
    auto put = [&](std::wstring name, std::wstring value) {
        if (value.empty()) return;
        const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
            return _wcsicmp(entry.name.c_str(), name.c_str()) == 0;
        });
        if (found == entries.end()) entries.push_back({std::move(name),std::move(value)});
        else found->value = std::move(value);
    };
    // CUE metadata must describe the sub-track, not the source album image.
    put(L"title", source.title.empty() ? core::Utf8ToWide(track.title) : source.title);
    put(L"artist", source.artist.empty() ? core::Utf8ToWide(track.artist) : source.artist);
    put(L"album", source.album.empty() ? core::Utf8ToWide(track.album) : source.album);
    if (track.subtrack) put(L"tracknumber", std::to_wstring(track.subtrack));
    return entries;
}
}

PlaylistConversionResult ConvertPlaylistTrack(
    plugins::PluginManager& library, const playlist::Track& track,
    size_t encoder_index, const std::filesystem::path& destination,
    const settings::ConvertSettings& settings, HMODULE ttpcomm,
    const settings::EqualizerSettings* equalizer, std::stop_token stop,
    const ConversionCallbacks& callbacks) {
    PlaylistConversionResult converted;
    converted.destination = destination;
    auto fail = [&](HRESULT error, std::wstring text) {
        converted.result = error;
        converted.diagnostic = std::move(text);
        return converted;
    };
    auto checkpoint = [&] {
        return !stop.stop_requested() &&
            (!callbacks.checkpoint || callbacks.checkpoint());
    };
    if (!checkpoint()) return fail(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"conversion cancelled");
    const bool wave = encoder_index == kWaveConversionEncoder;
    const bool lame = encoder_index == kLameConversionEncoder;
    if (!wave && !lame && encoder_index >= library.EncoderFactories().size())
        return fail(E_INVALIDARG, L"encoder index is out of range");
    if (destination.empty()) return fail(E_INVALIDARG, L"conversion destination is empty");

    ConversionRuntime runtime;
    ConversionOutput transaction;
    HRESULT result = S_OK;
    std::unique_ptr<plugins::LegacyEncoderSession> encoder;
    std::unique_ptr<audio::FileEncoder> file_encoder;
    if (!wave && !lame) {
        encoder = library.CreateEncoder(encoder_index, &result, &converted.diagnostic);
        if (!encoder) return fail(FAILED(result) ? result : E_NOINTERFACE,
                                  converted.diagnostic);
        auto extension = PrimaryEncoderExtension(encoder->FileExtension());
        if (extension.empty()) extension = PrimaryEncoderExtension(
            library.EncoderFactories()[encoder_index].extension);
        if (extension.empty() || extension.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
            return fail(E_INVALIDARG, L"encoder has no valid configured file extension");
        converted.destination.replace_extension(extension);
    } else {
        converted.destination.replace_extension(wave ? L".wav" : L".mp3");
        file_encoder = std::make_unique<audio::FileEncoder>();
    }
    const auto& output = converted.destination;
    if (ConversionTargetsSource(output, track))
        return fail(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
                    L"conversion destination is a source file");
    for (const auto& path : callbacks.protected_sources) {
        playlist::Track other;
        other.path = path;
        if (ConversionTargetsSource(output, other))
            return fail(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
                        L"conversion destination is another batch source");
    }

    const DWORD attributes = GetFileAttributesW(output.c_str());
    bool replace = settings.save_mode == 2;
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
            return fail(HRESULT_FROM_WIN32(ERROR_DIRECTORY), L"destination is a directory");
        if (settings.save_mode == 0) return fail(S_FALSE, L"destination skipped");
        if (settings.save_mode != 2) {
            if (!callbacks.confirm_replace)
                return fail(HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), L"destination requires prompt");
            if (!callbacks.confirm_replace(output)) return fail(S_FALSE, L"destination skipped");
            replace = true;
        }
    }
    if (!checkpoint()) return fail(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"conversion cancelled");
    auto source = audio::CreateDecodedAudioSource(track.path, track.subtrack, &library, ttpcomm);
    if (!source) return fail(E_OUTOFMEMORY, L"unable to create decoded audio source");
    audio::PlaybackOptions source_options;
    source_options.file_buffer_bytes = 64 * 1024;
    if (!source->Open(track.path, source_options))
        return fail(!runtime.Available() ? runtime.Result() :
                    HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), source->Error());
    const WAVEFORMATEX& source_format = source->OutputFormat();
    if (!source_format.nChannels || !source_format.nSamplesPerSec ||
        !source_format.nAvgBytesPerSec || !source_format.nBlockAlign)
        return fail(E_INVALIDARG, L"invalid decoded PCM format");

    // 004B0D1A -> ReplayGain/EQ/surround -> 004AA360 (Wave only).
    // Quantizing before effects/resampling destroyed precision and gave native
    // encoders the wrong ABI. The original AddIn input is IEEE float64.
    audio::PcmOutputTransform decoded_transform;
    audio::PcmOutputTransformOptions decoded_options;
    decoded_options.floating_point = true;
    decoded_options.resample_rate = settings.resample_rate;
    if (!decoded_transform.Open(source_format, decoded_options, ttpcomm))
        return fail(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), decoded_transform.Error());
    const auto intermediate = decoded_transform.OutputFormat();
    const auto metadata = source->Metadata();
    OfflinePcmProcessor processors(intermediate, ttpcomm);
    if (!processors.Open(settings, equalizer, metadata))
        return fail(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), processors.Error());

    audio::PcmOutputTransform quantizer;
    WAVEFORMATEX output_format = intermediate;
    if (wave || lame) {
        audio::PcmOutputTransformOptions output_options;
        int bits=settings.output_bits ? settings.output_bits
                                     : source->DisplayFormat().bits_per_sample;
        // 004122CD asks the reader (slot 10), not the double-PCM decoder;
        // unreported/unsupported source depths fall back to 16 bits.
        if (bits!=8 && bits!=16 && bits!=24 && bits!=32) bits=16;
        output_options.output_bits = lame ? 16 : bits;
        if (!quantizer.Open(intermediate, output_options, ttpcomm))
            return fail(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), quantizer.Error());
        output_format = quantizer.OutputFormat();
    }

    // An existing destination remains intact until the complete encoder and
    // metadata transaction succeeds. Temporary files are ours, never sources.
    result = transaction.Create(output);
    if (FAILED(result)) return fail(result, L"unable to create output file");
    result = encoder ? encoder->Open(transaction.temporary, output_format) :
        file_encoder->Open(transaction.temporary, output_format, lame,
            {settings.lame_mode, settings.lame_bitrate, settings.lame_quality});
    if (FAILED(result)) return fail(result, L"encoder destination open failed");
    const auto entries = ConversionMetadata(metadata, track);
    if (encoder) {
        // Missing tag interface and unsupported individual tags are nonfatal,
        // as in 004125C1/004121C4. Never propagate ReplayGain into new audio.
        static_cast<void>(encoder->SetMetadata(entries));
        result = encoder->Start();
        if (FAILED(result)) return fail(result, L"ISoundEncoder slot 4 start failed");
    }

    std::vector<std::byte> pcm, transformed, quantized;
    const auto duration = source->Duration().count();
    unsigned last_percent = 101;
    for (;;) {
        if (!checkpoint()) {
            result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            converted.diagnostic = L"conversion cancelled";
            break;
        }
        bool end{};
        if (!source->Read(64 * 1024, pcm, end)) {
            result = E_FAIL; converted.diagnostic = source->Error(); break;
        }
        if (pcm.size() % source_format.nBlockAlign) {
            result = E_UNEXPECTED; converted.diagnostic = L"partial decoded PCM frame"; break;
        }
        converted.pcm_bytes += pcm.size();
        if (!decoded_transform.Process(pcm, transformed, end) || !processors.Process(transformed)) {
            result = E_FAIL;
            converted.diagnostic = processors.Error().empty()
                ? decoded_transform.Error() : processors.Error();
            break;
        }
        if (file_encoder) {
            if (!quantizer.Process(transformed, quantized, end)) {
                result = E_FAIL; converted.diagnostic = quantizer.Error(); break;
            }
            if (!quantized.empty()) result = file_encoder->Write(quantized);
        } else if (!transformed.empty()) result = encoder->WritePcm(transformed);
        if (FAILED(result)) { converted.diagnostic = L"encoder PCM write failed"; break; }
        if (callbacks.progress) {
            const unsigned percent = duration > 0
                ? static_cast<unsigned>(std::clamp<long double>(
                    static_cast<long double>(converted.pcm_bytes) * 100000.0L /
                    source_format.nAvgBytesPerSec / duration, 0, 100)) : 0;
            if (percent != last_percent) { callbacks.progress(percent); last_percent = percent; }
        }
        if (end) break;
        if (pcm.empty()) {
            result = E_UNEXPECTED; converted.diagnostic = L"empty non-final PCM block"; break;
        }
    }
    const HRESULT finalized = encoder ? encoder->Finalize() : file_encoder->Finalize();
    if (SUCCEEDED(result) && FAILED(finalized)) {
        result = finalized; converted.diagnostic = L"encoder finalize failed";
    }
    encoder.reset();
    file_encoder.reset();
    if (SUCCEEDED(result) && lame && !entries.empty()) {
        std::vector<audio::BuiltinTagWriteField> tags;
        for (const auto& entry : entries)
            if (!entry.name.empty() && !entry.value.empty() &&
                _wcsnicmp(entry.name.c_str(), L"replaygain_", 11) != 0)
                tags.push_back({core::WideToUtf8(entry.name), entry.value});
        const auto written = audio::WriteBuiltinFileInfo(transaction.temporary, {}, tags);
        if (FAILED(written.status)) {
            result = written.status; converted.diagnostic = L"MP3 metadata write failed";
        }
    }
    if (SUCCEEDED(result) && !checkpoint()) result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (SUCCEEDED(result)) {
        result = transaction.Commit(output, replace);
        if (FAILED(result)) converted.diagnostic = L"unable to commit converted file";
    }
    if (SUCCEEDED(result)) {
        converted.diagnostic.clear();
        if (callbacks.progress) callbacks.progress(100);
    }
    converted.result = result;
    return converted;
}

bool ShowPlaylistConverter(
    HWND owner, HMODULE resources, HMODULE ttpcomm,
    const plugins::PluginManager* library, std::vector<playlist::Track> tracks,
    settings::ConvertSettings& settings, const settings::EqualizerSettings& equalizer,
    std::function<void(const std::filesystem::path&)> completed,
    std::vector<std::wstring> display_titles) {
    // 00412A8D/00412AB8: one modeless conversion task; reopening raises it.
    if (g_convert_dialog && IsWindow(g_convert_dialog)) {
        ShowWindow(g_convert_dialog,SW_RESTORE);
        SetForegroundWindow(g_convert_dialog);
        return true;
    }
    if (!owner || !resources || !library || tracks.empty()) return false;
    ConvertConfigState configuration{resources,library,&settings};
    if (DialogBoxParamW(resources,MAKEINTRESOURCEW(kConvertConfigDialog),owner,
        ConvertConfigProc,reinterpret_cast<LPARAM>(&configuration)) != IDOK) return false;
    const size_t count=library->EncoderFactories().size();
    size_t encoder=kWaveConversionEncoder;
    std::wstring extension=L"wav";
    if (settings.writer_index>0 && static_cast<size_t>(settings.writer_index)<=count) {
        encoder=static_cast<size_t>(settings.writer_index)-1;
        extension=PrimaryEncoderExtension(library->EncoderFactories()[encoder].extension);
    } else if (configuration.lame && static_cast<size_t>(settings.writer_index)==count+1) {
        encoder=kLameConversionEncoder;
        extension=L"mp3";
    }
    auto state=std::make_shared<ConvertProgressState>();
    state->resources=resources;
    state->ttpcomm=RetainModule(ttpcomm);
    state->library=library->RetainForBackground();
    if (!state->library || (ttpcomm && !state->ttpcomm)) {
        ShowResourceError(owner,resources,0x814e);
        return false;
    }
    state->encoder_index=encoder;
    state->settings=settings;
    state->equalizer=equalizer;
    state->completed=std::move(completed);
    state->tracks=std::move(tracks);
    state->display_titles=std::move(display_titles);
    for (size_t index=0;index<state->tracks.size();++index) {
        const auto& track=state->tracks[index];
        auto folder=settings.folder.empty() ? track.path.parent_path() : settings.folder;
        audio::ArchiveMemberPath member;
        if (settings.folder.empty() && audio::ParseArchiveMemberPath(track.path.wstring(),member))
            folder=member.archive.parent_path();
        // An empty creator extension is valid for ttp_clienc: slot 7 on the
        // created encoder supplies the configured preset's real extension.
        state->destinations.push_back(MakeOutputPath(folder,track,extension,
                                                      settings.add_number!=0,index+1));
    }
    state->results.resize(state->tracks.size());
    for (auto& result:state->results) result.result=kConversionPending;
    auto* holder=new ConvertLifetime(state);
    const HWND dialog=CreateDialogParamW(resources,MAKEINTRESOURCEW(kConvertProgressDialog),
        owner,ConvertProgressProc,reinterpret_cast<LPARAM>(holder));
    if (!dialog) {
        // WM_NCDESTROY normally consumes the holder after WM_INITDIALOG.
        // A template lookup failure never delivers either message.
        if (!state->dialog_initialized) delete holder;
        return false;
    }
    ShowWindow(dialog,SW_SHOWNORMAL);
    return true;
}

bool TranslatePlaylistConverterMessage(MSG& message) {
    return g_convert_dialog && IsWindow(g_convert_dialog) &&
        (message.hwnd==g_convert_dialog || IsChild(g_convert_dialog,message.hwnd)) &&
        IsDialogMessageW(g_convert_dialog,&message);
}

void ClosePlaylistConverter(HWND owner) {
    if (g_convert_dialog && IsWindow(g_convert_dialog) &&
        GetWindow(g_convert_dialog,GW_OWNER)==owner)
        SendMessageW(g_convert_dialog,WM_CLOSE,0,0);
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
