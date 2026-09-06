#include "player_window_internal.h"
#include "file_info_cover_policy.h"
#include "file_info_probe_protocol.h"
#include "file_info_probe_client.h"
#include "file_info_mp3_policy.h"
#include "modern_file_dialog.h"

#include "ttplayer/audio/archive_member.h"
#include "ttplayer/core/text.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <commctrl.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <prsht.h>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <wincodec.h>
#include <shlwapi.h>

namespace ttplayer::ui {
namespace {

constexpr UINT kFileInfoReadComplete = WM_APP + 0x416;
constexpr UINT kFileInfoSaveComplete = WM_APP + 0x417;
constexpr UINT kFileInfoSave = 0x82e;
constexpr UINT kFileInfoReload = 0x855;
constexpr UINT kFileInfoPrevious = 0x835;
constexpr UINT kFileInfoNext = 0x836;
constexpr UINT kFileInfoAdvanced = 0x875;
constexpr UINT kFileInfoPropertiesPage = 372; // ttpres DIALOG 0x174
constexpr UINT kFileInfoCoverPage = 374;      // ttpres DIALOG 0x176

constexpr std::array<int, 7> kTagControls{
    1009, 1021, 1025, 1029, 1030, 1031, 1032};
constexpr std::array<const char*, 7> kTagNames{
    "Title", "Artist", "Album", "Tracknumber", "Genre", "Date",
    "Comment"};

struct FileInfoStrings {
    std::wstring different;
    std::wstring close;
    std::wstring local_song;
    std::wstring network_song;
    std::wstring read_status;
    std::wstring read_cancel_question;
    std::wstring save_status;
    std::wstring save_cancel_question;
    std::wstring save_failure;
    std::wstring readonly_question;
    std::wstring many_title;
    std::wstring indexed_title;
    std::wstring playing_title;
    std::wstring save;
    std::wstring reload;
    std::wstring previous;
    std::wstring next;
    std::wstring advanced;
    std::wstring id3v2_utf8_warning;
    std::vector<std::wstring> channel_names;
};

struct FileInfoRecord {
    size_t row{};
    std::filesystem::path path;
    int subtrack{};
    std::array<std::wstring, 7> tags;
    std::wstring file_name;
    std::wstring song_source;
    std::wstring codec;
    std::wstring channels;
    std::wstring sample_rate;
    std::wstring bits;
    std::wstring bitrate;
    std::wstring duration;
    std::wstring gain;
    std::vector<std::pair<std::string, std::string>> metadata;
    std::vector<unsigned char> cover;
    std::string media_type;
    std::uint32_t bitrate_bps{};
    std::uint32_t sample_rate_hz{};
    int duration_ms{-2};
    bool reader_opened{};
    bool writable{};
};

struct FileInfoCombined {
    std::array<std::wstring, 7> tags;
    std::wstring file_name;
    std::wstring song_source;
    std::wstring codec;
    std::wstring channels;
    std::wstring sample_rate;
    std::wstring bits;
    std::wstring bitrate;
    std::wstring duration;
    std::wstring gain;
    std::vector<unsigned char> cover;
    std::vector<std::pair<std::wstring, std::wstring>> metadata;
    size_t cover_count{};
    bool writable{};
};

struct FileInfoReadResult {
    std::uint64_t generation{};
    std::vector<FileInfoRecord> records;
};

struct FileInfoWriteResult {
    struct Item {
        size_t row{};
        std::vector<std::pair<size_t, std::wstring>> values;
    };
    std::vector<Item> items;
    HRESULT error{S_OK};
};

struct FileInfoContext : std::enable_shared_from_this<FileInfoContext> {
    HWND owner{};
    HWND sheet{};
    HWND properties_page{};
    HWND cover_page{};
    HMODULE resources{};
    std::filesystem::path helper;
    std::filesystem::path addin_directory;
    std::filesystem::path ttpcomm_path;
    settings::GeneralSettings* general_settings{};
    settings::HistorySettings* history_settings{};
    size_t playlist_index{};
    bool library_mode{};
    bool explicit_playback_track{};
    std::optional<playlist::Track> explicit_source;
    std::vector<playlist::Track> tracks;
    std::vector<size_t> rows;
    std::optional<size_t> playing_row;
    FileInfoStrings strings;
    FileInfoCombined combined;
    std::set<size_t> touched_rows;
    std::atomic<HWND> post_target{};
    std::mutex pending_mutex;
    std::unique_ptr<FileInfoReadResult> pending_read;
    std::unique_ptr<FileInfoWriteResult> pending_write;
    std::jthread read_worker;
    std::jthread save_worker;
    std::uint64_t generation{};
    int navigation_after_save{};
    HBITMAP cover_bitmap{};
    detail::FileInfoProbeCoverAction cover_action{
        detail::FileInfoProbeCoverAction::unchanged};
    bool loaded{};
    bool loading{};
    bool saving{};
    bool closing{};
    bool advanced_mode{};

    ~FileInfoContext() {
        if (cover_bitmap) DeleteObject(cover_bitmap);
    }
};

bool AsciiEquals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        char lhs = left[index];
        char rhs = right[index];
        if (lhs >= 'A' && lhs <= 'Z') lhs += 'a' - 'A';
        if (rhs >= 'A' && rhs <= 'Z') rhs += 'a' - 'A';
        if (lhs != rhs) return false;
    }
    return true;
}

bool WideAsciiEquals(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        wchar_t lhs = left[index];
        wchar_t rhs = right[index];
        if (lhs >= L'A' && lhs <= L'Z') lhs += L'a' - L'A';
        if (rhs >= L'A' && rhs <= L'Z') rhs += L'a' - L'A';
        if (lhs != rhs) return false;
    }
    return true;
}

bool LooksLikeNetworkPath(std::wstring_view path) {
    const size_t separator = path.find(L"://");
    return separator != std::wstring_view::npos && separator != 0;
}

std::wstring SafeWide(std::string_view value) {
    if (value.empty()) return {};
    try {
        return core::Utf8ToWide(std::string(value));
    } catch (const std::exception&) {
        return {};
    }
}

std::string SafeUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    try {
        return core::WideToUtf8(value);
    } catch (const std::exception&) {
        return {};
    }
}

std::wstring TrackMetadataValue(const playlist::Track& track,
                                std::string_view name) {
    const auto found = std::find_if(
        track.metadata.begin(), track.metadata.end(),
        [name](const auto& entry) { return AsciiEquals(entry.first, name); });
    return found == track.metadata.end() ? std::wstring{}
                                         : SafeWide(found->second);
}

