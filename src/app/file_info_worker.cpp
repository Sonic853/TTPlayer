#include "ttplayer/platform/optional_windows_api.h"
#include "ttplayer/app/file_info_worker.h"
#include "../ui/file_info_cover_policy.h"
#include "../ui/file_info_probe_protocol.h"
#include "../ui/playlist_info_session_protocol.h"

#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/builtin_file_info.h"
#include "ttplayer/audio/format_probe.h"
#include "ttplayer/audio/midi_player.h"
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
#include <system_error>
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
    result.cover_writable = reader.HasThumbnailInterface() &&
                            (reader.Capabilities() & 4U) != 0;
    result.cover_maximum_bytes = reader.MaximumThumbnailBytes();
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
    result.cover_writable = (source.capabilities & 4U) != 0;
    result.cover_maximum_bytes = ttplayer::ui::detail::kFileInfoProbeMaximumCoverBytes;
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
    if (ttplayer::audio::IsMidiPath(path)) return true;
    std::wstring hint;
    if (FAILED(ttplayer::audio::ProbeAudioFormatHint(path, hint))) return false;
    return hint == L".mp3" || hint == L".mp2" || hint == L".mp1" ||
           hint == L".mpa" || hint == L".mp3pro" || hint == L".wav" || hint == L".wave";
}

HRESULT TryBuiltinRead(const std::filesystem::path& path,
                       const ttplayer::ui::detail::FileInfoProbeMp3Policy& policy,
                       ttplayer::ui::detail::FileInfoProbeReadResult& result,
                       bool allow_system_fallback = true) {
    ttplayer::audio::ArchiveMemberPath archive;
    if (ttplayer::audio::ParseArchiveMemberPath(path.native(), archive))
        return E_NOINTERFACE;
    ttplayer::audio::BuiltinFileInfo builtin;
    const HRESULT status = ttplayer::audio::ReadBuiltinFileInfo(
        path, ConvertPolicy(policy), builtin, allow_system_fallback);
    if (SUCCEEDED(status)) PopulateBuiltinResult(std::move(builtin), result);
    return status;
}

