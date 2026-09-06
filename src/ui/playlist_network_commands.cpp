#include "ttplayer/ui/playlist_network_commands.h"

#include "ttplayer/core/text.h"

#include <algorithm>
#include <cwctype>
#include <exception>

namespace ttplayer::ui {
namespace {

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            if (character >= 'A' && character <= 'Z')
                return static_cast<char>(character - 'A' + 'a');
            return static_cast<char>(character);
        });
    return value;
}

std::wstring Wide(std::string_view value) {
    if (value.empty()) return {};
    try {
        return core::Utf8ToWide(value);
    } catch (const std::exception&) {
        return {};
    }
}

} // namespace

bool IsLegacyNetworkTrack(const playlist::Track& track) {
    // FUN_0041B6EA searches the stored source for "://" and requires the
    // marker not to be at offset zero.
    const auto source = track.path.wstring();
    const auto marker = source.find(L"://");
    return marker != std::wstring::npos && marker > 0;
}

bool SupportsLegacyFreeDbQuery(const playlist::Track& track) {
    // FUN_00483D4E admits only FUN_0041B669's CD Audio/CDA result or
    // FUN_00411CAE's CUE-source result before entering FUN_0043840A.
    const auto extension = Lower(track.path.extension().wstring());
    if (extension == L".cda" || extension == L".cue") return true;
    const auto media_type = LowerAscii(track.media_type);
    return media_type == "cd|cd audio" || media_type == "cda";
}

bool LegacyNetworkActionAcceptsTrack(
    LegacyPlaylistNetworkAction action, const playlist::Track& track) {
    switch (action) {
    case LegacyPlaylistNetworkAction::freedb:
        return SupportsLegacyFreeDbQuery(track);
    case LegacyPlaylistNetworkAction::report:
        // FUN_004878A4 explicitly exits unless FUN_0041B6EA sees "://".
        return IsLegacyNetworkTrack(track);
    case LegacyPlaylistNetworkAction::download:
    default:
        // FUN_004853A5 has no URL guard.  Its normal single-local-item menu
        // entry is removed by FUN_00488FEF, but a command invocation still
        // reaches the original missing-Music-Window message branch.
        return true;
    }
}

LegacyBackendNotice LegacyBackendUnavailableNotice(
    LegacyPlaylistNetworkAction action) noexcept {
    switch (action) {
    case LegacyPlaylistNetworkAction::freedb:
        // Original FreeDB server-read failure.
        return {0x81f4, 0x8165};
    case LegacyPlaylistNetworkAction::report:
        // Report title plus FUN_004853A5's optional Music Window failure.
        return {0x84d1, 0x81f5};
    case LegacyPlaylistNetworkAction::download:
    default:
        // FUN_004853A5, LAB_00485483.
        return {0x81f4, 0x81f5};
    }
}

std::wstring BuildLegacyReportTrackText(
    const playlist::Track& track, std::wstring_view heading) {
    auto title = Wide(track.title);
    if (title.empty()) title = track.path.stem().wstring();
    // TTPlayer.exe:0051F178 is the literal "%s： %s - %s";
    // FUN_0047DD08 supplies resource 0x84d1, Artist and Title, then limits
    // the complete result to 30 UTF-16 code units.
    std::wstring result(heading);
    result += L"： ";
    result += Wide(track.artist);
    result += L" - ";
    result += title;
    if (result.size() > 30) {
        result.resize(30);
        result += L"....";
    }
    return result;
}

} // namespace ttplayer::ui
