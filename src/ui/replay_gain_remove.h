#pragma once

#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/builtin_file_info.h"
#include "ttplayer/audio/cue_sheet.h"
#include "ttplayer/audio/disc_media.h"
#include "ttplayer/playlist/playlist.h"
#include "ttplayer/plugins/plugin_manager.h"

#include <algorithm>
#include <cwchar>
#include <new>
#include <system_error>
#include <vector>

namespace ttplayer::ui::detail {

struct ReplayGainRemovalFailure {
    playlist::Track track;
    HRESULT result{E_FAIL};
};

// The existing playlist_metadata_working_ release/acquire pair publishes this
// immutable result. The detached worker owns no PlayerWindow or HWND.
struct ReplayGainRemovalResult {
    std::vector<playlist::Track> succeeded;
    std::vector<ReplayGainRemovalFailure> failed;
    HRESULT worker_result{S_OK};
    bool canceled{};
};

inline bool IsReplayGainTrackField(const wchar_t* name) noexcept {
    return !_wcsicmp(name, L"replaygain_track_gain") ||
           !_wcsicmp(name, L"replaygain_track_peak");
}

inline bool HasReplayGainTrackValues(const audio::CueMetadata& fields) {
    return std::any_of(fields.begin(), fields.end(), [](const auto& field) {
        return !field.second.empty() && IsReplayGainTrackField(field.first.c_str());
    });
}

inline bool HasReplayGainTrackValues(const audio::BuiltinFileInfo& info) {
    return std::any_of(info.metadata.begin(), info.metadata.end(), [](const auto& field) {
        return !field.value.empty() && IsReplayGainTrackField(field.name.c_str());
    });
}

// Preserve the logical track identity. CUE edits must target the selected
// subtrack, never its referenced audio file or every track in the sheet.
inline HRESULT RemoveReplayGainTags(const plugins::PluginManager* library,
                                    const playlist::Track& track) noexcept {
    try {
        const auto& path = track.path;
        if (path.empty()) return E_INVALIDARG;
        audio::ArchiveMemberPath member;
        if (path.native().find(L"://") != std::wstring::npos ||
            audio::ParseArchiveMemberPath(path.native(), member)) return E_ACCESSDENIED;
        if (!_wcsicmp(path.extension().c_str(), L".cue")) {
            if (track.subtrack <= 0) return E_INVALIDARG;
            auto sheet = audio::CueSheet::Load(path);
            if (!sheet.FindTrack(track.subtrack)) return E_INVALIDARG;
            sheet.WriteTrackMetadata(track.subtrack, {
                {L"replaygain_track_gain", L""}, {L"replaygain_track_peak", L""}});
            const auto saved = audio::CueSheet::Load(path);
            const auto* selected = saved.FindTrack(track.subtrack);
            return selected && !HasReplayGainTrackValues(selected->metadata)
                ? S_OK : STG_E_WRITEFAULT;
        }
        if (!_wcsicmp(path.extension().c_str(), L".cda")) {
            const auto disc = audio::ReadDiscLayout(path);
            const auto number = audio::CdaTrackNumber(path);
            audio::WriteDiscMetadata(disc, {{number, {
                {L"replaygain_track_gain", L""}, {L"replaygain_track_peak", L""}}}});
            const auto saved = audio::ReadDiscMetadata(disc);
            const auto found = saved.find(number);
            return found == saved.end() || !HasReplayGainTrackValues(found->second)
                ? S_OK : STG_E_WRITEFAULT;
        }
        if (track.subtrack > 0) return E_NOTIMPL;

        // The MPEG writer validates frames, not the suffix. Use the same
        // bounded parser for the fresh read, including a mislabeled MP3.
        audio::BuiltinFileInfo mpeg;
        if (SUCCEEDED(audio::ReadBuiltinMpegFileInfo(path, {}, mpeg))) {
            const HRESULT written = audio::WriteBuiltinMpegReplayGain(path, {}, {});
            if (FAILED(written)) return written;
            mpeg = {};
            const HRESULT read = audio::ReadBuiltinMpegFileInfo(path, {}, mpeg);
            if (FAILED(read)) return read;
            return HasReplayGainTrackValues(mpeg) ? STG_E_WRITEFAULT : S_OK;
        }
        if (!library) return E_NOINTERFACE;

        HRESULT opened{};
        auto writer = library->OpenReaderForMetadata(path, &opened);
        if (!writer) return FAILED(opened) ? opened : E_NOINTERFACE;
        // 00484009 calls metadata slot 6 directly, without the File Properties
        // capability-bit guard. Old FLAC writers can report bit 4 clear.
        const HRESULT gain = writer->SetMetadataValueDirect("replaygain_track_gain", {});
        const HRESULT peak = writer->SetMetadataValueDirect("replaygain_track_peak", {});
        const HRESULT committed = writer->CommitMetadata();
        writer.reset(); // Legacy writers commit on the last Release.
        if (FAILED(gain)) return gain;
        if (FAILED(peak)) return peak;
        if (FAILED(committed)) return committed;

        const auto saved = library->OpenReaderForInspection(path, &opened);
        if (!saved) return FAILED(opened) ? opened : STG_E_READFAULT;
        for (const auto* name : {"replaygain_track_gain", "replaygain_track_peak"}) {
            std::wstring value;
            const HRESULT read = saved->ReadMetadataValue(name, value);
            if (FAILED(read)) return read;
            if (!value.empty()) return STG_E_WRITEFAULT;
        }
        // Also check the fresh enumeration for readers whose GetValue omits
        // custom fields. Accept an explicit empty field as a successful delete.
        for (const auto& field : saved->Metadata())
            if (!field.value.empty() && IsReplayGainTrackField(field.name.c_str()))
                return STG_E_WRITEFAULT;
        return S_OK;
    } catch (const std::system_error& error) {
        return HRESULT_FROM_WIN32(error.code().value());
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
}

} // namespace ttplayer::ui::detail