std::unique_ptr<ttplayer::plugins::LegacyReaderSession> OpenReader(
    const ttplayer::plugins::PluginManager& manager,
    const std::filesystem::path& logical_path, HMODULE ttpcomm,
    HRESULT* result) {
    ttplayer::audio::ArchiveMemberPath member;
    if (!ttplayer::audio::ParseArchiveMemberPath(logical_path.native(), member))
        return manager.OpenReaderForInspection(logical_path, result);

    try {
        const auto bytes = ttplayer::audio::ReadArchiveMember(member, ttpcomm);
        if (bytes.size() > static_cast<size_t>(UINT_MAX)) {
            if (result) *result = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            return {};
        }
        IStream* stream = ttplayer::platform::SHCreateMemStream(
            bytes.empty() ? nullptr : bytes.data(),
            static_cast<UINT>(bytes.size()));
        if (!stream) {
            if (result) *result = E_OUTOFMEMORY;
            return {};
        }
        auto reader = manager.OpenReaderForInspection(logical_path, stream, result);
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
        result.status = TryBuiltinRead(logical_path, request.mp3, result, false);
        completed = SUCCEEDED(result.status);
    }
    const HRESULT loaded = completed ? S_OK : manager.Load(addin_directory);
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

class PlaylistReaderContext {
public:
    ttplayer::plugins::PluginManager manager;
    std::filesystem::path addin, comm_path;
    HMODULE comm{};
    bool load_attempted{};
    HRESULT loaded{E_FAIL};
    PlaylistReaderContext(std::filesystem::path directory, std::filesystem::path library)
        : addin(std::move(directory)), comm_path(std::move(library)) {}
    ~PlaylistReaderContext() {
        manager.Shutdown();
        if (comm) FreeLibrary(comm);
    }
    HRESULT Load() {
        if (!load_attempted) { load_attempted = true; loaded = manager.Load(addin); }
        return loaded;
    }
    HMODULE ArchiveLibrary() {
        if (!comm && !comm_path.empty()) comm = LoadLibraryExW(
            comm_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        return comm;
    }
};

ttplayer::ui::detail::FileInfoProbeReadResult ReadPlaylistMetadata(
    PlaylistReaderContext& context, const std::filesystem::path& logical_path,
    int subtrack, const ttplayer::ui::detail::FileInfoProbeMp3Policy& policy,
    bool include_cover = false) {
    using namespace ttplayer;
    ui::detail::FileInfoProbeReadResult result;
    if (_wcsicmp(logical_path.extension().c_str(), L".cue") == 0) {
        try {
            audio::ArchiveMemberPath member;
            const bool archived = audio::ParseArchiveMemberPath(logical_path.native(), member);
            const auto sheet = archived ? audio::CueSheet::LoadFromMemory(
                audio::ReadArchiveMember(member, context.ArchiveLibrary()), logical_path)
                : audio::CueSheet::Load(logical_path);
            const auto* track = sheet.FindTrack(subtrack);
            if (!track || !track->has_index01) {
                result.status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                return result;
            }
            for (const auto& candidate : sheet.AudioCandidates(*track)) {
                result = ReadPlaylistMetadata(context, candidate, 0, policy, false);
                if (SUCCEEDED(result.status)) break;
            }
            if (FAILED(result.status)) return result;
            const auto start = static_cast<std::int64_t>(track->start_frame * 1000U / 75U);
            if (result.duration_ms && start > result.duration_ms) {
                result.status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                return result;
            }
            const auto duration = track->DurationMilliseconds();
            result.duration_ms = static_cast<DWORD>(std::clamp<std::int64_t>(
                duration >= 0 ? duration : static_cast<std::int64_t>(result.duration_ms) - start,
                0, MAXDWORD));
            result.metadata.clear();
            for (const auto& [key, value] : track->metadata)
                result.metadata.push_back({key, value});
            result.cover.clear();
            result.cover_writable = result.cover_maximum_bytes = 0;
            result.capabilities &= ~4U;
            const auto attributes = archived ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(logical_path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_READONLY))
                result.capabilities |= 4U;
            return result;
        } catch (const std::exception&) {
            result.status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            return result;
        }
    }
    audio::ArchiveMemberPath archive;
    const auto comm = audio::ParseArchiveMemberPath(logical_path.native(), archive)
        ? context.ArchiveLibrary() : nullptr;
    bool completed{};
    if (IsDirectBuiltinPath(logical_path)) {
        result.status = TryBuiltinRead(logical_path, policy, result, false);
        completed = SUCCEEDED(result.status);
    }
    if (!completed && SUCCEEDED(context.Load())) {
        HRESULT opened{};
        if (auto reader = OpenReader(context.manager, logical_path, comm, &opened)) {
            PopulateReaderResult(*reader, result, include_cover);
            completed = true;
        } else result.status = FAILED(opened) ? opened : E_NOINTERFACE;
    }
    if (!completed) result.status = TryBuiltinRead(logical_path, policy, result);
    if (!include_cover) result.cover.clear();
    return result;
}

int ReadCueFileInfo(const std::filesystem::path& addin,
    const std::filesystem::path& path, const std::filesystem::path& comm,
    int subtrack, const std::filesystem::path& request_path,
    const std::filesystem::path& output) {
    ttplayer::ui::detail::FileInfoProbeReadRequest request;
    if (!ttplayer::ui::detail::ReadFileInfoProbeReadRequest(request_path, request)) return 3;
    PlaylistReaderContext context(addin, comm);
    return ttplayer::ui::detail::WriteFileInfoProbeReadResult(output,
        ReadPlaylistMetadata(context, path, subtrack, request.mp3, true)) ? 0 : 4;
}

int ReadPlaylistInfo(const std::filesystem::path& addin,
                     const std::filesystem::path& path,
                     const std::filesystem::path& comm, int subtrack,
                     const std::filesystem::path& request_path,
                     const std::filesystem::path& output) {
    ttplayer::ui::detail::FileInfoProbeReadRequest request;
    if (!ttplayer::ui::detail::ReadFileInfoProbeReadRequest(request_path, request)) return 3;
    PlaylistReaderContext context(addin, comm);
    return ttplayer::ui::detail::WriteFileInfoProbeReadResult(output,
        ReadPlaylistMetadata(context, path, subtrack, request.mp3)) ? 0 : 4;
}

int ServePlaylistInfo(wchar_t** args) {
    using namespace ttplayer::ui::detail;
    const HANDLE incoming = OpenEventW(SYNCHRONIZE, FALSE, args[6]);
    const HANDLE outgoing = OpenEventW(EVENT_MODIFY_STATE, FALSE, args[7]);
    if (!incoming || !outgoing) {
        if (incoming) CloseHandle(incoming);
        if (outgoing) CloseHandle(outgoing);
        return 3;
    }
    int code{};
    {
        PlaylistReaderContext context(args[2], args[3]);
        // Idle retirement also bounds lifetime if the parent exits without a
        // usable job object. The parent restarts an idle child on the next read.
        while (WaitForSingleObject(incoming, 60000) == WAIT_OBJECT_0) {
            PlaylistInfoSessionRequest request;
            if (!ReadPlaylistInfoSessionRequest(args[4], request) ||
                !WritePlaylistInfoSessionResult(args[5], request.id, ReadPlaylistMetadata(
                    context, request.path, request.subtrack, request.mp3)) ||
                !SetEvent(outgoing)) { code = 4; break; }
        }
    }
    CloseHandle(outgoing);
    CloseHandle(incoming);
    return code;
}

int WriteFileInfo(const std::filesystem::path& addin_directory,
                  const std::filesystem::path& logical_path,
                  const std::filesystem::path& /*ttpcomm_path*/,
                  const std::filesystem::path& request_path,
                  const std::filesystem::path& output, int subtrack = 0) {
    ttplayer::ui::detail::FileInfoProbeWriteRequest request;
    if (!ttplayer::ui::detail::ReadFileInfoProbeWriteRequest(
            request_path, request))
        return 3;

    ttplayer::ui::detail::FileInfoProbeWriteResult result;
    HMODULE ttpcomm{};
    ttplayer::audio::ArchiveMemberPath archive;
    if (ttplayer::audio::ParseArchiveMemberPath(logical_path.native(), archive)) {
        result.status = E_ACCESSDENIED;
        if (request.cover_action != ttplayer::ui::detail::FileInfoProbeCoverAction::unchanged)
            result.cover_status = E_ACCESSDENIED;
        return ttplayer::ui::detail::WriteFileInfoProbeWriteResult(output, result) ? 0 : 4;
    }

    if (_wcsicmp(logical_path.extension().c_str(), L".cue") == 0) {
        using namespace ttplayer;
        result.status = S_OK;
        audio::CueMetadata fields;
        result.fields.assign(request.fields.size(), S_OK);
        for (size_t i = 0; i < request.fields.size(); ++i) {
            const auto& field = request.fields[i];
            const std::wstring name(field.name.begin(), field.name.end());
            if (!audio::CueSheet::IsWritableField(name)) {
                result.fields[i] = E_ACCESSDENIED;
                result.status = E_ACCESSDENIED;
            } else fields.emplace_back(name, field.value);
        }
        if (request.cover_action != ui::detail::FileInfoProbeCoverAction::unchanged)
            result.status = result.cover_status = E_ACCESSDENIED;
        try {
            if (!fields.empty()) audio::CueSheet::Load(logical_path).WriteTrackMetadata(subtrack, fields);
            else if (!audio::CueSheet::Load(logical_path).FindTrack(subtrack))
                result.status = E_INVALIDARG;
        } catch (const std::system_error& error) {
            result.status = HRESULT_FROM_WIN32(error.code().value());
            for (auto& status : result.fields) if (SUCCEEDED(status)) status = result.status;
        } catch (const std::exception&) {
            result.status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            for (auto& status : result.fields) if (SUCCEEDED(status)) status = result.status;
        }
        return ui::detail::WriteFileInfoProbeWriteResult(output, result) ? 0 : 4;
    }

    ttplayer::plugins::PluginManager manager;
    bool completed{};
    ttplayer::audio::BuiltinFileInfo mpeg;
    if (SUCCEEDED(ttplayer::audio::ReadBuiltinMpegFileInfo(
            logical_path, ConvertPolicy(request.mp3), mpeg))) {
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
    const HRESULT loaded = completed ? S_OK : manager.Load(addin_directory);
    if (!completed && SUCCEEDED(loaded)) {
        HRESULT opened{};
        auto reader = manager.OpenReaderForMetadata(logical_path, &opened);
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
    if (_wcsicmp(arguments[1], L"playlist-session") == 0 && count == 8) {
        exit_code = ServePlaylistInfo(arguments);
    } else if (_wcsicmp(arguments[1], L"read") == 0 && count == 7) {
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
    } else if ((_wcsicmp(arguments[1], L"cue-read") == 0 ||
                _wcsicmp(arguments[1], L"cue-write") == 0) && count == 8) {
        wchar_t* end{};
        const long subtrack = std::wcstol(arguments[5], &end, 10);
        if (end && *end == L'\0' && subtrack > 0 && subtrack <= INT_MAX) {
            if (_wcsicmp(arguments[1], L"cue-read") == 0)
                exit_code = ReadCueFileInfo(arguments[2], arguments[3], arguments[4],
                    static_cast<int>(subtrack), arguments[6], arguments[7]);
            else
                exit_code = WriteFileInfo(arguments[2], arguments[3], arguments[4],
                    arguments[6], arguments[7], static_cast<int>(subtrack));
        }
    } else if (_wcsicmp(arguments[1], L"write") == 0 && count == 7) {
        exit_code = WriteFileInfo(arguments[2], arguments[3], arguments[4],
                                  arguments[5], arguments[6]);
    }
    if (owns_apartment) CoUninitialize();
    return exit_code;
}
