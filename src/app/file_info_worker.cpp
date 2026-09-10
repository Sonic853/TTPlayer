#include "ttplayer/app/file_info_worker.h"
#include "../ui/file_info_cover_policy.h"
#include "../ui/file_info_probe_protocol.h"

#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/builtin_file_info.h"
#include "ttplayer/audio/cue_sheet.h"
#include "ttplayer/plugins/plugin_manager.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <objbase.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>
#include <shlwapi.h>

namespace {

bool EqualsAsciiInsensitive(std::wstring_view left,
                            std::wstring_view right) {
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

std::wstring MetadataValue(
    const std::vector<ttplayer::plugins::MetadataEntry>& entries,
    std::wstring_view name) {
    const auto found = std::find_if(entries.begin(), entries.end(),
        [name](const auto& entry) {
            return EqualsAsciiInsensitive(entry.name, name);
        });
    return found == entries.end() ? std::wstring{} : found->value;
}

void MergeMetadata(std::vector<ttplayer::plugins::MetadataEntry>& entries,
                   std::wstring name, std::wstring value) {
    if (name.empty() || value.empty()) return;
    const auto found = std::find_if(entries.begin(), entries.end(),
        [&name](const auto& entry) {
            return EqualsAsciiInsensitive(entry.name, name);
        });
    if (found == entries.end())
        entries.push_back({std::move(name), std::move(value)});
    else
        found->value = std::move(value);
}

void PopulateReaderResult(
    ttplayer::plugins::LegacyReaderSession& reader,
    ttplayer::ui::detail::FileInfoProbeReadResult& result,
    bool include_cover) {
    result.status = S_OK;
    result.capabilities = reader.Capabilities();
    result.format = reader.Format();
    result.duration_ms = static_cast<DWORD>(std::clamp<std::int64_t>(
        reader.DurationMilliseconds(), 0,
        static_cast<std::int64_t>(MAXDWORD)));
    result.encoded_bits_per_second = static_cast<DWORD>(
        std::min<std::uint64_t>(reader.EncodedBitsPerSecond(), MAXDWORD));
    result.codec = reader.CodecName();
    if (include_cover) result.cover = reader.Thumbnail();

    auto entries = reader.Metadata();
    auto title = MetadataValue(entries, L"title");
    auto artist = MetadataValue(entries, L"artist");
    if (artist.empty()) artist = MetadataValue(entries, L"author");
    auto album = MetadataValue(entries, L"album");
    const auto read_known = [&reader, &entries](
                                const char* narrow, const wchar_t* wide,
                                std::wstring& destination) {
        if (!destination.empty()) return;
        const auto value = reader.MetadataValue(narrow);
        if (!value || value->empty()) return;
        destination = *value;
        MergeMetadata(entries, wide, *value);
    };
    read_known("title", L"Title", title);
    read_known("artist", L"Artist", artist);
    if (artist.empty()) read_known("author", L"Artist", artist);
    read_known("album", L"Album", album);
    MergeMetadata(entries, L"Title", title);
    MergeMetadata(entries, L"Artist", artist);
    MergeMetadata(entries, L"Album", album);

    result.metadata.reserve(std::min<size_t>(
        entries.size(), ttplayer::ui::detail::kFileInfoProbeMaximumFields));
    for (const auto& entry : entries) {
        if (result.metadata.size() >=
            ttplayer::ui::detail::kFileInfoProbeMaximumFields) break;
        result.metadata.push_back({entry.name, entry.value});
    }
}

ttplayer::audio::Mp3TagPolicy ConvertPolicy(
    const ttplayer::ui::detail::FileInfoProbeMp3Policy& policy) noexcept {
    return {policy.read_priority, policy.write_type, policy.id3v2_encoding,
            policy.id3v2_padding != 0};
}

void PopulateBuiltinResult(
    ttplayer::audio::BuiltinFileInfo source,
    ttplayer::ui::detail::FileInfoProbeReadResult& result) {
    result.status = S_OK;
    result.capabilities = source.capabilities;
    result.format = source.format;
    result.duration_ms = source.duration_ms;
    result.encoded_bits_per_second = source.encoded_bits_per_second;
    result.codec = std::move(source.codec);
    result.cover = std::move(source.cover);
    result.metadata.reserve(source.metadata.size());
    for (auto& field : source.metadata)
        result.metadata.push_back(
            {std::move(field.name), std::move(field.value)});
}

bool IsDirectBuiltinPath(const std::filesystem::path& path) {
    return _wcsicmp(path.extension().c_str(), L".mp3") == 0 ||
           _wcsicmp(path.extension().c_str(), L".wav") == 0 ||
           _wcsicmp(path.extension().c_str(), L".wave") == 0;
}

HRESULT TryBuiltinRead(const std::filesystem::path& path,
                       const ttplayer::ui::detail::FileInfoProbeMp3Policy& policy,
                       ttplayer::ui::detail::FileInfoProbeReadResult& result) {
    ttplayer::audio::ArchiveMemberPath archive;
    if (ttplayer::audio::ParseArchiveMemberPath(path.native(), archive))
        return E_NOINTERFACE;
    ttplayer::audio::BuiltinFileInfo builtin;
    const HRESULT status = ttplayer::audio::ReadBuiltinFileInfo(
        path, ConvertPolicy(policy), builtin);
    if (SUCCEEDED(status)) PopulateBuiltinResult(std::move(builtin), result);
    return status;
}

std::unique_ptr<ttplayer::plugins::LegacyReaderSession> OpenReader(
    const ttplayer::plugins::PluginManager& manager,
    const std::filesystem::path& logical_path, HMODULE ttpcomm,
    HRESULT* result) {
    ttplayer::audio::ArchiveMemberPath member;
    if (!ttplayer::audio::ParseArchiveMemberPath(logical_path.native(), member))
        return manager.OpenReader(logical_path, result);

    try {
        const auto bytes = ttplayer::audio::ReadArchiveMember(member, ttpcomm);
        if (bytes.size() > static_cast<size_t>(UINT_MAX)) {
            if (result) *result = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            return {};
        }
        IStream* stream = SHCreateMemStream(
            bytes.empty() ? nullptr : bytes.data(),
            static_cast<UINT>(bytes.size()));
        if (!stream) {
            if (result) *result = E_OUTOFMEMORY;
            return {};
        }
        auto reader = manager.OpenReader(logical_path, stream, result);
        stream->Release();
        return reader;
    } catch (const std::exception&) {
        if (result) *result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return {};
    }
}

int ReadFileInfo(const std::filesystem::path& addin_directory,
                 const std::filesystem::path& logical_path,
                 const std::filesystem::path& ttpcomm_path,
                 const std::filesystem::path& request_path,
                 const std::filesystem::path& output) {
    ttplayer::ui::detail::FileInfoProbeReadResult result;
    ttplayer::ui::detail::FileInfoProbeReadRequest request;
    if (!ttplayer::ui::detail::ReadFileInfoProbeReadRequest(
            request_path, request))
        return 3;
    HMODULE ttpcomm{};
    ttplayer::audio::ArchiveMemberPath archive;
    if (ttplayer::audio::ParseArchiveMemberPath(logical_path.native(), archive))
        ttpcomm = LoadLibraryExW(ttpcomm_path.c_str(), nullptr,
                                LOAD_WITH_ALTERED_SEARCH_PATH);

    ttplayer::plugins::PluginManager manager;
    bool completed{};
    if (IsDirectBuiltinPath(logical_path)) {
        result.status = TryBuiltinRead(logical_path, request.mp3, result);
        completed = SUCCEEDED(result.status);
    }
    const HRESULT loaded = manager.Load(addin_directory);
    if (!completed && SUCCEEDED(loaded)) {
        HRESULT opened{};
        auto reader = OpenReader(manager, logical_path, ttpcomm, &opened);
        if (!reader) {
            result.status = FAILED(opened) ? opened : E_NOINTERFACE;
        } else {
            PopulateReaderResult(*reader, result, true);
            completed = true;
        }
    }
    if (!completed)
        result.status = TryBuiltinRead(logical_path, request.mp3, result);
    manager.Shutdown();
    if (ttpcomm) FreeLibrary(ttpcomm);
    return ttplayer::ui::detail::WriteFileInfoProbeReadResult(output, result)
        ? 0 : 4;
}

int ReadPlaylistInfo(const std::filesystem::path& addin_directory,
                     const std::filesystem::path& logical_path,
                     const std::filesystem::path& ttpcomm_path,
                     int subtrack, const std::filesystem::path& request_path,
                     const std::filesystem::path& output) {
    ttplayer::ui::detail::FileInfoProbeReadResult result;
    ttplayer::ui::detail::FileInfoProbeReadRequest request;
    if (!ttplayer::ui::detail::ReadFileInfoProbeReadRequest(
            request_path, request))
        return 3;
    HMODULE ttpcomm = ttpcomm_path.empty() ? nullptr :
        LoadLibraryExW(ttpcomm_path.c_str(), nullptr,
                       LOAD_WITH_ALTERED_SEARCH_PATH);
    std::filesystem::path decoder_path = logical_path;
    std::optional<std::int64_t> cue_duration;
    std::wstring cue_title;
    std::wstring cue_artist;
    std::wstring cue_album;
    int cue_track_number{};

    if (subtrack > 0 &&
        _wcsicmp(logical_path.extension().c_str(), L".cue") == 0) {
        try {
            ttplayer::audio::CueSheet sheet;
            ttplayer::audio::ArchiveMemberPath member;
            if (ttplayer::audio::ParseArchiveMemberPath(
                    logical_path.native(), member)) {
                sheet = ttplayer::audio::CueSheet::LoadFromMemory(
                    ttplayer::audio::ReadArchiveMember(member, ttpcomm),
                    logical_path);
            } else {
                sheet = ttplayer::audio::CueSheet::Load(logical_path);
            }
            const auto* track = sheet.FindTrack(subtrack);
            if (!track) {
                result.status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            } else {
                decoder_path = track->audio_path;
                cue_title = track->title;
                cue_artist = track->performer.empty()
                    ? sheet.Performer() : track->performer;
                cue_album = sheet.Title();
                cue_track_number = track->number;
                const auto duration = track->DurationMilliseconds();
                cue_duration = duration >= 0
                    ? duration
                    : -static_cast<std::int64_t>(
                        track->start_frame * 1000U / 75U);
            }
        } catch (const std::exception&) {
            result.status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
    }

    ttplayer::plugins::PluginManager manager;
    if (result.status == E_FAIL) {
        bool completed{};
        if (IsDirectBuiltinPath(decoder_path)) {
            result.status = TryBuiltinRead(decoder_path, request.mp3, result);
            completed = SUCCEEDED(result.status);
        }
        const HRESULT loaded = manager.Load(addin_directory);
        if (!completed && SUCCEEDED(loaded)) {
            if (manager.HasReaderForPath(decoder_path)) {
                HRESULT opened{};
                auto reader = OpenReader(manager, decoder_path, ttpcomm,
                                         &opened);
                if (!reader) {
                    result.status = FAILED(opened) ? opened : E_NOINTERFACE;
                } else {
                    PopulateReaderResult(*reader, result, false);
                    completed = true;
                }
            }
        }
        if (!completed) {
            result.status = TryBuiltinRead(decoder_path, request.mp3, result);
            completed = SUCCEEDED(result.status);
        }
        if (completed) {
                    if (cue_duration) {
                        const auto duration = *cue_duration >= 0
                            ? *cue_duration
                            : std::max<std::int64_t>(0,
                                static_cast<std::int64_t>(result.duration_ms) +
                                    *cue_duration);
                        result.duration_ms = static_cast<DWORD>(
                            std::clamp<std::int64_t>(duration, 0,
                                static_cast<std::int64_t>(MAXDWORD)));
                    }
                    const auto merge = [&result](std::wstring name,
                                                 std::wstring value) {
                        if (value.empty()) return;
                        const auto found = std::find_if(
                            result.metadata.begin(), result.metadata.end(),
                            [&name](const auto& entry) {
                                return EqualsAsciiInsensitive(
                                    entry.name, name);
                            });
                        if (found != result.metadata.end()) {
                            found->value = std::move(value);
                        } else if (result.metadata.size() <
                                ttplayer::ui::detail::
                                    kFileInfoProbeMaximumFields) {
                            result.metadata.push_back(
                                {std::move(name), std::move(value)});
                        }
                    };
                    merge(L"Title", std::move(cue_title));
                    merge(L"Artist", std::move(cue_artist));
                    merge(L"Album", std::move(cue_album));
                    if (cue_track_number > 0)
                        merge(L"Tracknumber",
                              std::to_wstring(cue_track_number));
                }
    }
    manager.Shutdown();
    if (ttpcomm) FreeLibrary(ttpcomm);
    return ttplayer::ui::detail::WriteFileInfoProbeReadResult(output, result)
        ? 0 : 4;
}

int WriteFileInfo(const std::filesystem::path& addin_directory,
                  const std::filesystem::path& logical_path,
                  const std::filesystem::path& ttpcomm_path,
                  const std::filesystem::path& request_path,
                  const std::filesystem::path& output) {
    ttplayer::ui::detail::FileInfoProbeWriteRequest request;
    if (!ttplayer::ui::detail::ReadFileInfoProbeWriteRequest(
            request_path, request))
        return 3;

    ttplayer::ui::detail::FileInfoProbeWriteResult result;
    HMODULE ttpcomm{};
    ttplayer::audio::ArchiveMemberPath archive;
    if (ttplayer::audio::ParseArchiveMemberPath(logical_path.native(), archive))
        ttpcomm = LoadLibraryExW(ttpcomm_path.c_str(), nullptr,
                                LOAD_WITH_ALTERED_SEARCH_PATH);

    ttplayer::plugins::PluginManager manager;
    bool completed{};
    if (_wcsicmp(logical_path.extension().c_str(), L".mp3") == 0 &&
        !ttplayer::audio::ParseArchiveMemberPath(logical_path.native(), archive)) {
        std::vector<ttplayer::audio::BuiltinTagWriteField> fields;
        fields.reserve(request.fields.size());
        for (const auto& field : request.fields)
            fields.push_back({field.name, field.value});
        const auto cover_action = request.cover_action ==
                ttplayer::ui::detail::FileInfoProbeCoverAction::replace
            ? ttplayer::audio::BuiltinCoverAction::replace
            : request.cover_action ==
                    ttplayer::ui::detail::FileInfoProbeCoverAction::remove
                ? ttplayer::audio::BuiltinCoverAction::remove
                : ttplayer::audio::BuiltinCoverAction::unchanged;
        auto written = ttplayer::audio::WriteBuiltinFileInfo(
            logical_path, ConvertPolicy(request.mp3), fields, cover_action,
            request.cover);
        result.status = written.status;
        result.fields = std::move(written.fields);
        result.cover_status = written.cover_status;
        completed = true;
    }
    const HRESULT loaded = manager.Load(addin_directory);
    if (!completed && SUCCEEDED(loaded)) {
        HRESULT opened{};
        auto reader = OpenReader(manager, logical_path, ttpcomm, &opened);
        if (!reader) {
            result.status = FAILED(opened) ? opened : E_NOINTERFACE;
        } else {
            result.fields.reserve(request.fields.size());
            result.status = S_OK;
            for (const auto& field : request.fields) {
                const HRESULT written = reader->SetMetadataValue(
                    field.name, field.value);
                result.fields.push_back(written);
                if (FAILED(written) && SUCCEEDED(result.status))
                    result.status = written;
            }
            if (request.cover_action !=
                    ttplayer::ui::detail::FileInfoProbeCoverAction::unchanged) {
                if (request.cover_action ==
                        ttplayer::ui::detail::FileInfoProbeCoverAction::remove) {
                    result.cover_status = reader->ClearThumbnails();
                } else {
                    const auto mime =
                        ttplayer::ui::detail::CoverMimeType(request.cover);
                    if (mime.empty()) {
                        result.cover_status =
                            HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                    } else {
                        ttplayer::plugins::ThumbnailData cover;
                        cover.mime = mime;
                        cover.bytes = request.cover;
                        // APIC/WM/Picture type 3 is the native "front cover"
                        // value used by the file-information page.
                        cover.picture_type = 3;
                        result.cover_status =
                            reader->ReplaceThumbnails(
                                std::span<const ttplayer::plugins::ThumbnailData>(
                                    &cover, 1));
                    }
                }
                if (SUCCEEDED(result.status)) result.status = result.cover_status;
            }
            // Several readers commit their tag transaction from Release.
            reader.reset();
            completed = true;
        }
    }
    if (!completed) result.status = FAILED(loaded) ? loaded : E_NOINTERFACE;
    manager.Shutdown();
    if (ttpcomm) FreeLibrary(ttpcomm);
    return ttplayer::ui::detail::WriteFileInfoProbeWriteResult(output, result)
        ? 0 : 4;
}

} // namespace

int ttplayer::app::RunFileInfoWorker(int count, wchar_t** arguments) {
    if (count != 7 && count != 8) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool owns_apartment = SUCCEEDED(apartment);
    int exit_code{2};
    if (_wcsicmp(arguments[1], L"read") == 0 && count == 7) {
        exit_code = ReadFileInfo(arguments[2], arguments[3], arguments[4],
                                 arguments[5], arguments[6]);
    } else if (_wcsicmp(arguments[1], L"playlist-read") == 0 && count == 8) {
        wchar_t* end{};
        const long subtrack = std::wcstol(arguments[5], &end, 10);
        if (end && *end == L'\0' && subtrack >= 0 && subtrack <= INT_MAX) {
            exit_code = ReadPlaylistInfo(arguments[2], arguments[3],
                arguments[4], static_cast<int>(subtrack), arguments[6],
                arguments[7]);
        }
    } else if (_wcsicmp(arguments[1], L"write") == 0 && count == 7) {
        exit_code = WriteFileInfo(arguments[2], arguments[3], arguments[4],
                                  arguments[5], arguments[6]);
    }
    if (owns_apartment) CoUninitialize();
    return exit_code;
}
