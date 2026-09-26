#include "ttplayer/ui/cover_image.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include "player_window_internal.h"
#include "file_info_cover_policy.h"
#include "file_info_probe_protocol.h"
#include "file_info_probe_client.h"
#include "file_info_mp3_policy.h"
#include "file_info_editing.h"
#include "options_buttons.h"
#include "../audio/tag_genres.h"
#include "modern_file_dialog.h"

#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/cue_sheet.h"
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
#include <shlwapi.h>
#include <shellapi.h>

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
    DWORD cover_maximum_bytes{};
    bool writable{};
    bool cover_writable{};
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
    DWORD cover_maximum_bytes{detail::kFileInfoProbeMaximumCoverBytes};
    bool writable{};
    bool cover_writable{};
};

struct FileInfoReadResult {
    std::uint64_t generation{};
    std::vector<FileInfoRecord> records;
};

struct FileInfoWriteResult {
    struct Item {
        size_t row{};
        detail::FileInfoFields values;
        bool cover_saved{};
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
    FileInfoStrings strings;
    FileInfoCombined combined;
    std::vector<FileInfoRecord> originals;
    std::vector<FileInfoRecord> drafts;
    HWND toolbar{};
    HIMAGELIST toolbar_images{};
    UINT codepage{GetACP()};
    bool populating{};
    HWND inline_edit{};
    std::string inline_name;
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
        if (toolbar_images) ImageList_Destroy(toolbar_images);
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
        record.bitrate = detail::FileInfoBitrate(track.bitrate_bps & 0x7fffffffU,
                                               (track.bitrate_bps & 0x80000000U) != 0);
    record.duration = detail::FileInfoDuration(track.duration_ms);
    record.gain = TrackMetadataValue(track, "replaygain_track_gain");

    if (record.path.empty() || LooksLikeNetworkPath(record.path.native()) ||
        stop.stop_requested()) {
        return record;
    }

    const auto probe = detail::RunFileInfoReadProbe(
        stop, helper, addin_directory, record.path, ttpcomm_path, 15000,
        nullptr, mp3_policy, record.subtrack);
    if (!probe || FAILED(probe->status) || stop.stop_requested()) return record;
    record.reader_opened = true;
    audio::ArchiveMemberPath archive_member;
    record.writable = (probe->capabilities & 4U) != 0 &&
        !audio::ParseArchiveMemberPath(record.path.native(), archive_member);
    record.cover_writable = record.writable && probe->cover_writable != 0;
    record.cover_maximum_bytes = probe->cover_maximum_bytes;