std::vector<std::wstring> Split(std::wstring_view value, wchar_t separator) {
    std::vector<std::wstring> result;
    size_t begin{};
    while (begin <= value.size()) {
        const size_t end = value.find(separator, begin);
        result.emplace_back(value.substr(
            begin, end == std::wstring_view::npos ? value.size() - begin
                                                   : end - begin));
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return result;
}

std::pair<std::wstring, std::wstring> SplitStatus(std::wstring value) {
    const size_t separator = value.find(L'|');
    if (separator == std::wstring::npos) return {std::move(value), {}};
    auto question = value.substr(separator + 1);
    value.resize(separator);
    return {std::move(value), std::move(question)};
}

std::wstring FormatDuration(int duration_ms) {
    if (duration_ms < 0) return {};
    const auto total = static_cast<unsigned int>(duration_ms / 1000);
    const unsigned int hours = total / 3600;
    const unsigned int minutes = total / 60 % 60;
    const unsigned int seconds = total % 60;
    wchar_t value[32]{};
    if (hours != 0)
        swprintf_s(value, L"%u:%02u:%02u", hours, minutes, seconds);
    else
        swprintf_s(value, L"%u:%02u", total / 60, seconds);
    return value;
}

std::wstring FormatChannels(WORD channels,
                            const std::vector<std::wstring>& names) {
    if (channels != 0 && channels <= names.size())
        return names[channels - 1];
    return channels == 0 ? std::wstring{} : std::to_wstring(channels);
}

std::wstring ProbeMetadataValue(
    const detail::FileInfoProbeReadResult& result,
    std::wstring_view name) {
    const auto found = std::find_if(result.metadata.begin(),
        result.metadata.end(), [name](const auto& entry) {
            return WideAsciiEquals(entry.name, name);
        });
    return found == result.metadata.end() ? std::wstring{} : found->value;
}

detail::FileInfoProbeMp3Policy ProbeMp3Policy(
    const settings::GeneralSettings* settings) noexcept {
    detail::FileInfoProbeMp3Policy policy;
    if (!settings) return policy;
    policy.read_priority = static_cast<std::uint32_t>(
        settings->mp3_read_tag_priority);
    policy.write_type = static_cast<std::uint32_t>(
        settings->mp3_write_tag_type);
    policy.id3v2_encoding = static_cast<std::uint32_t>(
        settings->mp3_id3v2_encoding);
    policy.id3v2_padding = settings->mp3_id3v2_padding ? 1U : 0U;
    return policy;
}

FileInfoRecord ReadFileInfoRecord(
    size_t row, const playlist::Track& track,
    const std::filesystem::path& helper,
    const std::filesystem::path& addin_directory,
    const std::filesystem::path& ttpcomm_path,
    const FileInfoStrings& strings,
    const detail::FileInfoProbeMp3Policy& mp3_policy,
    std::stop_token stop) {
    FileInfoRecord record;
    record.row = row;
    record.path = track.path;
    record.subtrack = track.subtrack;
    record.file_name = track.path.wstring();
    record.song_source = LooksLikeNetworkPath(record.file_name)
        ? strings.network_song : strings.local_song;
    record.tags[0] = SafeWide(track.title);
    record.tags[1] = SafeWide(track.artist);
    record.tags[2] = SafeWide(track.album);
    record.tags[3] = track.track_number > 0
        ? std::to_wstring(track.track_number)
        : TrackMetadataValue(track, "Tracknumber");
    record.tags[4] = TrackMetadataValue(track, "Genre");
    record.tags[5] = TrackMetadataValue(track, "Date");
    if (record.tags[5].empty())
        record.tags[5] = TrackMetadataValue(track, "Year");
    record.tags[6] = TrackMetadataValue(track, "Comment");
    record.metadata = track.metadata;
    record.media_type = track.media_type;
    record.bitrate_bps = track.bitrate_bps;
    record.sample_rate_hz = track.sample_rate_hz;
    record.duration_ms = track.duration_ms;
    record.codec = SafeWide(track.media_type);
    if (track.sample_rate_hz != 0)
        record.sample_rate = std::to_wstring(track.sample_rate_hz) + L" Hz";
    if ((track.bitrate_bps & 0x7fffffffU) != 0)
        record.bitrate = std::to_wstring(
            (track.bitrate_bps & 0x7fffffffU) / 1000U) + L" Kbps";
    record.duration = FormatDuration(track.duration_ms);
    record.gain = TrackMetadataValue(track, "replaygain_track_gain");

    if (record.path.empty() || LooksLikeNetworkPath(record.path.native()) ||
        stop.stop_requested()) {
        return record;
    }

    const auto probe = detail::RunFileInfoReadProbe(
        stop, helper, addin_directory, record.path, ttpcomm_path, 15000,
        nullptr, mp3_policy);
    if (!probe || FAILED(probe->status) || stop.stop_requested()) return record;
    record.reader_opened = true;
    audio::ArchiveMemberPath archive_member;
    record.writable = (probe->capabilities & 4U) != 0 &&
        !audio::ParseArchiveMemberPath(record.path.native(), archive_member);

    static constexpr std::array<std::wstring_view, 7> names{
        L"Title", L"Artist", L"Album", L"Tracknumber", L"Genre", L"Date",
        L"Comment"};
    for (size_t index = 0; index < names.size(); ++index) {
        auto value = ProbeMetadataValue(*probe, names[index]);
        if (index == 1 && value.empty())
            value = ProbeMetadataValue(*probe, L"Author");
        if (index == 5 && value.empty())
            value = ProbeMetadataValue(*probe, L"Year");
        if (!value.empty()) record.tags[index] = std::move(value);
    }

    record.metadata.clear();
    record.metadata.reserve(probe->metadata.size());
    for (const auto& entry : probe->metadata) {
        const auto name = SafeUtf8(entry.name);
        if (!name.empty())
            record.metadata.emplace_back(name, SafeUtf8(entry.value));
    }
    const WAVEFORMATEX& format = probe->format;
    record.codec = probe->codec;
    record.channels = FormatChannels(format.nChannels, strings.channel_names);
    if (format.nSamplesPerSec != 0)
        record.sample_rate = std::to_wstring(format.nSamplesPerSec) + L" Hz";
    if (format.wBitsPerSample != 0)
        record.bits = std::to_wstring(format.wBitsPerSample) + L" bit";
    const std::uint64_t encoded = probe->encoded_bits_per_second != 0
        ? probe->encoded_bits_per_second
        : static_cast<std::uint64_t>(format.nAvgBytesPerSec) * 8U;
    if (encoded != 0)
        record.bitrate = std::to_wstring(encoded / 1000U) + L" Kbps";
    record.duration_ms = static_cast<int>(std::min<DWORD>(
        probe->duration_ms, static_cast<DWORD>(INT_MAX)));
    record.duration = FormatDuration(record.duration_ms);
    record.gain = ProbeMetadataValue(*probe, L"replaygain_track_gain");
    record.cover = probe->cover;
    record.media_type = SafeUtf8(record.codec);
    record.bitrate_bps = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(encoded, 0x7fffffffU));
    record.sample_rate_hz = format.nSamplesPerSec;
    return record;
}

void CombineValue(std::wstring& destination, std::wstring_view value,
                  std::wstring_view different, bool first,
                  bool insensitive = false) {
    if (first) {
        destination = value;
        return;
    }
    const bool equal = insensitive
        ? _wcsicmp(destination.c_str(), std::wstring(value).c_str()) == 0
        : destination == value;
    if (!equal) destination = different;
}

FileInfoCombined CombineRecords(const std::vector<FileInfoRecord>& records,
                                const FileInfoStrings& strings) {
    FileInfoCombined combined;
    for (size_t record_index = 0; record_index < records.size(); ++record_index) {
        const auto& record = records[record_index];
        const bool first = record_index == 0;
        for (size_t field = 0; field < combined.tags.size(); ++field)
            CombineValue(combined.tags[field], record.tags[field],
                         strings.different, first);
        CombineValue(combined.file_name, record.file_name, strings.different,
                     first, true);
        CombineValue(combined.song_source, record.song_source,
                     strings.different, first, true);
        CombineValue(combined.codec, record.codec, strings.different, first);
        CombineValue(combined.channels, record.channels, strings.different,
                     first);
        CombineValue(combined.sample_rate, record.sample_rate,
                     strings.different, first);
        CombineValue(combined.bits, record.bits, strings.different, first);
        CombineValue(combined.bitrate, record.bitrate, strings.different,
                     first);
        CombineValue(combined.duration, record.duration, strings.different,
                     first);
        CombineValue(combined.gain, record.gain, strings.different, first);
        combined.writable = combined.writable || record.writable;
        if (!record.cover.empty()) {
            ++combined.cover_count;
            if (combined.cover.empty()) combined.cover = record.cover;
        }
    }
    std::vector<std::wstring> metadata_names;
    for (const auto& record : records) {
        for (const auto& entry : record.metadata) {
            const auto name = SafeWide(entry.first);
            if (name.empty()) continue;
            const auto existing = std::find_if(
                metadata_names.begin(), metadata_names.end(),
                [&name](const auto& item) {
                    return WideAsciiEquals(item, name);
                });
            if (existing == metadata_names.end())
                metadata_names.push_back(name);
        }
    }
    for (const auto& name : metadata_names) {
        std::wstring value;
        for (size_t record_index = 0; record_index < records.size();
             ++record_index) {
            const auto& metadata = records[record_index].metadata;
            const auto found = std::find_if(metadata.begin(), metadata.end(),
                [&name](const auto& entry) {
                    return WideAsciiEquals(SafeWide(entry.first), name);
                });
            CombineValue(value, found == metadata.end()
                ? std::wstring_view{} : std::wstring_view(SafeWide(found->second)),
                strings.different, record_index == 0);
        }
        combined.metadata.emplace_back(name, std::move(value));
    }
    return combined;
}

