#include "track_metadata.h"
#include "ttplayer/core/text.h"

#include <algorithm>
#include <charconv>
#include <climits>

namespace ttplayer::ui::detail {

TrackInfoState ApplyFileInfoProbeToTrack(
    playlist::Track& track, const std::optional<FileInfoProbeReadResult>& probe,
    FileInfoProbeProcessState process) {
    if (!probe)
        return process == FileInfoProbeProcessState::launch_failed ||
               process == FileInfoProbeProcessState::cancelled
            ? TrackInfoState::unavailable : TrackInfoState::failed;
    if (probe->status == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ||
        probe->status == HRESULT(0x8004A001L) ||
        probe->status == HRESULT(0x8004A002L) ||
        probe->status == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return TrackInfoState::unavailable;
    if (FAILED(probe->status)) return TrackInfoState::failed;

    const auto utf8 = [](std::wstring_view value) {
        try { return core::WideToUtf8(value); }
        catch (const std::exception&) { return std::string{}; }
    };
    const auto field = [&](const wchar_t* name) {
        const auto found = std::find_if(probe->metadata.begin(), probe->metadata.end(),
            [name](const auto& entry) { return _wcsicmp(entry.name.c_str(), name) == 0; });
        return found == probe->metadata.end() ? std::string{} : utf8(found->value);
    };
    track.title = field(L"Title");
    track.artist = field(L"Artist");
    if (track.artist.empty()) track.artist = field(L"Author");
    track.album = field(L"Album");
    auto number = field(L"Tracknumber");
    if (number.empty()) number = field(L"Track");
    int parsed{};
    const auto conversion = std::from_chars(number.data(), number.data() + number.size(), parsed);
    track.track_number = conversion.ec == std::errc{} && parsed > 0 ? parsed : 0;
    track.duration_ms = probe->status == S_FALSE ? -2 :
        static_cast<int>(std::min<DWORD>(probe->duration_ms, INT_MAX));
    track.media_type = utf8(probe->codec);
    const auto encoded = probe->encoded_bits_per_second ? probe->encoded_bits_per_second :
        static_cast<std::uint64_t>(probe->format.nAvgBytesPerSec) * 8U;
    track.bitrate_bps = static_cast<std::uint32_t>(std::min<std::uint64_t>(encoded, 0x7fffffffU));
    track.sample_rate_hz = probe->format.nSamplesPerSec;
    track.channels = probe->format.nChannels;
    track.bits_per_sample = probe->format.wBitsPerSample;
    const auto previous = track.metadata;
    track.metadata.clear();
    track.metadata.reserve(probe->metadata.size());
    for (const auto& entry : probe->metadata)
        if (!entry.name.empty()) track.metadata.emplace_back(utf8(entry.name), utf8(entry.value));
    PreserveBrowserTrackIdentity(previous, track.metadata);
    PreserveOsuTrackMetadata(previous, track.metadata);
    RestoreOsuTrackMetadata(track);
    return probe->status == S_FALSE ? TrackInfoState::provisional : TrackInfoState::success;
}

} // namespace ttplayer::ui::detail