    static constexpr std::array<std::wstring_view, 7> names{
        L"Title", L"Artist", L"Album", L"Tracknumber", L"Genre", L"Date",
        L"Comment"};
    for (size_t index = 0; index < names.size(); ++index) {
        auto value = ProbeMetadataValue(*probe, names[index]);
        if (index == 1 && value.empty())
            value = ProbeMetadataValue(*probe, L"Author");
        if (index == 5 && value.empty())
            value = ProbeMetadataValue(*probe, L"Year");
        record.tags[index] = std::move(value);
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
    if (const auto separator = record.codec.find(L'|'); separator != std::wstring::npos)
        record.codec.erase(0, separator + 1);
    record.channels = FormatChannels(format.nChannels, strings.channel_names);
    if (format.nSamplesPerSec != 0)
        record.sample_rate = std::to_wstring(format.nSamplesPerSec) + L" Hz";
    if (format.wBitsPerSample != 0)
        record.bits = std::to_wstring(format.wBitsPerSample) + L" Bits";
    const std::uint64_t encoded = probe->encoded_bits_per_second != 0
        ? probe->encoded_bits_per_second
        : static_cast<std::uint64_t>(format.nAvgBytesPerSec) * 8U;
    if (encoded != 0)
        record.bitrate = detail::FileInfoBitrate(encoded & 0x7fffffffU,
                                                (encoded & 0x80000000U) != 0);
    record.duration_ms = static_cast<int>(std::min<DWORD>(
        probe->duration_ms, static_cast<DWORD>(INT_MAX)));
    record.duration = detail::FileInfoDuration(record.duration_ms);
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
        combined.cover_writable = combined.cover_writable || record.cover_writable;
        if (record.cover_writable && record.cover_maximum_bytes)
            combined.cover_maximum_bytes = std::min(combined.cover_maximum_bytes, record.cover_maximum_bytes);
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
    return DecodeCoverPreview(bytes, {target_width, target_height}, GetSysColor(COLOR_WINDOW));
}

detail::FileInfoFields RecordFields(const FileInfoRecord& record) {
    detail::FileInfoFields fields;
    for (const auto& [name, value] : record.metadata)
        fields.emplace_back(name, SafeWide(value));
    return fields;
}

void SetRecordField(FileInfoRecord& record, std::string_view name,
                    std::wstring_view value) {
    if (record.subtrack > 0 && _wcsicmp(record.path.extension().c_str(), L".cue") == 0 &&
        !audio::CueSheet::IsWritableField(SafeWide(name))) return;
    auto fields = RecordFields(record);
    detail::SetFileInfoField(fields, name, value);
    record.metadata.clear();
    for (const auto& field : fields)
        record.metadata.emplace_back(field.first, SafeUtf8(field.second));
    for (size_t index = 0; index < kTagNames.size(); ++index) {
        record.tags[index] = detail::FileInfoField(fields, kTagNames[index]);
        if (index == 1 && record.tags[index].empty())
            record.tags[index] = detail::FileInfoField(fields, "Author");
        if (index == 5 && record.tags[index].empty())
            record.tags[index] = detail::FileInfoField(fields, "Year");
    }
    record.gain = detail::FileInfoField(fields, "replaygain_track_gain");
}

using FileInfoChangesByRow = std::map<size_t, detail::FileInfoFields>;

FileInfoChangesByRow CollectFileInfoChanges(const FileInfoContext& context) {
    FileInfoChangesByRow changes;
    for (size_t i = 0; i < context.drafts.size() && i < context.originals.size(); ++i) {
        if (!context.drafts[i].writable) continue;
        auto fields = detail::FileInfoChanges(RecordFields(context.originals[i]),
                                              RecordFields(context.drafts[i]));
        if (!fields.empty()) changes.emplace(context.drafts[i].row, std::move(fields));
    }
    return changes;
}

bool FileInfoDirty(const FileInfoContext& context) {
    if (!CollectFileInfoChanges(context).empty()) return true;
    for (size_t i = 0; i < context.drafts.size() && i < context.originals.size(); ++i)
        if (context.drafts[i].cover_writable &&
            context.drafts[i].cover != context.originals[i].cover) return true;
    return false;
}

void UpdateSheetState(FileInfoContext& context);
void PopulatePropertiesPage(FileInfoContext& context);
void FinishFileInfoCell(FileInfoContext& context, bool commit);

bool CanEditFileInfoField(const FileInfoContext& context, std::string_view name) {
    return std::any_of(context.drafts.begin(), context.drafts.end(), [&](const auto& record) {
        return record.writable && (record.subtrack <= 0 ||
            _wcsicmp(record.path.extension().c_str(), L".cue") != 0 ||
            audio::CueSheet::IsWritableField(SafeWide(name)));
    });
}

void RefreshFileInfoDraft(FileInfoContext& context, bool populate = true) {
    context.combined = CombineRecords(context.drafts, context.strings);
    if (populate) PopulatePropertiesPage(context);
    UpdateSheetState(context);
}

void EditFileInfoField(FileInfoContext& context, std::string_view name,
                       std::wstring_view value, bool populate = true) {
    for (auto& record : context.drafts)
        if (record.writable) SetRecordField(record, name, value);
    RefreshFileInfoDraft(context, populate);
}

std::wstring FileInfoResource(HMODULE resources, UINT id,
                              std::wstring_view fallback = {}) {
    wchar_t buffer[2048]{};
    const int count = LoadStringW(resources, id, buffer, static_cast<int>(std::size(buffer)));
    return count > 0 ? std::wstring(buffer, count) : std::wstring(fallback);
}

HWND CreateFileInfoToolbar(HWND page, HMODULE resources, const RECT& bounds,
                           HIMAGELIST& images, bool editor = false) {
    const HWND toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | TBSTYLE_LIST |
            CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        page, reinterpret_cast<HMENU>(0xe800), nullptr, nullptr);
    if (!toolbar) return nullptr;
    SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS | TBSTYLE_EX_MIXEDBUTTONS);
    images = ImageList_LoadImageW(resources, MAKEINTRESOURCEW(149), 16, 0,
                                 RGB(192, 192, 192), IMAGE_BITMAP, LR_CREATEDIBSECTION);
    SendMessageW(toolbar, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
    const UINT commands[]{0x875, 0, 0x86c, 0x86b, 0x8076, 0, 0x834,
                          0x8077, 0x8078, 0x8079, 0, 0x8075};
    int image{};
    for (const auto command : commands) {
        TBBUTTON button{};
        button.idCommand = command;
        button.fsState = TBSTATE_ENABLED;
        button.fsStyle = command ? BTNS_BUTTON : BTNS_SEP;
        button.iBitmap = command ? image++ : 6;
        button.iString = -1;
        if (editor && command != 0x86c && command != 0x86b && command != 0x8076)
            continue;
        if (command == 0x86c || command == 0x86b) button.fsStyle |= BTNS_WHOLEDROPDOWN;
        if (command == 0x875) {
            button.fsStyle |= BTNS_CHECK | BTNS_SHOWTEXT | BTNS_AUTOSIZE;
            auto text = FileInfoResource(resources, command, L"高级");
            const auto last = text.find_last_of(L'\n');
            if (last != text.npos) text.erase(0, last + 1);
            text.push_back(L'\0');
            button.iString = SendMessageW(toolbar, TB_ADDSTRINGW, 0,
                                          reinterpret_cast<LPARAM>(text.c_str()));
        }
        SendMessageW(toolbar, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&button));
    }
    return toolbar;
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
        auto names = Split(FileInfoResource(context.resources, 0x8156), L'|');
        names.resize(2);
        column.pszText = names[0].data();
        column.cx = std::max<LONG>(70, (bounds.right - bounds.left) / 3);
        ListView_InsertColumn(list, 0, &column);
        column.cx = std::max<LONG>(
            80, bounds.right - bounds.left - column.cx - 4);
        column.pszText = names[1].data();
        ListView_InsertColumn(list, 1, &column);
    }
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
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
    if (!context.combined.metadata.empty())
        ListView_SetItemState(list, std::clamp(selected, 0,
            static_cast<int>(context.combined.metadata.size()) - 1),
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void SetAdvancedMetadataMode(FileInfoContext& context, bool enabled) {
    context.advanced_mode = enabled;
    const HWND list = GetDlgItem(context.properties_page, 2164);
    RECT area{};
    GetWindowRect(list, &area);
    // 0043144E hides intersecting labels and the artist link as well as edits.
    for (HWND child = GetWindow(context.properties_page, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        RECT bounds{}, intersection{};
        GetWindowRect(child, &bounds);
        if (child != list && IntersectRect(&intersection, &bounds, &area) &&
            bounds.top >= area.top && bounds.bottom <= area.bottom)
            ShowWindow(child, enabled ? SW_HIDE : SW_SHOW);
    }
    ShowWindow(list, enabled ? SW_SHOW : SW_HIDE);
    SendMessageW(context.toolbar, TB_CHECKBUTTON, kFileInfoAdvanced, MAKELONG(enabled, 0));
    SendMessageW(context.toolbar, TB_HIDEBUTTON, 0x834, MAKELONG(enabled, 0));
    for (const UINT command : {0x8077U, 0x8078U, 0x8079U})
        SendMessageW(context.toolbar, TB_HIDEBUTTON, command, MAKELONG(!enabled, 0));
    InvalidateRect(context.properties_page, nullptr, TRUE);
}

void CreateFileInfoEditorToolbar(FileInfoContext& context) {
    RECT bounds{};
    GetWindowRect(GetDlgItem(context.properties_page, 2164), &bounds);
    MapWindowPoints(nullptr, context.properties_page, reinterpret_cast<POINT*>(&bounds), 2);
    bounds.top = bounds.bottom + 6;
    bounds.bottom = bounds.top + 24;
    context.toolbar = CreateFileInfoToolbar(context.properties_page, context.resources,
                                             bounds, context.toolbar_images);
}

void PopulatePropertiesPage(FileInfoContext& context) {
    const HWND page = context.properties_page;
    if (!page || !context.loaded) return;
    context.populating = true;
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

    for (size_t i = 0; i < kTagControls.size(); ++i)
        EnableWindow(GetDlgItem(page, kTagControls[i]), CanEditFileInfoField(context, kTagNames[i]));
    SetAdvancedMetadataMode(context, context.advanced_mode);
    context.populating = false;
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
    if (context.rows.size() == 1 || context.cover_action == detail::FileInfoProbeCoverAction::replace)
        context.cover_bitmap = DecodeCoverBitmap(
            context.combined.cover, bounds.right, bounds.bottom);
    if (context.cover_bitmap) {
        const LONG_PTR style = GetWindowLongPtrW(picture, GWL_STYLE);
        SetWindowLongPtrW(picture, GWL_STYLE,
                          (style & ~SS_TYPEMASK) | SS_BITMAP | SS_CENTERIMAGE);
        SendMessageW(picture, STM_SETIMAGE, IMAGE_BITMAP,
                     reinterpret_cast<LPARAM>(context.cover_bitmap));
    } else {
        const LONG_PTR style = GetWindowLongPtrW(picture, GWL_STYLE);
        SetWindowLongPtrW(picture, GWL_STYLE, (style & ~SS_TYPEMASK) | SS_CENTER | SS_CENTERIMAGE);
        wchar_t text[256]{};
        swprintf_s(text, FileInfoResource(context.resources, 0x8219,
            L"%d个文件中含有专辑封面信息").c_str(), static_cast<int>(context.combined.cover_count));
        SetWindowTextW(picture, text);
        InvalidateRect(picture, nullptr, TRUE);
    }
    const bool ready = context.loaded && !context.loading && !context.saving &&
                       context.combined.cover_writable;
    EnableWindow(GetDlgItem(page, 2220), ready);
    EnableWindow(GetDlgItem(page, 2221),
                 ready && context.combined.cover_count != 0);
}

std::wstring FormatFileInfoTitle(const FileInfoContext& context) {
    wchar_t title[256]{};
    if (context.rows.size() > 1) {
        const auto& format = context.strings.many_title;
        if (!format.empty())
            swprintf_s(title, format.c_str(),
                       static_cast<int>(context.rows.size()));
    } else if (!context.rows.empty()) {
        if (context.explicit_playback_track)
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
        for (size_t i = 0; i < kTagControls.size(); ++i)
            EnableWindow(GetDlgItem(context.properties_page, kTagControls[i]),
                         ready && CanEditFileInfoField(context, kTagNames[i]));
        EnableWindow(GetDlgItem(context.properties_page, 2164), ready);
        EnableWindow(GetDlgItem(context.properties_page, 2160), ready && context.combined.writable);
        SendMessageW(context.toolbar, TB_ENABLEBUTTON, kFileInfoAdvanced, MAKELONG(ready, 0));
        const bool selected = ListView_GetNextItem(GetDlgItem(context.properties_page, 2164),
                                                   -1, LVNI_SELECTED) >= 0;
        for (const UINT command : {0x86cU, 0x86bU, 0x8076U, 0x834U, 0x8077U, 0x8078U, 0x8079U, 0x8075U}) {
            bool enabled = ready && context.combined.writable;
            if (command == 0x8078 || command == 0x8079) enabled = enabled && selected;
            if (command == 0x8075) enabled = enabled && !CollectFileInfoChanges(context).empty();
            SendMessageW(context.toolbar, TB_ENABLEBUTTON, command, MAKELONG(enabled, 0));
        }
    }
    if (context.cover_page) {
        EnableWindow(GetDlgItem(context.cover_page, 2220),
                     ready && context.combined.cover_writable);
        EnableWindow(GetDlgItem(context.cover_page, 2221),
                     ready && context.combined.cover_writable &&
                         context.combined.cover_count != 0);
    }
    EnableWindow(GetDlgItem(context.sheet, kFileInfoSave),
                 ready && FileInfoDirty(context));
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
    context.originals = result.records;
    context.drafts = result.records;
    context.codepage = GetACP();
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
    FinishFileInfoCell(*context, false);
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

std::optional<detail::FileInfoProbeWriteResult> RunFileInfoWriteProbe(
    std::stop_token stop, const std::filesystem::path& helper,
    const std::filesystem::path& addin_directory,
    const std::filesystem::path& logical_path,
    const std::filesystem::path& ttpcomm_path,
    const detail::FileInfoFields& changes,
    detail::FileInfoProbeCoverAction cover_action,
    const std::vector<unsigned char>& cover,
    const detail::FileInfoProbeMp3Policy& mp3_policy, int subtrack) {
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
        request.fields.push_back({field, value});
    }
    request.cover_action = cover_action;
    if (cover_action == detail::FileInfoProbeCoverAction::replace)
        request.cover = cover;
    const bool encoded = detail::WriteFileInfoProbeWriteRequest(
        request_path, request);
    std::vector<std::wstring> arguments{L"write", addin_directory.wstring(),
        logical_path.wstring(), ttpcomm_path.wstring()};
    if (subtrack > 0 && _wcsicmp(logical_path.extension().c_str(), L".cue") == 0) {
        arguments.front() = L"cue-write";
        arguments.push_back(std::to_wstring(subtrack));
    }
    arguments.push_back(request_path.wstring());
    arguments.push_back(output_path.wstring());
    const bool completed = encoded && detail::RunFileInfoProbe(stop, helper,
        arguments, 20000).Succeeded();
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
    FileInfoChangesByRow changes,
    int navigation_after_save) {
    if (!context ||
        (changes.empty() && context->cover_action ==
                              detail::FileInfoProbeCoverAction::unchanged) ||
        !context->loaded || context->loading || context->saving ||
        context->closing) return;

    context->saving = true;
    context->navigation_after_save = navigation_after_save;
    UpdateSheetState(*context);
    const auto tracks = context->tracks;
    const auto helper = context->helper;
    const auto addin_directory = context->addin_directory;
    const auto ttpcomm_path = context->ttpcomm_path;
    const auto mp3_policy = ProbeMp3Policy(context->general_settings);
    const auto cover_action = context->cover_action;
    const auto drafts = context->drafts;
    const auto originals = context->originals;
    context->save_worker = std::jthread(
        [context, tracks, changes = std::move(changes), helper,
         addin_directory, ttpcomm_path, mp3_policy, cover_action, drafts, originals]
        (std::stop_token stop) {
        const HRESULT initialized = CoInitializeEx(nullptr,
                                                   COINIT_MULTITHREADED);
        auto result = std::make_unique<FileInfoWriteResult>();
        for (size_t record_index = 0; record_index < drafts.size(); ++record_index) {
            if (stop.stop_requested()) break;
            const auto& draft = drafts[record_index];
            const size_t row = draft.row;
            if (row >= tracks.size() || !draft.writable) continue;
            const auto found = changes.find(row);
            const detail::FileInfoFields fields = found == changes.end()
                ? detail::FileInfoFields{} : found->second;
            const auto action = draft.cover_writable && record_index < originals.size() &&
                draft.cover != originals[record_index].cover ? cover_action
                    : detail::FileInfoProbeCoverAction::unchanged;
            if (fields.empty() && action == detail::FileInfoProbeCoverAction::unchanged) continue;
            const auto written = RunFileInfoWriteProbe(
                stop, helper, addin_directory, tracks[row].path,
                ttpcomm_path, fields, action, draft.cover, mp3_policy, tracks[row].subtrack);
            if (!written) {
                if (!stop.stop_requested() && SUCCEEDED(result->error))
                    result->error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                continue;
            }
            FileInfoWriteResult::Item item;
            item.row = row;
            if (FAILED(written->status) && SUCCEEDED(result->error))
                result->error = written->status;
            if (written->fields.size() != fields.size() &&
                SUCCEEDED(result->error))
                result->error = E_UNEXPECTED;
            const size_t count = std::min(written->fields.size(),
                                          fields.size());
            for (size_t index = 0; index < count; ++index) {
                if (SUCCEEDED(written->fields[index]))
                    item.values.push_back(fields[index]);
                else if (SUCCEEDED(result->error))
                    result->error = written->fields[index];
            }
            if (action !=
                    detail::FileInfoProbeCoverAction::unchanged &&
                FAILED(written->cover_status) && SUCCEEDED(result->error)) {
                result->error = written->cover_status;
            }
            item.cover_saved = action != detail::FileInfoProbeCoverAction::unchanged &&
                SUCCEEDED(written->cover_status);
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
            MergeTrackMetadata(track, field, value);
            if (track.subtrack > 0 && _wcsicmp(track.path.extension().c_str(), L".cue") == 0 &&
                AsciiEquals(field, "Album")) {
                for (size_t i = 0; i < context.tracks.size(); ++i) {
                    auto& sibling = context.tracks[i];
                    if (sibling.subtrack > 0 && _wcsicmp(sibling.path.c_str(), track.path.c_str()) == 0) {
                        MergeTrackMetadata(sibling, field, value);
                        context.touched_rows.insert(i);
                    }
                }
            }
            for (auto& original : context.originals)
                if (original.row == item.row) SetRecordField(original, field, value);
        }
        if (item.cover_saved) {
            for (size_t i = 0; i < context.originals.size(); ++i)
                if (context.originals[i].row == item.row)
                    context.originals[i].cover = context.drafts[i].cover;
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
    FinishFileInfoCell(*context, true);
    auto changes = CollectFileInfoChanges(*context);
    if (FileInfoDirty(*context)) {
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
    FinishFileInfoCell(*context, true);
    auto changes = CollectFileInfoChanges(*context);
    if (changes.empty() && context->cover_action ==
                               detail::FileInfoProbeCoverAction::unchanged) {
        BeginRead(context);
        return;
    }
    if (MakeFilesWritable(*context))
        BeginSave(context, std::move(changes), 0);
}

std::wstring ConvertFileInfoText(const std::wstring& value, UINT from, UINT to) {
    if (value.empty() || from == to) return value;
    const int bytes = WideCharToMultiByte(from, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return value;
    std::string encoded(bytes, '\0');
    if (!WideCharToMultiByte(from, 0, value.data(), static_cast<int>(value.size()),
                             encoded.data(), bytes, nullptr, nullptr)) return value;
    const int count = MultiByteToWideChar(to, 0, encoded.data(), bytes, nullptr, 0);
    if (count <= 0) return value;
    std::wstring converted(count, L'\0');
    if (!MultiByteToWideChar(to, 0, encoded.data(), bytes, converted.data(), count)) return value;
    return converted;
}

std::wstring MapFileInfoChinese(const std::wstring& value, UINT command) {
    if (value.empty()) return value;
    const DWORD flags = command == 0x86d ? LCMAP_TRADITIONAL_CHINESE : LCMAP_SIMPLIFIED_CHINESE;
    const int count = LCMapStringW(MAKELCID(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
        SORT_DEFAULT), flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return value;
    std::wstring converted(count, L'\0');
    if (!LCMapStringW(MAKELCID(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
        SORT_DEFAULT), flags, value.data(), static_cast<int>(value.size()), converted.data(), count))
        return value;
    return converted;
}

thread_local std::vector<UINT>* file_info_codepages{};
BOOL CALLBACK CollectFileInfoCodepage(LPWSTR text) {
    if (file_info_codepages) file_info_codepages->push_back(wcstoul(text, nullptr, 10));
    return TRUE;
}

UINT FileInfoTransformMenu(HWND page, HWND toolbar, HMODULE resources,
                           UINT command, UINT codepage) {
    RECT bounds{};
    SendMessageW(toolbar, TB_GETRECT, command, reinterpret_cast<LPARAM>(&bounds));
    MapWindowPoints(toolbar, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
    const HMENU menu = CreatePopupMenu();
    if (command == 0x86c) {
        for (const UINT id : {0x86dU, 0x86eU})
            AppendMenuW(menu, MF_STRING, id, FileInfoResource(resources, id).c_str());
    } else {
        std::vector<UINT> pages;
        file_info_codepages = &pages;
        EnumSystemCodePagesW(CollectFileInfoCodepage, CP_INSTALLED);
        file_info_codepages = nullptr;
        std::sort(pages.begin(), pages.end());
        for (const auto cp : pages) {
            CPINFOEXW info{};
            if (GetCPInfoExW(cp, 0, &info))
                AppendMenuW(menu, MF_STRING | (cp == codepage ? MF_CHECKED : 0), cp,
                             info.CodePageName);
        }
        SetMenuDefaultItem(menu, GetACP(), FALSE);
    }
    const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                       bounds.left, bounds.bottom, 0, page, nullptr);
    DestroyMenu(menu);
    return choice;
}

bool FileInfoTooltip(HMODULE resources, LPARAM value) {
    auto* info = reinterpret_cast<NMTTDISPINFOW*>(value);
    if (!info || info->hdr.code != TTN_GETDISPINFOW) return false;
    static thread_local std::wstring text;
    text = FileInfoResource(resources, static_cast<UINT>(info->hdr.idFrom));
    text.resize(text.find(L'\n') == text.npos ? text.size() : text.find(L'\n'));
    info->lpszText = text.data();
    return true;
}

struct FileInfoFieldDialog {
    FileInfoContext* context{};
    std::wstring name, value;
    bool adding{};
    HWND toolbar{};
    HIMAGELIST images{};
    UINT codepage{GetACP()};
    ~FileInfoFieldDialog() { if (images) ImageList_Destroy(images); }
};

INT_PTR CALLBACK FileInfoFieldDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<FileInfoFieldDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<FileInfoFieldDialog*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        const HWND combo = GetDlgItem(dialog, 1005);
        for (const auto* name : {"Title", "Artist", "Album", "Tracknumber", "Genre", "Date",
                                "Comment", "replaygain_track_gain", "Lyrics"})
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(SafeWide(name).c_str()));
        if (state->context->history_settings)
            for (const auto& name : state->context->history_settings->tag_names)
                if (SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                        reinterpret_cast<LPARAM>(name.c_str())) == CB_ERR)
                    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        SetWindowTextW(combo, state->name.c_str());
        SetDlgItemTextW(dialog, 1052, state->name.c_str());
        SendDlgItemMessageW(dialog, 1052, EM_SETREADONLY, TRUE, 0);
        ShowWindow(combo, state->adding ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(dialog, 1052), state->adding ? SW_HIDE : SW_SHOW);
        SetDlgItemTextW(dialog, 2168, state->value.c_str());
        SendDlgItemMessageW(dialog, 2168, EM_SETLIMITTEXT, 1024 * 1024, 0);
        RECT button{}, name{};
        GetWindowRect(GetDlgItem(dialog, IDOK), &button);
        GetWindowRect(combo, &name);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&button), 2);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&name), 2);
        const RECT bounds{name.left, button.bottom - 24, button.left - 2, button.bottom};
        state->toolbar = CreateFileInfoToolbar(dialog, state->context->resources,
                                               bounds, state->images, true);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_COMMAND) {
        const UINT command = LOWORD(wparam);
        if (command == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        if (command == IDOK) {
            if (state->adding) state->name = detail::FileInfoTrim(GetControlText(dialog, 1005));
            if (state->name.empty()) { SetFocus(GetDlgItem(dialog, 1005)); return TRUE; }
            state->value = detail::FileInfoTrim(GetControlText(dialog, 2168));
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (command == 0x8076) {
            SetDialogText(dialog, 2168, detail::FileInfoTitleCase(GetControlText(dialog, 2168)));
            return TRUE;
        }
    }
    if (message == WM_NOTIFY) {
        if (FileInfoTooltip(state->context->resources, lparam)) return TRUE;
        const auto* info = reinterpret_cast<NMTOOLBARW*>(lparam);
        if (info->hdr.hwndFrom == state->toolbar && info->hdr.code == TBN_DROPDOWN) {
            const UINT choice = FileInfoTransformMenu(dialog, state->toolbar,
                state->context->resources, info->iItem, state->codepage);
            if (choice) {
                auto text = GetControlText(dialog, 2168);
                if (info->iItem == 0x86c) text = MapFileInfoChinese(text, choice);
                else {
                    text = ConvertFileInfoText(text, state->codepage, choice);
                    state->codepage = choice;
                }
                SetDialogText(dialog, 2168, text);
            }
            return TRUE;
        }
    }
    return FALSE;
}

void EditAdvancedFileInfo(FileInfoContext& context, bool adding) {
    if (!context.loaded || context.loading || context.saving || !context.combined.writable) return;
    FileInfoFieldDialog state;
    state.context = &context;
    state.adding = adding;
    if (!adding) {
        const int selected = ListView_GetNextItem(GetDlgItem(context.properties_page, 2164), -1, LVNI_SELECTED);
        if (selected < 0 || static_cast<size_t>(selected) >= context.combined.metadata.size()) return;
        state.name = context.combined.metadata[selected].first;
        if (!CanEditFileInfoField(context, SafeUtf8(state.name))) return;
        state.value = context.combined.metadata[selected].second;
        if (state.value == context.strings.different) state.value.clear();
    }
    if (ShowWtlModalDialog(context.resources, MAKEINTRESOURCEW(225), context.sheet,
            FileInfoFieldDialogProc, reinterpret_cast<LPARAM>(&state)) != IDOK) return;
    EditFileInfoField(context, SafeUtf8(state.name), state.value);
    if (adding && context.history_settings) {
        auto& names = context.history_settings->tag_names;
        if (std::none_of(names.begin(), names.end(), [&](const auto& name) {
                return WideAsciiEquals(name, state.name); })) names.push_back(state.name);
    }
}

struct FileInfoPatternDialog { std::wstring pattern; };
INT_PTR CALLBACK FileInfoPatternDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<FileInfoPatternDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<FileInfoPatternDialog*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        for (const auto* pattern : {L"%(Artist) - %(Title)", L"%(Artist) - %(TrackNumber).%(Title)",
             L"%(TrackNumber).%(Artist) - %(Title)", L"%(Artist)\\%(Title)",
             L"%(Album)\\%(TrackNumber).%(Title)", L"%(Artist)\\%(Album)\\%(Title)",
             L"%(Genre)\\%(Album)\\%(Title)"})
            SendDlgItemMessageW(dialog, 2161, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pattern));
        if (state->pattern.empty()) SendDlgItemMessageW(dialog, 2161, CB_SETCURSEL, 0, 0);
        else SetDialogText(dialog, 2161, state->pattern);
        return TRUE;
    }
    if (message == WM_COMMAND && state) {
        if (LOWORD(wparam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        if (LOWORD(wparam) == IDOK) {
            state->pattern = detail::FileInfoTrim(GetControlText(dialog, 2161));
            if (!state->pattern.empty()) EndDialog(dialog, IDOK);
            return TRUE;
        }
    }
    return FALSE;
}

void GuessFileInfoTags(FileInfoContext& context) {
    FileInfoPatternDialog state{context.history_settings ? context.history_settings->tag_pattern : L""};
    if (ShowWtlModalDialog(context.resources, MAKEINTRESOURCEW(224), context.sheet,
            FileInfoPatternDialogProc, reinterpret_cast<LPARAM>(&state)) != IDOK) return;
    if (context.history_settings) context.history_settings->tag_pattern = state.pattern;
    for (auto& record : context.drafts)
        if (record.writable)
            for (const auto& [name, value] : detail::GuessFileInfoFields(record.path.native(), state.pattern))
                SetRecordField(record, SafeUtf8(name), value);
    RefreshFileInfoDraft(context);
}

void FileInfoTransform(FileInfoContext& context, UINT command, UINT codepage = 0) {
    if (codepage && context.codepage != GetACP()) {
        // 00431FDE restores the original fields before choosing another codepage.
        for (size_t i = 0; i < context.drafts.size(); ++i) {
            auto cover = std::move(context.drafts[i].cover);
            context.drafts[i] = context.originals[i];
            context.drafts[i].cover = std::move(cover);
        }
    }
    for (auto& record : context.drafts) {
        if (!record.writable) continue;
        for (const auto& [name, value] : RecordFields(record)) {
            const auto text = codepage ? ConvertFileInfoText(value, GetACP(), codepage)
                : command == 0x8076 ? detail::FileInfoTitleCase(value) : MapFileInfoChinese(value, command);
            SetRecordField(record, name, text);
        }
    }
    if (codepage) context.codepage = codepage;
    RefreshFileInfoDraft(context);
}

std::wstring FileInfoUrlParameter(std::wstring_view text) {
    const auto bytes = SafeUtf8(text);
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result;
    for (const unsigned char ch : bytes) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') result += ch;
        else { result += L'%'; result += digits[ch >> 4]; result += digits[ch & 15]; }
    }
    return result;
}