HBITMAP DecodeCoverBitmap(const std::vector<unsigned char>& bytes,
                          int target_width, int target_height) {
    if (bytes.empty() || target_width <= 0 || target_height <= 0) return nullptr;
    IWICImagingFactory* factory{};
    IWICStream* stream{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICFormatConverter* converter{};
    IWICBitmapScaler* scaler{};
    HBITMAP bitmap{};
    void* dib_bits{};

    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result) &&
        bytes.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max()))
        result = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    if (SUCCEEDED(result)) result = stream->InitializeFromMemory(
        const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()));
    if (SUCCEEDED(result)) result = factory->CreateDecoderFromStream(
        stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) result = converter->Initialize(
        frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
        0.0, WICBitmapPaletteTypeCustom);

    UINT source_width{};
    UINT source_height{};
    if (SUCCEEDED(result)) result = converter->GetSize(&source_width,
                                                       &source_height);
    UINT width{};
    UINT height{};
    if (SUCCEEDED(result) && source_width != 0 && source_height != 0) {
        const double scale = std::min(
            static_cast<double>(target_width) / source_width,
            static_cast<double>(target_height) / source_height);
        width = std::max<UINT>(1, static_cast<UINT>(source_width * scale));
        height = std::max<UINT>(1, static_cast<UINT>(source_height * scale));
        result = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(result))
            result = scaler->Initialize(converter, width, height,
                                         WICBitmapInterpolationModeFant);
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = target_width;
    info.bmiHeader.biHeight = -target_height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (SUCCEEDED(result)) {
        const HDC screen = GetDC(nullptr);
        bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &dib_bits,
                                  nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!bitmap || !dib_bits) result = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(result)) {
        const COLORREF background = GetSysColor(COLOR_WINDOW);
        const std::uint32_t pixel = static_cast<std::uint32_t>(
            GetBValue(background) | (GetGValue(background) << 8) |
            (GetRValue(background) << 16) | 0xff000000U);
        auto* destination = static_cast<std::uint32_t*>(dib_bits);
        std::fill(destination,
                  destination + static_cast<size_t>(target_width) * target_height,
                  pixel);
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4U);
        result = scaler->CopyPixels(nullptr, width * 4U,
                                    static_cast<UINT>(pixels.size()),
                                    pixels.data());
        if (SUCCEEDED(result)) {
            const int left = (target_width - static_cast<int>(width)) / 2;
            const int top = (target_height - static_cast<int>(height)) / 2;
            for (UINT row = 0; row < height; ++row) {
                std::memcpy(destination +
                    static_cast<size_t>(top + static_cast<int>(row)) *
                        target_width + left,
                    pixels.data() + static_cast<size_t>(row) * width * 4U,
                    static_cast<size_t>(width) * 4U);
            }
        }
    }

    if (scaler) scaler->Release();
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (FAILED(result) && bitmap) {
        DeleteObject(bitmap);
        bitmap = nullptr;
    }
    return bitmap;
}

std::optional<std::vector<unsigned char>> ReadCoverBytes(
    const std::filesystem::path& path) {
    if (!detail::IsSupportedCoverPath(path)) return std::nullopt;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    bool okay = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
        size.QuadPart <= detail::kFileInfoProbeMaximumCoverBytes;
    std::vector<unsigned char> bytes;
    if (okay) {
        try { bytes.resize(static_cast<size_t>(size.QuadPart)); }
        catch (...) { okay = false; }
    }
    size_t offset{};
    while (okay && offset < bytes.size()) {
        DWORD read{};
        const DWORD wanted = static_cast<DWORD>(std::min<size_t>(
            bytes.size() - offset, static_cast<size_t>(MAXDWORD)));
        if (!ReadFile(file, bytes.data() + offset, wanted, &read, nullptr) ||
            read == 0) {
            okay = false;
            break;
        }
        offset += read;
    }
    CloseHandle(file);
    // Validate the payload before it reaches either the built-in APIC writer
    // or an old third-party ISoundThumbnail implementation.
    if (!okay || detail::CoverMimeType(bytes).empty()) return std::nullopt;
    return bytes;
}

void SetDialogText(HWND dialog, int identifier, const std::wstring& value) {
    if (dialog) SetDlgItemTextW(dialog, identifier, value.c_str());
}

template <size_t Count>
void PopulateMp3PolicyCombo(
    HWND combo, const std::array<detail::Mp3TagPolicyChoice, Count>& choices,
    std::uint32_t selected_value, bool select_first_when_unknown) {
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int selected = -1;
    for (size_t index = 0; index < choices.size(); ++index) {
        const auto& choice = choices[index];
        const LRESULT item = SendMessageW(combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(choice.label.data()));
        if (item == CB_ERR || item == CB_ERRSPACE) continue;
        SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(item),
                     static_cast<LPARAM>(choice.value));
        if (choice.value == selected_value) selected = static_cast<int>(item);
    }
    if (selected < 0 && select_first_when_unknown && !choices.empty())
        selected = 0;
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}

void EnableMp3Id3v2PolicyControls(FileInfoContext& context) {
    if (!context.properties_page || !context.general_settings) return;
    const BOOL enabled = detail::Mp3WriteTypeIncludesId3v2(
        static_cast<std::uint32_t>(
            context.general_settings->mp3_write_tag_type));
    EnableWindow(GetDlgItem(context.properties_page, 2174), enabled);
    EnableWindow(GetDlgItem(context.properties_page, 2175), enabled);
}

void PopulateMp3PolicyControls(FileInfoContext& context) {
    if (!context.properties_page || !context.general_settings) return;
    const auto& general = *context.general_settings;
    PopulateMp3PolicyCombo(GetDlgItem(context.properties_page, 2163),
        detail::kMp3ReadPriorityChoices,
        static_cast<std::uint32_t>(general.mp3_read_tag_priority), true);
    PopulateMp3PolicyCombo(GetDlgItem(context.properties_page, 2161),
        detail::kMp3WriteTypeChoices,
        static_cast<std::uint32_t>(general.mp3_write_tag_type), false);
    PopulateMp3PolicyCombo(GetDlgItem(context.properties_page, 2174),
        detail::kMp3Id3v2EncodingChoices,
        static_cast<std::uint32_t>(general.mp3_id3v2_encoding), false);
    SendDlgItemMessageW(context.properties_page, 2174, CB_SETCURSEL,
        static_cast<WPARAM>(detail::Mp3Id3v2EncodingSelection(
            static_cast<std::uint32_t>(general.mp3_id3v2_encoding))), 0);
    CheckDlgButton(context.properties_page, 2175,
        general.mp3_id3v2_padding ? BST_CHECKED : BST_UNCHECKED);
    EnableMp3Id3v2PolicyControls(context);
}