void OpenFileInfoDetails(FileInfoContext& context, bool album) {
    const auto field = [&](size_t index) {
        return context.combined.tags[index] == context.strings.different
            ? std::wstring{} : FileInfoUrlParameter(context.combined.tags[index]);
    };
    auto url = FileInfoResource(context.resources, 0x8298, L"http://www.qianqian.com");
    url += album ? L"/ttclient/zhuanji_ttpsh.php?" : L"/ttclient/geshouku_ttpsh.php?";
    if (album) url += L"album=" + field(2) + L"&";
    url += L"artist=" + field(1) + L"&";
    ShellExecuteW(context.sheet, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK FileInfoLinkProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                  UINT_PTR, DWORD_PTR) {
    if (message == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, FileInfoLinkProc, 1);
    return DefSubclassProc(window, message, wparam, lparam);
}

void InitializeFileInfoLink(HWND page) {
    const HWND link = GetDlgItem(page, 1066);
    SetWindowLongPtrW(link, GWL_STYLE, GetWindowLongPtrW(link, GWL_STYLE) | SS_NOTIFY);
    SetWindowSubclass(link, FileInfoLinkProc, 1, 0);
}

void FinishFileInfoCell(FileInfoContext& context, bool commit) {
    const HWND edit = std::exchange(context.inline_edit, nullptr);
    if (!edit) return;
    auto text = GetControlText(context.properties_page, 0xe801);
    const auto name = std::exchange(context.inline_name, {});
    DestroyWindow(edit);
    if (commit) EditFileInfoField(context, name, text);
}

LRESULT CALLBACK FileInfoCellProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                  UINT_PTR, DWORD_PTR reference) {
    auto& context = *reinterpret_cast<FileInfoContext*>(reference);
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (message == WM_KILLFOCUS || (message == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE))) {
        PostMessageW(context.properties_page, WM_APP + 0x418,
                     message == WM_KILLFOCUS ? 1 : wparam == VK_RETURN ? 3 : 2,
                     reinterpret_cast<LPARAM>(window));
        if (message == WM_KEYDOWN) return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, FileInfoCellProc, 1);
    return DefSubclassProc(window, message, wparam, lparam);
}