std::optional<std::uint32_t> SelectedMp3PolicyValue(HWND dialog,
                                                    int identifier) {
    const HWND combo = GetDlgItem(dialog, identifier);
    if (!combo) return std::nullopt;
    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) return std::nullopt;
    const LRESULT value = SendMessageW(
        combo, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
    if (value == CB_ERR) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

void PopulateAdvancedMetadata(FileInfoContext& context) {
    const HWND list = GetDlgItem(context.properties_page, 2164);
    if (!list) return;
    ListView_SetExtendedListViewStyleEx(
        list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const HWND header = ListView_GetHeader(list);
    if (!header || Header_GetItemCount(header) == 0) {
        RECT bounds{};
        GetClientRect(list, &bounds);
        LVCOLUMNW column{};
        column.mask = LVCF_WIDTH | LVCF_TEXT;
        wchar_t empty[] = L"";
        column.pszText = empty;
        column.cx = std::max<LONG>(70, (bounds.right - bounds.left) / 3);
        ListView_InsertColumn(list, 0, &column);
        column.cx = std::max<LONG>(
            80, bounds.right - bounds.left - column.cx - 4);
        ListView_InsertColumn(list, 1, &column);
    }
    ListView_DeleteAllItems(list);
    for (size_t index = 0; index < context.combined.metadata.size(); ++index) {
        const auto& [name, value] = context.combined.metadata[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(name.c_str());
        const int inserted = ListView_InsertItem(list, &item);
        if (inserted >= 0) {
            LVITEMW subitem{};
            subitem.iSubItem = 1;
            subitem.pszText = const_cast<wchar_t*>(value.c_str());
            SendMessageW(list, LVM_SETITEMTEXTW,
                         static_cast<WPARAM>(inserted),
                         reinterpret_cast<LPARAM>(&subitem));
        }
    }
}

void SetAdvancedMetadataMode(FileInfoContext& context, bool enabled) {
    context.advanced_mode = enabled;
    const HWND list = GetDlgItem(context.properties_page, 2164);
    for (const int identifier : kTagControls)
        ShowWindow(GetDlgItem(context.properties_page, identifier),
                   enabled ? SW_HIDE : SW_SHOW);
    if (list) {
        ShowWindow(list, enabled ? SW_SHOW : SW_HIDE);
        if (enabled)
            SetWindowPos(list, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    CheckDlgButton(context.properties_page, kFileInfoAdvanced,
                   enabled ? BST_CHECKED : BST_UNCHECKED);
}

void CreateAdvancedMetadataButton(FileInfoContext& context) {
    if (!context.properties_page ||
        GetDlgItem(context.properties_page, kFileInfoAdvanced)) return;
    RECT bounds{151, 140, 207, 155};
    MapDialogRect(context.properties_page, &bounds);
    const HWND button = CreateWindowExW(
        0, WC_BUTTONW, context.strings.advanced.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        bounds.left, bounds.top, bounds.right - bounds.left,
        bounds.bottom - bounds.top, context.properties_page,
        reinterpret_cast<HMENU>(
            static_cast<UINT_PTR>(kFileInfoAdvanced)), nullptr, nullptr);
    if (button) {
        const HFONT font = reinterpret_cast<HFONT>(SendDlgItemMessageW(
            context.properties_page, 1009, WM_GETFONT, 0, 0));
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void PopulatePropertiesPage(FileInfoContext& context) {
    const HWND page = context.properties_page;
    if (!page || !context.loaded) return;
    for (size_t index = 0; index < kTagControls.size(); ++index)
        SetDialogText(page, kTagControls[index], context.combined.tags[index]);
    SetDialogText(page, 2001, context.combined.file_name);
    SetDialogText(page, 2280, context.combined.song_source);
    SetDialogText(page, 1018, context.combined.codec);
    SetDialogText(page, 1003, context.combined.channels);
    SetDialogText(page, 1001, context.combined.sample_rate);
    SetDialogText(page, 1007, context.combined.bits);
    SetDialogText(page, 1105, context.combined.bitrate);
    SetDialogText(page, 1013, context.combined.duration);
    SetDialogText(page, 1048, context.combined.gain);
    PopulateAdvancedMetadata(context);

    for (const int identifier : kTagControls)
        EnableWindow(GetDlgItem(page, identifier), context.combined.writable);
    SetAdvancedMetadataMode(context, context.advanced_mode);
}

void PopulateCoverPage(FileInfoContext& context) {
    const HWND page = context.cover_page;
    if (!page || !context.loaded) return;
    const HWND picture = GetDlgItem(page, 2222);
    if (!picture) return;
    if (context.cover_bitmap) {
        SendMessageW(picture, STM_SETIMAGE, IMAGE_BITMAP, 0);
        DeleteObject(context.cover_bitmap);
        context.cover_bitmap = nullptr;
    }
    RECT bounds{};
    GetClientRect(picture, &bounds);
    context.cover_bitmap = DecodeCoverBitmap(
        context.combined.cover, bounds.right, bounds.bottom);
    if (context.cover_bitmap) {
        const LONG_PTR style = GetWindowLongPtrW(picture, GWL_STYLE);
        SetWindowLongPtrW(picture, GWL_STYLE,
                          (style & ~SS_TYPEMASK) | SS_BITMAP | SS_CENTERIMAGE);
        SendMessageW(picture, STM_SETIMAGE, IMAGE_BITMAP,
                     reinterpret_cast<LPARAM>(context.cover_bitmap));
    } else {
        InvalidateRect(picture, nullptr, TRUE);
    }
    const bool ready = context.loaded && !context.loading && !context.saving &&
                       context.combined.writable;
    EnableWindow(GetDlgItem(page, 2220), ready);
    EnableWindow(GetDlgItem(page, 2221),
                 ready && !context.combined.cover.empty());
}

std::wstring FormatFileInfoTitle(const FileInfoContext& context) {
    wchar_t title[256]{};
    if (context.rows.size() > 1) {
        const auto& format = context.strings.many_title;
        if (!format.empty())
            swprintf_s(title, format.c_str(),
                       static_cast<int>(context.rows.size()));
    } else if (!context.rows.empty()) {
        if (context.playing_row && *context.playing_row == context.rows.front())
            return context.strings.playing_title;
        const auto& format = context.strings.indexed_title;
        if (!format.empty())
            swprintf_s(title, format.c_str(),
                       static_cast<int>(context.rows.front() + 1),
                       static_cast<int>(context.tracks.size()));
    }
    return title;
}

void UpdateSheetState(FileInfoContext& context) {
    if (!context.sheet) return;
    const auto title = context.loading ? context.strings.read_status
        : context.saving ? context.strings.save_status
                         : FormatFileInfoTitle(context);
    if (!title.empty()) SetWindowTextW(context.sheet, title.c_str());
    const bool ready = context.loaded && !context.loading && !context.saving;
    if (context.properties_page) {
        for (const int identifier : kTagControls)
            EnableWindow(GetDlgItem(context.properties_page, identifier),
                         ready && context.combined.writable);
        EnableWindow(GetDlgItem(context.properties_page, kFileInfoAdvanced),
                     ready);
        EnableWindow(GetDlgItem(context.properties_page, 2164), ready);
    }
    if (context.cover_page) {
        EnableWindow(GetDlgItem(context.cover_page, 2220),
                     ready && context.combined.writable);
        EnableWindow(GetDlgItem(context.cover_page, 2221),
                     ready && context.combined.writable &&
                         !context.combined.cover.empty());
    }
    EnableWindow(GetDlgItem(context.sheet, kFileInfoSave),
                 ready && context.combined.writable);
    EnableWindow(GetDlgItem(context.sheet, kFileInfoReload), ready);
    const bool single = ready && context.rows.size() == 1;
    const size_t row = single ? context.rows.front() : 0;
    EnableWindow(GetDlgItem(context.sheet, kFileInfoPrevious),
                 single && row > 0);
    EnableWindow(GetDlgItem(context.sheet, kFileInfoNext),
                 single && row + 1 < context.tracks.size());
}

void ApplyReadResult(FileInfoContext& context, FileInfoReadResult result) {
    if (result.generation != context.generation || context.closing) return;
    context.combined = CombineRecords(result.records, context.strings);
    context.cover_action = detail::FileInfoProbeCoverAction::unchanged;
    for (const auto& record : result.records) {
        if (record.row >= context.tracks.size() || !record.reader_opened)
            continue;
        auto& track = context.tracks[record.row];
        track.title = SafeUtf8(record.tags[0]);
        track.artist = SafeUtf8(record.tags[1]);
        track.album = SafeUtf8(record.tags[2]);
        const long number = std::wcstol(record.tags[3].c_str(), nullptr, 10);
        track.track_number = number > 0 && number <= INT_MAX
            ? static_cast<int>(number) : 0;
        track.metadata = record.metadata;
        track.media_type = record.media_type;
        track.bitrate_bps = record.bitrate_bps;
        track.sample_rate_hz = record.sample_rate_hz;
        track.duration_ms = record.duration_ms;
        context.touched_rows.insert(record.row);
    }
    context.loaded = true;
    context.loading = false;
    PopulatePropertiesPage(context);
    PopulateCoverPage(context);
    UpdateSheetState(context);
}

void BeginRead(const std::shared_ptr<FileInfoContext>& context) {
    if (!context || context->closing || context->loading || context->saving)
        return;
    if (context->read_worker.joinable()) context->read_worker.join();
    context->loaded = false;
    context->loading = true;
    const std::uint64_t generation = ++context->generation;
    UpdateSheetState(*context);
    const auto rows = context->rows;
    const auto tracks = context->tracks;
    const auto strings = context->strings;
    const auto helper = context->helper;
    const auto addin_directory = context->addin_directory;
    const auto ttpcomm_path = context->ttpcomm_path;
    const auto mp3_policy = ProbeMp3Policy(context->general_settings);
    context->read_worker = std::jthread(
        [context, generation, rows, tracks, strings, helper,
         addin_directory, ttpcomm_path, mp3_policy]
        (std::stop_token stop) {
            const HRESULT initialized = CoInitializeEx(
                nullptr, COINIT_MULTITHREADED);
            std::unique_ptr<FileInfoReadResult> result;
            try {
                result = std::make_unique<FileInfoReadResult>();
                result->generation = generation;
                result->records.reserve(rows.size());
                for (const size_t row : rows) {
                    if (stop.stop_requested()) break;
                    if (row < tracks.size())
                        result->records.push_back(ReadFileInfoRecord(
                            row, tracks[row], helper, addin_directory,
                            ttpcomm_path, strings, mp3_policy, stop));
                }
            } catch (const std::exception&) {
                result = std::make_unique<FileInfoReadResult>();
                result->generation = generation;
            }
            if (SUCCEEDED(initialized)) CoUninitialize();
            if (stop.stop_requested()) return;
            if (!result) return;
            {
                const std::scoped_lock lock(context->pending_mutex);
                context->pending_read = std::move(result);
            }
            const HWND target = context->post_target.load(
                std::memory_order_acquire);
            if (!target || !IsWindow(target) ||
                !PostMessageW(target, kFileInfoReadComplete,
                              static_cast<WPARAM>(generation), 0)) {
                const std::scoped_lock lock(context->pending_mutex);
                context->pending_read.reset();
            }
        });
}

std::wstring GetControlText(HWND dialog, int identifier) {
    const HWND control = GetDlgItem(dialog, identifier);
    if (!control) return {};
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<size_t>(length));
    return result;
}

void MergeTrackMetadata(playlist::Track& track, std::string_view name,
                        std::wstring_view value) {
    const auto found = std::find_if(
        track.metadata.begin(), track.metadata.end(),
        [name](const auto& entry) { return AsciiEquals(entry.first, name); });
    const auto utf8 = SafeUtf8(value);
    if (found == track.metadata.end()) {
        if (!utf8.empty()) track.metadata.emplace_back(std::string(name), utf8);
    } else if (utf8.empty()) {
        track.metadata.erase(found);
    } else {
        found->second = utf8;
    }
    if (AsciiEquals(name, "Title")) track.title = utf8;
    else if (AsciiEquals(name, "Artist")) track.artist = utf8;
    else if (AsciiEquals(name, "Album")) track.album = utf8;
    else if (AsciiEquals(name, "Tracknumber")) {
        const long number = std::wcstol(std::wstring(value).c_str(), nullptr, 10);
        track.track_number = number > 0 && number <= INT_MAX
            ? static_cast<int>(number) : 0;
    }
}

bool MakeFilesWritable(FileInfoContext& context) {
    for (const size_t row : context.rows) {
        if (row >= context.tracks.size()) continue;
        const auto& path = context.tracks[row].path;
        audio::ArchiveMemberPath member;
        if (LooksLikeNetworkPath(path.native()) ||
            audio::ParseArchiveMemberPath(path.native(), member)) continue;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_READONLY) == 0) continue;
        if (MessageBoxW(context.sheet, context.strings.readonly_question.c_str(),
                        FormatFileInfoTitle(context).c_str(),
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return false;
        if (!SetFileAttributesW(path.c_str(),
                                attributes & ~FILE_ATTRIBUTE_READONLY))
            return false;
    }
    return true;
}

std::vector<std::pair<size_t, std::wstring>> CollectFileInfoChanges(
    const FileInfoContext& context) {
    std::vector<std::pair<size_t, std::wstring>> changes;
    for (size_t field = 0; field < kTagControls.size(); ++field) {
        const auto value = GetControlText(context.properties_page,
                                          kTagControls[field]);
        if (value != context.combined.tags[field])
            changes.emplace_back(field, value);
    }
    return changes;
}

std::optional<detail::FileInfoProbeWriteResult> RunFileInfoWriteProbe(
    std::stop_token stop, const std::filesystem::path& helper,
    const std::filesystem::path& addin_directory,
    const std::filesystem::path& logical_path,
    const std::filesystem::path& ttpcomm_path,
    const std::vector<std::pair<size_t, std::wstring>>& changes,
    detail::FileInfoProbeCoverAction cover_action,
    const std::vector<unsigned char>& cover,
    const detail::FileInfoProbeMp3Policy& mp3_policy) {
    const auto request_path = detail::ProbeTemporaryFile();
    const auto output_path = detail::ProbeTemporaryFile();
    if (request_path.empty() || output_path.empty()) {
        if (!request_path.empty()) DeleteFileW(request_path.c_str());
        if (!output_path.empty()) DeleteFileW(output_path.c_str());
        return std::nullopt;
    }
    detail::FileInfoProbeWriteRequest request;
    request.mp3 = mp3_policy;
    request.fields.reserve(changes.size());
    for (const auto& [field, value] : changes) {
        if (field < kTagNames.size())
            request.fields.push_back({kTagNames[field], value});
    }
    request.cover_action = cover_action;
    if (cover_action == detail::FileInfoProbeCoverAction::replace)
        request.cover = cover;
    const bool encoded = detail::WriteFileInfoProbeWriteRequest(
        request_path, request);
    const bool completed = encoded && detail::RunFileInfoProbe(stop, helper,
        {L"write", addin_directory.wstring(), logical_path.wstring(),
         ttpcomm_path.wstring(), request_path.wstring(),
         output_path.wstring()}, 20000).Succeeded();
    detail::FileInfoProbeWriteResult result;
    const bool decoded = completed &&
        detail::ReadFileInfoProbeWriteResult(output_path, result);
    DeleteFileW(request_path.c_str());
    DeleteFileW(output_path.c_str());
    if (!decoded) return std::nullopt;
    return result;
}

void BeginSave(
    const std::shared_ptr<FileInfoContext>& context,
    std::vector<std::pair<size_t, std::wstring>> changes,
    int navigation_after_save) {
    if (!context ||
        (changes.empty() && context->cover_action ==
                              detail::FileInfoProbeCoverAction::unchanged) ||
        !context->loaded || context->loading || context->saving ||
        context->closing) return;

    context->saving = true;
    context->navigation_after_save = navigation_after_save;
    UpdateSheetState(*context);
    const auto rows = context->rows;
    const auto tracks = context->tracks;
    const auto helper = context->helper;
    const auto addin_directory = context->addin_directory;
    const auto ttpcomm_path = context->ttpcomm_path;
    const auto mp3_policy = ProbeMp3Policy(context->general_settings);
    const auto cover_action = context->cover_action;
    const auto cover = context->combined.cover;
    context->save_worker = std::jthread(
        [context, rows, tracks, changes = std::move(changes), helper,
         addin_directory, ttpcomm_path, mp3_policy, cover_action, cover]
        (std::stop_token stop) {
        const HRESULT initialized = CoInitializeEx(nullptr,
                                                   COINIT_MULTITHREADED);
        auto result = std::make_unique<FileInfoWriteResult>();
        for (const size_t row : rows) {
            if (stop.stop_requested()) break;
            if (row >= tracks.size()) continue;
            audio::ArchiveMemberPath member;
            if (LooksLikeNetworkPath(tracks[row].path.native()) ||
                audio::ParseArchiveMemberPath(tracks[row].path.native(), member)) {
                if (SUCCEEDED(result->error)) result->error = E_ACCESSDENIED;
                continue;
            }
            const auto written = RunFileInfoWriteProbe(
                stop, helper, addin_directory, tracks[row].path,
                ttpcomm_path, changes, cover_action, cover, mp3_policy);
            if (!written) {
                if (!stop.stop_requested() && SUCCEEDED(result->error))
                    result->error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                continue;
            }
            FileInfoWriteResult::Item item;
            item.row = row;
            if (FAILED(written->status) && SUCCEEDED(result->error))
                result->error = written->status;
            if (written->fields.size() != changes.size() &&
                SUCCEEDED(result->error))
                result->error = E_UNEXPECTED;
            const size_t count = std::min(written->fields.size(),
                                          changes.size());
            for (size_t index = 0; index < count; ++index) {
                if (SUCCEEDED(written->fields[index]))
                    item.values.push_back(changes[index]);
                else if (SUCCEEDED(result->error))
                    result->error = written->fields[index];
            }
            if (cover_action !=
                    detail::FileInfoProbeCoverAction::unchanged &&
                FAILED(written->cover_status) && SUCCEEDED(result->error)) {
                result->error = written->cover_status;
            }
            if (!item.values.empty())
                result->items.push_back(std::move(item));
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        if (stop.stop_requested()) return;
        {
            const std::scoped_lock lock(context->pending_mutex);
            context->pending_write = std::move(result);
        }
        const HWND target = context->post_target.load(
            std::memory_order_acquire);
        if (!target || !IsWindow(target) ||
            !PostMessageW(target, kFileInfoSaveComplete, 0, 0)) {
            const std::scoped_lock lock(context->pending_mutex);
            context->pending_write.reset();
        }
    });
}

void ApplyWriteResult(FileInfoContext& context,
                      FileInfoWriteResult result) {
    context.saving = false;
    for (const auto& item : result.items) {
        if (item.row >= context.tracks.size()) continue;
        auto& track = context.tracks[item.row];
        for (const auto& [field, value] : item.values) {
            if (field < kTagNames.size())
                MergeTrackMetadata(track, kTagNames[field], value);
        }
        context.touched_rows.insert(item.row);
    }
    const int navigation = std::exchange(context.navigation_after_save, 0);
    if (FAILED(result.error)) {
        MessageBoxW(context.sheet, context.strings.save_failure.c_str(),
                    FormatFileInfoTitle(context).c_str(),
                    MB_OK | MB_ICONERROR);
        UpdateSheetState(context);
        return;
    }
    if (navigation < 0 && context.rows.size() == 1 &&
        context.rows.front() > 0) {
        --context.rows.front();
    } else if (navigation > 0 && context.rows.size() == 1 &&
               context.rows.front() + 1 < context.tracks.size()) {
        ++context.rows.front();
    }
    BeginRead(context.shared_from_this());
}

void NavigateFileInfo(const std::shared_ptr<FileInfoContext>& context,
                      int direction) {
    if (!context || context->rows.size() != 1 || context->loading ||
        context->saving || !context->properties_page) return;
    const size_t current = context->rows.front();
    if ((direction < 0 && current == 0) ||
        (direction > 0 && current + 1 >= context->tracks.size()))
        return;
    auto changes = CollectFileInfoChanges(*context);
    if (!changes.empty() || context->cover_action !=
                                detail::FileInfoProbeCoverAction::unchanged) {
        if (MakeFilesWritable(*context))
            BeginSave(context, std::move(changes), direction);
        return;
    }
    if (direction < 0) --context->rows.front();
    else ++context->rows.front();
    BeginRead(context);
}

void SaveFileInfo(const std::shared_ptr<FileInfoContext>& context) {
    if (!context || !context->loaded || context->loading || context->saving ||
        !context->properties_page) return;
    auto changes = CollectFileInfoChanges(*context);
    if (changes.empty() && context->cover_action ==
                               detail::FileInfoProbeCoverAction::unchanged) {
        BeginRead(context);
        return;
    }
    if (MakeFilesWritable(*context))
        BeginSave(context, std::move(changes), 0);
}

INT_PTR CALLBACK FileInfoPropertiesPageProc(HWND dialog, UINT message,
                                            WPARAM wparam, LPARAM value) {
    auto* context = reinterpret_cast<FileInfoContext*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));
    if (message == WM_INITDIALOG) {
        const auto* page = reinterpret_cast<const PROPSHEETPAGEW*>(value);
        context = page ? reinterpret_cast<FileInfoContext*>(page->lParam)
                       : nullptr;
        SetWindowLongPtrW(dialog, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(context));
        if (context) {
            context->properties_page = dialog;
            CreateAdvancedMetadataButton(*context);
            PopulateMp3PolicyControls(*context);
            for (const int identifier : kTagControls)
                SendDlgItemMessageW(dialog, identifier, EM_SETLIMITTEXT,
                                    4096, 0);
            PopulatePropertiesPage(*context);
        }
        return TRUE;
    }
    if (message == WM_COMMAND && context) {
        const UINT identifier = LOWORD(wparam);
        const UINT notification = HIWORD(wparam);
        if (identifier == kFileInfoAdvanced && notification == BN_CLICKED) {
            SetAdvancedMetadataMode(*context,
                IsDlgButtonChecked(dialog, kFileInfoAdvanced) == BST_CHECKED);
            if (context->history_settings)
                context->history_settings->advance_file_info =
                    context->advanced_mode;
            return TRUE;
        }
        if (!context->general_settings) return FALSE;
        auto& general = *context->general_settings;
        if (identifier == 2163 && notification == CBN_SELCHANGE) {
            if (const auto selected = SelectedMp3PolicyValue(dialog, 2163))
                general.mp3_read_tag_priority = static_cast<int>(*selected);
            return TRUE;
        }
        if (identifier == 2161 && notification == CBN_SELCHANGE) {
            if (const auto selected = SelectedMp3PolicyValue(dialog, 2161)) {
                general.mp3_write_tag_type = static_cast<int>(*selected);
                EnableMp3Id3v2PolicyControls(*context);
            }
            return TRUE;
        }
        if (identifier == 2174 && notification == CBN_SELCHANGE) {
            const auto selected = SelectedMp3PolicyValue(dialog, 2174);
            if (!selected) return TRUE;
            if (general.mp3_id3v2_encoding != 3 && *selected == 3U &&
                MessageBoxW(dialog, context->strings.id3v2_utf8_warning.c_str(),
                    nullptr, MB_YESNO | MB_ICONWARNING) != IDYES) {
                SendDlgItemMessageW(dialog, 2174, CB_SETCURSEL, 0, 0);
                return TRUE;
            }
            general.mp3_id3v2_encoding = static_cast<int>(*selected);
            return TRUE;
        }
        if (identifier == 2175 && notification == BN_CLICKED) {
            general.mp3_id3v2_padding =
                IsDlgButtonChecked(dialog, 2175) == BST_CHECKED;
            return TRUE;
        }
    }
    if (message == WM_DESTROY && context &&
        context->properties_page == dialog)
        context->properties_page = nullptr;
    return FALSE;
}

INT_PTR CALLBACK FileInfoCoverPageProc(HWND dialog, UINT message,
                                       WPARAM wparam, LPARAM value) {
    auto* context = reinterpret_cast<FileInfoContext*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));
    if (message == WM_INITDIALOG) {
        const auto* page = reinterpret_cast<const PROPSHEETPAGEW*>(value);
        context = page ? reinterpret_cast<FileInfoContext*>(page->lParam)
                       : nullptr;
        SetWindowLongPtrW(dialog, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(context));
        if (context) {
            context->cover_page = dialog;
            PopulateCoverPage(*context);
        }
        return TRUE;
    }
    if (message == WM_COMMAND && context &&
        HIWORD(wparam) == BN_CLICKED && context->loaded &&
        !context->loading && !context->saving && context->combined.writable) {
        const UINT identifier = LOWORD(wparam);
        if (identifier == 2220) {
            detail::ModernOpenFileOptions options;
            options.owner = context->sheet ? context->sheet : dialog;
            options.filters.push_back({L"*.bmp;*.jpeg;*.jpg;*.gif;*.png",
                                       L"*.bmp;*.jpeg;*.jpg;*.gif;*.png"});
            if (!context->rows.empty() &&
                context->rows.front() < context->tracks.size()) {
                options.initial_path =
                    context->tracks[context->rows.front()].path.parent_path();
            }
            const auto selected = detail::ModernOpenFile(options);
            if (!selected) return TRUE;
            auto bytes = ReadCoverBytes(*selected);
            if (!bytes) return TRUE;
            context->combined.cover = std::move(*bytes);
            context->cover_action =
                detail::FileInfoProbeCoverAction::replace;
            PopulateCoverPage(*context);
            UpdateSheetState(*context);
            return TRUE;
        }
        if (identifier == 2221) {
            context->combined.cover.clear();
            context->cover_action =
                detail::FileInfoProbeCoverAction::remove;
            PopulateCoverPage(*context);
            UpdateSheetState(*context);
            return TRUE;
        }
    }
    if (message == WM_DESTROY && context && context->cover_page == dialog)
        context->cover_page = nullptr;
    return FALSE;
}

void CreateFileInfoButtons(FileInfoContext& context) {
    const HWND cancel = GetDlgItem(context.sheet, IDCANCEL);
    if (!cancel) return;
    SetWindowTextW(cancel, context.strings.close.c_str());
    const HWND okay = GetDlgItem(context.sheet, IDOK);
    if (okay) ShowWindow(okay, SW_HIDE);

    RECT cancel_bounds{};
    GetWindowRect(cancel, &cancel_bounds);
    MapWindowPoints(nullptr, context.sheet,
                    reinterpret_cast<POINT*>(&cancel_bounds), 2);
    const int width = cancel_bounds.right - cancel_bounds.left;
    const int height = cancel_bounds.bottom - cancel_bounds.top;
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    const auto create = [&](UINT identifier, const std::wstring& text, int x) {
        const HWND button = CreateWindowExW(
            0, WC_BUTTONW, text.c_str(), style, x, cancel_bounds.top, width,
            height, context.sheet, reinterpret_cast<HMENU>(
                static_cast<UINT_PTR>(identifier)), nullptr, nullptr);
        if (button) SendMessageW(button, WM_SETFONT,
            SendMessageW(cancel, WM_GETFONT, 0, 0), TRUE);
    };
    create(kFileInfoSave, context.strings.save, 8);
    create(kFileInfoReload, context.strings.reload, 14 + width);
    if (context.rows.size() == 1) {
        create(kFileInfoPrevious, context.strings.previous,
               cancel_bounds.left - width * 2 - 12);
        create(kFileInfoNext, context.strings.next,
               cancel_bounds.left - width - 6);
    }
}

LRESULT CALLBACK FileInfoSheetSubclass(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam,
                                       UINT_PTR, DWORD_PTR reference) {
    auto* context = reinterpret_cast<FileInfoContext*>(reference);
    if (!context) return DefSubclassProc(window, message, wparam, lparam);
    if (message == kFileInfoReadComplete) {
        if (context->read_worker.joinable()) context->read_worker.join();
        std::unique_ptr<FileInfoReadResult> result;
        {
            const std::scoped_lock lock(context->pending_mutex);
            result = std::move(context->pending_read);
        }
        if (result) ApplyReadResult(*context, std::move(*result));
        return 0;
    }
    if (message == kFileInfoSaveComplete) {
        if (context->save_worker.joinable()) context->save_worker.join();
        std::unique_ptr<FileInfoWriteResult> result;
        {
            const std::scoped_lock lock(context->pending_mutex);
            result = std::move(context->pending_write);
        }
        if (result) ApplyWriteResult(*context, std::move(*result));
        return 0;
    }
    if (message == WM_COMMAND) {
        const UINT command = LOWORD(wparam);
        const auto shared = context->shared_from_this();
        if (command == kFileInfoSave) {
            SaveFileInfo(shared);
            return 0;
        }
        if (command == kFileInfoReload) {
            BeginRead(shared);
            return 0;
        }
        if (command == kFileInfoPrevious) {
            NavigateFileInfo(shared, -1);
            return 0;
        }
        if (command == kFileInfoNext) {
            NavigateFileInfo(shared, 1);
            return 0;
        }
        if (command == IDCANCEL) {
            SendMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        const std::wstring* question{};
        if (context->saving && !context->strings.save_cancel_question.empty())
            question = &context->strings.save_cancel_question;
        else if (context->loading &&
                 !context->strings.read_cancel_question.empty())
            question = &context->strings.read_cancel_question;
        if (question && MessageBoxW(window, question->c_str(),
                FormatFileInfoTitle(*context).c_str(),
                MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        context->closing = true;
        context->post_target.store(nullptr, std::memory_order_release);
        if (context->read_worker.joinable())
            context->read_worker.request_stop();
        if (context->save_worker.joinable())
            context->save_worker.request_stop();
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, FileInfoSheetSubclass, 1);
        context->sheet = nullptr;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

} // namespace

void PlayerWindow::ShowPlaylistProperties(
    const playlist::Track* explicit_playback_track) {
    const bool explicit_track = explicit_playback_track != nullptr;
    const size_t visible_count = explicit_track ? 1 : VisiblePlaylistTrackCount();
    const HWND owner = explicit_track ? window_ : playlist_window_;
    if (!owner || !sound_library_ || visible_count == 0) return;
    std::vector<size_t> rows;
    if (explicit_track) {
        rows.push_back(0);
    } else {
        rows.reserve(playlist_selected_rows_.size());
        for (const size_t row : playlist_selected_rows_) {
            if (row < visible_count) rows.push_back(row);
        }
        if (rows.empty() && playlist_selection_ &&
            *playlist_selection_ < visible_count)
            rows.push_back(*playlist_selection_);
    }
    if (rows.empty()) return;

    auto context = std::make_shared<FileInfoContext>();
    context->owner = owner;
    context->resources = ResourceModule();
    context->playlist_index = playlists_.ActiveIndex();
    context->library_mode = !explicit_track && settings_.playlist.library_mode;
    context->explicit_playback_track = explicit_track;
    if (explicit_track) context->explicit_source = *explicit_playback_track;
    context->general_settings = &settings_.general;
    context->history_settings = &settings_.history;
    context->advanced_mode = settings_.history.advance_file_info;
    context->tracks.reserve(visible_count);
    if (explicit_track) {
        context->tracks.push_back(*explicit_playback_track);
    } else {
        for (size_t row = 0; row < visible_count; ++row) {
            const auto* track = VisiblePlaylistTrack(row);
            if (!track) return;
            context->tracks.push_back(*track);
        }
    }
    context->rows = std::move(rows);
    context->playing_row = explicit_track
        ? std::optional<size_t>{0} : VisiblePlaylistPlayingRow();

    std::wstring executable(MAX_PATH, L'\0');
    DWORD executable_length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    while (executable_length == static_cast<DWORD>(executable.size()) &&
           executable.size() < 32768) {
        executable.resize(executable.size() * 2);
        executable_length = GetModuleFileNameW(
            nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    }
    if (executable_length == 0 || executable_length >= executable.size()) return;
    executable.resize(executable_length);
    const auto runtime = std::filesystem::path(executable).parent_path();
    context->helper = runtime / L"ttplayer_file_info_probe.exe";
    context->addin_directory = runtime / L"AddIn";
    context->ttpcomm_path = runtime / L"ttpcomm.dll";

    context->strings.different = ResourceText(0x816a);
    context->strings.close = ResourceText(8);
    context->strings.local_song = ResourceText(0x8ca5);
    context->strings.network_song = ResourceText(0x8ca4);
    const auto read = SplitStatus(ResourceText(0x816b));
    context->strings.read_status = read.first;
    context->strings.read_cancel_question = read.second;
    auto save = SplitStatus(ResourceText(0x816c));
    context->strings.save_status = std::move(save.first);
    context->strings.save_cancel_question = std::move(save.second);
    context->strings.save_failure = ResourceText(0x814e);
    context->strings.readonly_question = ResourceText(0x814f);
    context->strings.many_title = ResourceText(0x816d);
    context->strings.indexed_title = ResourceText(0x816e);
    context->strings.playing_title = ResourceText(0x816f);
    context->strings.save = ResourceText(0x821c);
    context->strings.reload = ResourceText(0x821d);
    context->strings.previous = ResourceText(0x8217);
    context->strings.next = ResourceText(0x8218);
    auto advanced = Split(ResourceText(0x875), L'\n');
    if (!advanced.empty())
        context->strings.advanced = std::move(advanced.front());
    context->strings.id3v2_utf8_warning = ResourceText(0x8170);
    context->strings.channel_names = Split(ResourceText(0x8154), L'|');

    std::array<PROPSHEETPAGEW, 2> pages{};
    pages[0].dwSize = sizeof(PROPSHEETPAGEW);
    pages[0].dwFlags = PSP_PREMATURE;
    pages[0].hInstance = context->resources;
    pages[0].pszTemplate = MAKEINTRESOURCEW(kFileInfoPropertiesPage);
    pages[0].pfnDlgProc = FileInfoPropertiesPageProc;
    pages[0].lParam = reinterpret_cast<LPARAM>(context.get());
    pages[1].dwSize = sizeof(PROPSHEETPAGEW);
    pages[1].dwFlags = PSP_PREMATURE;
    pages[1].hInstance = context->resources;
    pages[1].pszTemplate = MAKEINTRESOURCEW(kFileInfoCoverPage);
    pages[1].pfnDlgProc = FileInfoCoverPageProc;
    pages[1].lParam = reinterpret_cast<LPARAM>(context.get());

    const auto initial_title = FormatFileInfoTitle(*context);
    PROPSHEETHEADERW header{};
    header.dwSize = sizeof(header);
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_MODELESS | PSH_NOAPPLYNOW;
    header.hwndParent = owner;
    header.hInstance = context->resources;
    header.pszCaption = initial_title.c_str();
    header.nPages = static_cast<UINT>(pages.size());
    header.ppsp = pages.data();
    const INT_PTR created = PropertySheetW(&header);
    if (created <= 0) return;
    context->sheet = reinterpret_cast<HWND>(created);
    context->post_target.store(context->sheet, std::memory_order_release);
    SetWindowSubclass(context->sheet, FileInfoSheetSubclass, 1,
                      reinterpret_cast<DWORD_PTR>(context.get()));
    CreateFileInfoButtons(*context);
    UpdateSheetState(*context);
    BeginRead(context);

    const BOOL owner_was_enabled = IsWindowEnabled(owner);
    if (owner_was_enabled) EnableWindow(owner, FALSE);
    ShowWindow(context->sheet, SW_SHOWNORMAL);
    SetForegroundWindow(context->sheet);
    bool repost_quit{};
    MSG message{};
    while (context->sheet && IsWindow(context->sheet)) {
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) {
            repost_quit = received == 0;
            break;
        }
        if (!SendMessageW(context->sheet, PSM_ISDIALOGMESSAGE, 0,
                          reinterpret_cast<LPARAM>(&message))) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    context->closing = true;
    context->post_target.store(nullptr, std::memory_order_release);
    if (context->sheet && IsWindow(context->sheet)) DestroyWindow(context->sheet);
    if (context->read_worker.joinable()) {
        context->read_worker.request_stop();
        context->read_worker.join();
    }
    if (context->save_worker.joinable()) {
        context->save_worker.request_stop();
        context->save_worker.join();
    }
    {
        const std::scoped_lock lock(context->pending_mutex);
        context->pending_read.reset();
        context->pending_write.reset();
    }
    if (owner_was_enabled && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }

    bool changed{};
    if (context->explicit_playback_track && context->explicit_source &&
        !context->tracks.empty() && context->touched_rows.contains(0)) {
        const auto& source = *context->explicit_source;
        const auto& updated = context->tracks.front();
        const auto same_item = [](const playlist::Track& left,
                                  const playlist::Track& right) {
            return left.subtrack == right.subtrack &&
                _wcsicmp(left.path.c_str(), right.path.c_str()) == 0;
        };
        const auto apply = [&updated, &same_item](
            playlist::Playlist& list, size_t row) {
            if (row >= list.Tracks().size() ||
                !same_item(list.Tracks()[row], updated)) return false;
            bool result = list.SetDuration(row, updated.duration_ms);
            result |= list.SetMetadata(row, updated.title, updated.artist,
                                       updated.album);
            result |= list.SetExtendedMetadata(
                row, updated.metadata, updated.media_type,
                updated.bitrate_bps, updated.sample_rate_hz);
            return result;
        };

        if (media_library_playback_active_ && current_)
            changed |= apply(media_library_playback_, *current_);
        else if (playing_playlist_index_ && current_ &&
                 *playing_playlist_index_ < playlists_.Size()) {
            changed |= apply(playlists_.At(*playing_playlist_index_), *current_);
            if (changed) playlists_.MarkDirty(*playing_playlist_index_);
        }
        changed |= UpdateMediaLibraryTrackByIdentity(source, updated);
        if (opened_track_ && same_item(*opened_track_, source)) {
            *opened_track_ = updated;
            changed = true;
        }
        if (changed) RefreshPlaybackUi();
    } else if (context->library_mode) {
        for (const size_t row : context->touched_rows) {
            if (row < context->tracks.size())
                changed |= UpdateMediaLibraryTrackFromProperties(
                    row, context->tracks[row]);
        }
    } else if (context->playlist_index < playlists_.Size()) {
        auto& list = playlists_.At(context->playlist_index);
        for (const size_t row : context->touched_rows) {
            if (row >= list.Tracks().size() || row >= context->tracks.size())
                continue;
            const auto& before = list.Tracks()[row];
            const auto& updated = context->tracks[row];
            if (before.path != updated.path || before.subtrack != updated.subtrack)
                continue;
            changed |= list.SetDuration(row, updated.duration_ms);
            changed |= list.SetMetadata(row, updated.title, updated.artist,
                                        updated.album);
            changed |= list.SetExtendedMetadata(
                row, updated.metadata, updated.media_type,
                updated.bitrate_bps, updated.sample_rate_hz);
        }
        if (changed) playlists_.MarkDirty(context->playlist_index);
    }
    if (changed) RefreshPlaylist();
    if (repost_quit) PostQuitMessage(static_cast<int>(message.wParam));
}

} // namespace ttplayer::ui