void BeginFileInfoCell(FileInfoContext& context, int row) {
    if (!context.combined.writable || !context.loaded || context.loading || context.saving || row < 0 ||
        static_cast<size_t>(row) >= context.combined.metadata.size()) return;
    FinishFileInfoCell(context, true);
    const HWND list = GetDlgItem(context.properties_page, 2164);
    RECT bounds{};
    ListView_GetSubItemRect(list, row, 1, LVIR_BOUNDS, &bounds);
    MapWindowPoints(list, context.properties_page, reinterpret_cast<POINT*>(&bounds), 2);
    const auto [name, value] = context.combined.metadata[row];
    context.inline_name = SafeUtf8(name);
    context.inline_edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW,
        value == context.strings.different ? L"" : value.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        context.properties_page, reinterpret_cast<HMENU>(0xe801), nullptr, nullptr);
    SetWindowSubclass(context.inline_edit, FileInfoCellProc, 1, reinterpret_cast<DWORD_PTR>(&context));
    SendMessageW(context.inline_edit, WM_SETFONT, SendMessageW(list, WM_GETFONT, 0, 0), TRUE);
    SendMessageW(context.inline_edit, EM_SETLIMITTEXT, detail::kFileInfoProbeMaximumCharacters, 0);
    SendMessageW(context.inline_edit, EM_SETSEL, 0, -1);
    SetFocus(context.inline_edit);
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
            CreateFileInfoEditorToolbar(*context);
            InitializeFileInfoLink(dialog);
            for (const auto name : audio::kTagGenres)
                SendDlgItemMessageW(dialog, 1030, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(SafeWide(name).c_str()));
            PopulateMp3PolicyControls(*context);
            for (const int identifier : kTagControls)
                SendDlgItemMessageW(dialog, identifier, EM_SETLIMITTEXT,
                                    4096, 0);
            PopulatePropertiesPage(*context);
        }
        return TRUE;
    }
    if (message == WM_CTLCOLORSTATIC && context && GetDlgCtrlID(reinterpret_cast<HWND>(value)) == 1066) {
        SetTextColor(reinterpret_cast<HDC>(wparam), RGB(0, 0, 255));
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
    }
    if (message == WM_APP + 0x418 && context && reinterpret_cast<HWND>(value) == context->inline_edit) {
        FinishFileInfoCell(*context, (wparam & 1) != 0);
        if (wparam & 2) SetFocus(GetDlgItem(dialog, 2164));
        return TRUE;
    }
    if (message == WM_CTLCOLOREDIT && context) {
        const HWND edit = reinterpret_cast<HWND>(value);
        int id = GetDlgCtrlID(edit);
        if (GetDlgCtrlID(GetParent(edit)) == 1030) id = 1030;
        const auto control = std::find(kTagControls.begin(), kTagControls.end(), id);
        if (control != kTagControls.end()) {
            const auto index = static_cast<size_t>(control - kTagControls.begin());
            bool changed{};
            for (size_t i = 0; i < context->drafts.size(); ++i)
                changed |= context->drafts[i].tags[index] != context->originals[i].tags[index];
            const auto color = changed ? RGB(0, 0, 255)
                : context->combined.tags[index] == context->strings.different ? RGB(255, 0, 0)
                    : GetSysColor(COLOR_WINDOWTEXT);
            SetTextColor(reinterpret_cast<HDC>(wparam), color);
            SetBkColor(reinterpret_cast<HDC>(wparam), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
        }
    }
    if (message == WM_NOTIFY && context) {
        if (FileInfoTooltip(context->resources, value)) return TRUE;
        const auto* info = reinterpret_cast<NMHDR*>(value);
        if (info->hwndFrom == context->toolbar && info->code == TBN_DROPDOWN) {
            FinishFileInfoCell(*context, true);
            const auto* toolbar = reinterpret_cast<NMTOOLBARW*>(value);
            const UINT choice = FileInfoTransformMenu(dialog, context->toolbar,
                context->resources, toolbar->iItem, context->codepage);
            if (choice) FileInfoTransform(*context, choice, toolbar->iItem == 0x86b ? choice : 0);
            return TRUE;
        }
        if (info->idFrom == 2164) {
            if (info->code == NM_CLICK) {
                const auto* click = reinterpret_cast<NMITEMACTIVATE*>(value);
                if (click->iSubItem == 1) BeginFileInfoCell(*context, click->iItem);
                return TRUE;
            }
            if (info->code == NM_DBLCLK) {
                FinishFileInfoCell(*context, true);
                EditAdvancedFileInfo(*context, false);
                return TRUE;
            }
            if (info->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(value)->wVKey == VK_DELETE) {
                SendMessageW(dialog, WM_COMMAND, 0x8078, 0);
                return TRUE;
            }
            if (info->code == LVN_ITEMCHANGED && !context->populating) UpdateSheetState(*context);
            if (info->code == NM_CUSTOMDRAW) {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(value);
                LRESULT result = CDRF_DODEFAULT;
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) result = CDRF_NOTIFYITEMDRAW;
                else if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) result = CDRF_NOTIFYSUBITEMDRAW;
                else if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) && draw->iSubItem == 1 &&
                         draw->nmcd.dwItemSpec < context->combined.metadata.size()) {
                    const auto& [name, text] = context->combined.metadata[draw->nmcd.dwItemSpec];
                    bool changed{};
                    for (size_t i = 0; i < context->drafts.size(); ++i)
                        changed |= detail::FileInfoField(RecordFields(context->drafts[i]), SafeUtf8(name)) !=
                            detail::FileInfoField(RecordFields(context->originals[i]), SafeUtf8(name));
                    draw->clrText = changed ? RGB(0, 0, 255) : text == context->strings.different
                        ? RGB(255, 0, 0) : GetSysColor(COLOR_WINDOWTEXT);
                    draw->clrTextBk = GetSysColor(COLOR_WINDOW);
                }
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
                return TRUE;
            }
        }
    }
    if (message == WM_COMMAND && context) {
        const UINT identifier = LOWORD(wparam);
        const UINT notification = HIWORD(wparam);
        if (identifier == kFileInfoAdvanced && notification == BN_CLICKED) {
            FinishFileInfoCell(*context, true);
            PopulateAdvancedMetadata(*context);
            SetAdvancedMetadataMode(*context, !context->advanced_mode);
            if (context->history_settings)
                context->history_settings->advance_file_info =
                    context->advanced_mode;
            return TRUE;
        }
        if (identifier == 1066) { OpenFileInfoDetails(*context, false); return TRUE; }
        if (context->loaded && !context->loading && !context->saving && !context->populating) {
            const auto control = std::find(kTagControls.begin(), kTagControls.end(), identifier);
            if (control != kTagControls.end() &&
                (notification == EN_CHANGE || (identifier == 1030 &&
                    (notification == CBN_EDITCHANGE || notification == CBN_SELCHANGE)))) {
                auto text = GetControlText(dialog, identifier);
                if (identifier == 1030 && notification == CBN_SELCHANGE) {
                    const auto selected = SendDlgItemMessageW(dialog, 1030, CB_GETCURSEL, 0, 0);
                    const auto length = SendDlgItemMessageW(dialog, 1030, CB_GETLBTEXTLEN, selected, 0);
                    if (selected != CB_ERR && length >= 0) {
                        text.resize(static_cast<size_t>(length) + 1);
                        SendDlgItemMessageW(dialog, 1030, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text.data()));
                        text.resize(length);
                    }
                }
                EditFileInfoField(*context, kTagNames[control - kTagControls.begin()], text, false);
                InvalidateRect(reinterpret_cast<HWND>(value), nullptr, FALSE);
                return TRUE;
            }
            if (context->combined.writable && notification == 0) {
                FinishFileInfoCell(*context, identifier != 0x8075);
                switch (identifier) {
                case 2160: GuessFileInfoTags(*context); return TRUE;
                case 0x8076: FileInfoTransform(*context, identifier); return TRUE;
                case 0x834:
                    for (auto& record : context->drafts) if (record.writable) {
                        for (const auto* name : kTagNames) SetRecordField(record, name, L"");
                        SetRecordField(record, "replaygain_track_gain", L"");
                    }
                    RefreshFileInfoDraft(*context);
                    return TRUE;
                case 0x8077: EditAdvancedFileInfo(*context, true); return TRUE;
                case 0x8079: EditAdvancedFileInfo(*context, false); return TRUE;
                case 0x8078: {
                    const int selected = ListView_GetNextItem(GetDlgItem(dialog, 2164), -1, LVNI_SELECTED);
                    if (selected >= 0 && static_cast<size_t>(selected) < context->combined.metadata.size()) {
                        const auto name = SafeUtf8(context->combined.metadata[selected].first);
                        EditFileInfoField(*context, name, L"");
                    }
                    return TRUE;
                }
                case 0x8075:
                    for (size_t i = 0; i < context->drafts.size(); ++i) {
                        auto cover = std::move(context->drafts[i].cover);
                        context->drafts[i] = context->originals[i];
                        context->drafts[i].cover = std::move(cover);
                    }
                    context->codepage = GetACP();
                    RefreshFileInfoDraft(*context);
                    return TRUE;
                }
            }
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
            InitializeFileInfoLink(dialog);
            PopulateCoverPage(*context);
        }
        return TRUE;
    }
    if (message == WM_CTLCOLORSTATIC && GetDlgCtrlID(reinterpret_cast<HWND>(value)) == 1066) {
        SetTextColor(reinterpret_cast<HDC>(wparam), RGB(0, 0, 255));
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
    }
    if (message == WM_COMMAND && context && LOWORD(wparam) == 1066) {
        OpenFileInfoDetails(*context, true);
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
            if (!bytes) {
                MessageBoxW(dialog, L"无法读取专辑封面：图片格式无效或超过允许的大小。",
                            nullptr, MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (bytes->size() > context->combined.cover_maximum_bytes) {
                wchar_t text[256]{};
                swprintf_s(text, FileInfoResource(context->resources, 0x821a).c_str(),
                           static_cast<int>(context->combined.cover_maximum_bytes));
                MessageBoxW(dialog, text, nullptr, MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            for (auto& record : context->drafts)
                if (record.cover_writable) record.cover = *bytes;
            context->cover_action =
                detail::FileInfoProbeCoverAction::replace;
            context->combined = CombineRecords(context->drafts, context->strings);
            PopulateCoverPage(*context);
            UpdateSheetState(*context);
            return TRUE;
        }
        if (identifier == 2221) {
            for (auto& record : context->drafts)
                if (record.cover_writable) record.cover.clear();
            context->cover_action =
                detail::FileInfoProbeCoverAction::remove;
            context->combined = CombineRecords(context->drafts, context->strings);
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
    if (okay) {
        ShowWindow(okay, SW_HIDE);
        EnableWindow(okay, FALSE);
    }
    SendMessageW(context.sheet, DM_SETDEFID, IDCANCEL, 0);

    RECT cancel_bounds{};
    GetWindowRect(cancel, &cancel_bounds);
    MapWindowPoints(nullptr, context.sheet,
                    reinterpret_cast<POINT*>(&cancel_bounds), 2);
    const int width = cancel_bounds.right - cancel_bounds.left;
    const int height = cancel_bounds.bottom - cancel_bounds.top;
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    const auto create = [&](UINT identifier, const std::wstring& text, int x, int button_width) {
        const HWND button = CreateWindowExW(
            0, WC_BUTTONW, text.c_str(), style, x, cancel_bounds.top, button_width,
            height, context.sheet, reinterpret_cast<HMENU>(
                static_cast<UINT_PTR>(identifier)), nullptr, nullptr);
        if (button) SendMessageW(button, WM_SETFONT,
            SendMessageW(cancel, WM_GETFONT, 0, 0), TRUE);
    };
    const int save_x = cancel_bounds.left - width - 16;
    create(kFileInfoSave, context.strings.save, save_x, width + 10);
    create(kFileInfoReload, context.strings.reload, save_x - width - 16, width + 10);
    detail::InstallOptionsBitmapButton(context.sheet, IDCANCEL, context.resources, 2);
    if (context.rows.size() == 1) {
        RECT client{};
        GetClientRect(context.sheet, &client);
        const int margin = client.right - cancel_bounds.right;
        create(kFileInfoPrevious, context.strings.previous, margin, width);
        create(kFileInfoNext, context.strings.next, margin + width + 6, width);
        detail::InstallOptionsBitmapButton(context.sheet, kFileInfoPrevious, context.resources, 0x412);
        detail::InstallOptionsBitmapButton(context.sheet, kFileInfoNext, context.resources, 0x415);
    }
}

LRESULT CALLBACK FileInfoSheetSubclass(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam,
                                       UINT_PTR, DWORD_PTR reference) {
    auto* context = reinterpret_cast<FileInfoContext*>(reference);
    if (!context) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_SYSCOMMAND && (wparam & 0xfff0U) == SC_CLOSE) {
        // A property sheet consumes SC_CLOSE internally; it need not forward
        // it as WM_CLOSE. Unwind the non-client handler before cancellation.
        PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    }
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
        if (command == IDCANCEL || command == IDOK) {
            SendMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        if (context->closing) return 0;
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
        context->closing = true;
        context->post_target.store(nullptr, std::memory_order_release);
        context->read_worker.request_stop();
        context->save_worker.request_stop();
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
    context->helper.clear(); // Embedded worker in this EXE.
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
    const INT_PTR created = ShowWtlPropertySheet(header);
    if (created <= 0) return;
    context->sheet = reinterpret_cast<HWND>(created);
    context->post_target.store(context->sheet, std::memory_order_release);
    if (!SetWindowSubclass(context->sheet, FileInfoSheetSubclass, 1,
                           reinterpret_cast<DWORD_PTR>(context.get()))) {
        DestroyWindow(context->sheet);
        return;
    }
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
        // A modeless property sheet may end its current page while keeping
        // the outer HWND alive (e.g. PSM_PRESSBUTTON or keyboard navigation).
        // Windows requires the host loop to destroy that completed sheet;
        // merely testing IsWindow leaves an uncloseable disabled shell.
        if (context->sheet && IsWindow(context->sheet) &&
            !SendMessageW(context->sheet, PSM_GETCURRENTPAGEHWND, 0, 0))
            break;
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
