#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "modern_file_dialog.h"

#include "ttplayer/core/text.h"
#include "ttplayer/ui/dialog_history_policy.h"
#include "ttplayer/ui/playlist_catalog_reorder.h"
#include "ttplayer/ui/playlist_drag_completion.h"
#include "ttplayer/ui/playlist_list_contract.h"
#include "ttplayer/ui/playlist_network_commands.h"
#include "ttplayer/ui/playlist_transforms.h"
#include "ttplayer/ui/window_drag.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <commctrl.h>
#include <prsht.h>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <objidl.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <windowsx.h>

namespace ttplayer::ui {
using namespace detail;

namespace {
constexpr int kPlaylistTargetListControl = 1064;
constexpr int kPlaylistRenamePatternControl = 2161;
constexpr int kPlaylistUrlControl = 2001;

std::wstring FormatPlaylistResource(std::wstring_view format,
                                    const wchar_t* argument);
bool LooksLikeUrl(std::wstring_view value);

struct FormattedPlaylistTitle {
    std::wstring text;
    // Only the native short token "%R" participates in click-to-rate.
    // The long metadata spelling is rendered, but 0047ECA4 does not use it
    // for hit testing.
    std::optional<std::pair<size_t, size_t>> rating_span;
    bool valid{};
};

struct PlaylistFormatFieldValue {
    std::wstring rendered;
    size_t actual_length{};
    bool recognized{};
    bool rating{};
};

std::wstring PlaylistUtf8Field(std::string_view value) {
    if (value.empty()) return {};
    try {
        return core::Utf8ToWide(value);
    } catch (const std::exception&) {
        return {};
    }
}

std::wstring PlaylistMetadataValue(const playlist::Track& track,
                                   std::string_view name) {
    const auto equals = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            const auto lhs = static_cast<unsigned char>(left[index]);
            const auto rhs = static_cast<unsigned char>(right[index]);
            if (std::tolower(lhs) != std::tolower(rhs)) return false;
        }
        return true;
    };
    const auto found = std::find_if(track.metadata.begin(),
        track.metadata.end(), [name, &equals](const auto& entry) {
            return equals(entry.first, name);
        });
    return found == track.metadata.end() ? std::wstring{} :
        PlaylistUtf8Field(found->second);
}

// 00485C92 and 0048713E do not reuse the lyric currently displayed by the
// player for every selected row.  Each CPlayItem is looked up independently
// in the lyric association/search state (the same state filled by
// CLyric::_SearchLocalLyricFileProc at 00486B78).  The rebuild does not yet
// persist that private map, so reproduce its deterministic local-file side:
// exact adjacent names, enabled lyric-search folders, and the one explicit
// association that belongs to the currently playing item.  This helper is
// deliberately read-only and never returns a directory or URL.
std::vector<std::filesystem::path> FindPlaylistAssociatedLyrics(
    const playlist::Track& track,
    const ttplayer::settings::LyricSettings& lyric_settings,
    const std::filesystem::path& explicit_association = {}) {
    std::vector<std::filesystem::path> result;
    std::set<std::wstring> unique;

    const auto append_existing = [&](const std::filesystem::path& path) {
        if (path.empty() || LooksLikeUrl(path.wstring())) return;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return;

        std::error_code error;
        auto absolute = std::filesystem::absolute(path, error);
        if (error) absolute = path;
        auto key = absolute.lexically_normal().wstring();
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        if (unique.insert(std::move(key)).second) result.push_back(path);
    };

    if (track.path.empty() || LooksLikeUrl(track.path.wstring())) return result;

    const DWORD source_attributes = GetFileAttributesW(track.path.c_str());
    if (source_attributes == INVALID_FILE_ATTRIBUTES ||
        (source_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return result;

    // An explicitly associated lyric may have a nonstandard file name, so it
    // must be considered before the name-based search.  Callers pass it only
    // when this Track is the playing item to avoid borrowing another row's
    // association.  Requiring a live source prevents a stale playlist entry
    // from deleting or copying an otherwise unrelated lyric by itself.
    append_existing(explicit_association);

    std::wstring stem;
    try {
        stem = track.path.stem().wstring();
    } catch (const std::exception&) {
        return result;
    }
    if (stem.empty()) return result;

    std::wstring artist = PlaylistUtf8Field(track.artist);
    std::wstring title = PlaylistUtf8Field(track.title);
    if (artist.empty()) artist = PlaylistMetadataValue(track, "Artist");
    if (title.empty()) title = PlaylistMetadataValue(track, "Title");

    std::vector<std::filesystem::path> roots;
    const auto append_root = [&roots](const std::filesystem::path& root) {
        if (root.empty()) return;
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) return;
        const auto normalized = root.lexically_normal();
        const auto duplicate = std::find_if(roots.begin(), roots.end(),
            [&normalized](const auto& existing) {
                return _wcsicmp(existing.c_str(), normalized.c_str()) == 0;
            });
        if (duplicate == roots.end()) roots.push_back(normalized);
    };

    // LoadCurrentLyrics already recognizes these two locations.  Keep them
    // as fallbacks for profiles created before the Folders_* settings were
    // reconstructed.
    append_root(track.path.parent_path());
    const auto runtime_lyrics = FindRuntimePath(L"Lyrics");
    append_root(runtime_lyrics);

    for (auto configured : lyric_settings.folders) {
        // The leading '*' is TTPlayer's persisted check-state marker.  An
        // unchecked search location must not make an unrelated lyric eligible
        // for physical deletion/copy.
        if (configured.empty() || configured.front() != L'*') continue;
        configured.erase(configured.begin());
        if (configured == L"<Sound Folder>") {
            append_root(track.path.parent_path());
        } else if (configured == L"<Lyrics Download Folder>") {
            if (!lyric_settings.download_folder.empty())
                append_root(lyric_settings.download_folder);
            else
                append_root(runtime_lyrics);
        } else if (!configured.empty()) {
            append_root(FindRuntimePath(std::filesystem::path(configured)));
        }
    }

    for (const auto& root : roots) {
        append_existing(root / (stem + L".lrc"));
        append_existing(root / (stem + L".txt"));
        // 00486B78 scores metadata-based names in non-sound search folders;
        // the existing loader's accepted local spelling is "Artist - Title".
        const bool sound_folder = _wcsicmp(
            root.c_str(), track.path.parent_path().lexically_normal().c_str()) == 0;
        if (!sound_folder &&
            !artist.empty() && !title.empty()) {
            append_existing(root / (artist + L" - " + title + L".lrc"));
        }
    }
    return result;
}

std::wstring PlaylistRatingText(int rating) {
    rating = std::clamp(rating, 0, 5);
    std::wstring value;
    value.reserve(5);
    value.append(static_cast<size_t>(rating), L'\x2605');
    value.append(static_cast<size_t>(5 - rating), L'\x2606');
    return value;
}

std::wstring PlaylistFileTitle(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    // 004C7D9A only treats backslash as a path separator.  Its :// test
    // suppresses extension stripping, so a normal HTTP URL remains intact.
    if (value.find(L"://") != std::wstring::npos) return value;
    return path.stem().wstring();
}

std::wstring PlaylistFileExtension(const std::filesystem::path& path) {
    std::wstring extension;
    const std::wstring value = path.wstring();
    if (value.find(L"://") == std::wstring::npos) {
        extension = path.extension().wstring();
    } else {
        const size_t query = value.find(L'?');
        const size_t end = query == std::wstring::npos ? value.size() : query;
        const size_t slash = value.rfind(L'/', end == 0 ? 0 : end - 1);
        const size_t dot = value.rfind(L'.', end == 0 ? 0 : end - 1);
        if (dot != std::wstring::npos &&
            (slash == std::wstring::npos || dot > slash)) {
            extension = value.substr(dot, end - dot);
        }
    }
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   towlower);
    return extension;
}

std::wstring PlaylistParentName(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    // 004C7F0E likewise searches only for backslashes.  Forward-slash URLs
    // therefore have no parent-directory field in the original formatter.
    if (value.find(L"://") != std::wstring::npos) return {};
    const size_t file_separator = value.rfind(L'\\');
    if (file_separator == std::wstring::npos) return {};
    if (file_separator == 0) return L"\\";
    const size_t parent_separator = value.rfind(L'\\', file_separator - 1);
    const size_t begin = parent_separator == std::wstring::npos
        ? 0 : parent_separator + 1;
    return value.substr(begin, file_separator - begin);
}

PlaylistFormatFieldValue PlaylistFormatField(
    const playlist::Track& track, wchar_t short_name,
    std::wstring_view long_name, std::wstring_view unknown_format) {
    const auto actual = [](std::wstring value) {
        const size_t actual_length = value.size();
        return PlaylistFormatFieldValue{
            std::move(value), actual_length, true, false};
    };
    const auto metadata = [unknown_format](std::wstring value,
                                           std::wstring_view key,
                                           std::wstring fallback = {}) {
        const size_t actual_length = value.size();
        if (value.empty()) {
            if (!fallback.empty()) {
                value = std::move(fallback);
            } else if (!unknown_format.empty()) {
                value = FormatPlaylistResource(unknown_format,
                                                std::wstring(key).c_str());
            }
        }
        return PlaylistFormatFieldValue{
            std::move(value), actual_length, true, false};
    };

    // %(...) is not an alias for the short-token table.  004AE198 passes its
    // contents straight to the case-insensitive metadata lookup.  Track only
    // retains the metadata fields which the reconstructed reader/import path
    // currently exposes; every other key follows the native unknown-value
    // rendering and does not make the format successful.
    if (!long_name.empty()) {
        std::string key;
        try {
            key = core::WideToUtf8(long_name);
        } catch (const std::exception&) {
        }
        return metadata(PlaylistMetadataValue(track, key), long_name);
    }

    if (short_name == L'F')
        return actual(PlaylistFileTitle(track.path));
    if (short_name == L'E') {
        return actual(PlaylistFileExtension(track.path));
    }
    if (short_name == L'P') {
        // 004C7F0E returns the final directory component, not the complete
        // parent path despite the historical field name "filepath".
        return actual(PlaylistParentName(track.path));
    }
    if (short_name == L'C') {
        auto codec = PlaylistUtf8Field(track.media_type);
        if (const size_t separator = codec.find(L'|');
            separator != std::wstring::npos) codec.resize(separator);
        return metadata(std::move(codec), L"codec", L"N/A");
    }
    if (short_name == L'B') {
        const std::uint32_t value = track.bitrate_bps & 0x7fffffffU;
        if (value == 0) return {L"N/A", 0, true, false};
        if (value <= 1000000) {
            return actual(std::to_wstring(value / 1000) + L"K");
        }
        wchar_t rendered[32]{};
        swprintf_s(rendered, L"%.2fM",
            static_cast<double>(value) / 1000000.0);
        return actual(rendered);
    }
    if (short_name == L'R') {
        return {PlaylistRatingText(track.rating), 5, true, true};
    }
    if (short_name == L'A')
        return metadata(PlaylistMetadataValue(track, "Artist"), L"Artist");
    if (short_name == L'T')
        return metadata(PlaylistMetadataValue(track, "Title"), L"Title");
    if (short_name == L'L')
        return metadata(PlaylistMetadataValue(track, "Album"), L"Album");
    if (short_name == L'I') {
        auto number = PlaylistMetadataValue(track, "Tracknumber");
        return metadata(std::move(number), L"Tracknumber", L"0");
    }
    if (short_name == L'G')
        return metadata(PlaylistMetadataValue(track, "Genre"), L"Genre");
    if (short_name == L'Y')
        return metadata(PlaylistMetadataValue(track, "Date"), L"Date");
    if (short_name == L'D')
        return metadata(PlaylistMetadataValue(track, "Comment"), L"Comment");
    // Unknown short tokens are copied without the '%' and contribute no
    // actual metadata length.  A different real field can still make the
    // complete pattern acceptable.
    if (short_name != L'\0')
        return {std::wstring(1, short_name), 0, true, false};
    return {};
}

FormattedPlaylistTitle FormatPlaylistPattern(const playlist::Track& track,
                                              std::wstring_view pattern,
                                              std::wstring_view unknown_format) {
    FormattedPlaylistTitle result;
    if (pattern.empty()) return result;
    size_t actual_length{};
    size_t last_rating_length{};
    for (size_t offset = 0; offset < pattern.size();) {
        const size_t marker = pattern.find(L'%', offset);
        if (marker == std::wstring_view::npos) {
            result.text.append(pattern.substr(offset));
            break;
        }
        result.text.append(pattern.substr(offset, marker - offset));
        if (marker + 1 >= pattern.size()) return {};

        const wchar_t token = pattern[marker + 1];
        std::wstring_view long_name;
        size_t next = marker + 2;
        if (token == L'(') {
            const size_t close = pattern.find(L')', next);
            if (close == std::wstring_view::npos || close == next) return {};
            long_name = pattern.substr(next, close - next);
            next = close + 1;
        }
        const auto field = PlaylistFormatField(
            track, token == L'(' ? L'\0' : token, long_name,
            unknown_format);
        if (!field.recognized) return {};
        const size_t field_start = result.text.size();
        result.text += field.rendered;
        actual_length += field.actual_length;
        if (field.rating) last_rating_length = field.actual_length;
        if (token == L'R' && !result.rating_span)
            result.rating_span = std::pair{field_start, result.text.size()};
        offset = next;
    }
    // FUN_004AE198 rejects a format for which no field produced data.  This
    // is what lets the caller fall back from TagTitleFormat to DefTitleFormat.
    // The native parser subtracts only the most recent %R contribution from
    // its validity count.  That odd detail means a lone rating field fails,
    // while two %R fields technically make the format valid.
    result.valid = actual_length != last_rating_length;
    if (!result.valid) result = {};
    return result;
}

FormattedPlaylistTitle FormatPlaylistTitle(
    const playlist::Track& track,
    const settings::PlaylistSettings& settings,
    std::wstring_view unknown_format) {
    if (settings.tag_format != 0) {
        auto formatted = FormatPlaylistPattern(track,
            settings.tag_title_format, unknown_format);
        if (formatted.valid) return formatted;
    }
    auto formatted = FormatPlaylistPattern(track,
        settings.default_title_format, unknown_format);
    if (formatted.valid) return formatted;
    // 004AE7FD's final fallback is always filetitle (%F), even when a tag
    // Title exists on the item.
    formatted.text = PlaylistFileTitle(track.path);
    formatted.valid = true;
    return formatted;
}

HFONT CreatePlaylistFont(const settings::PlaylistSettings& settings) {
    NONCLIENTMETRICSW nonclient{sizeof(nonclient)};
    LOGFONTW descriptor{};
    if (settings.font_descriptor_valid) {
        descriptor = settings.font_descriptor;
    } else {
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 0, &nonclient, 0))
            descriptor = nonclient.lfMessageFont;
        descriptor.lfHeight = settings.font_height;
        descriptor.lfWeight = FW_NORMAL;
        descriptor.lfQuality = ANTIALIASED_QUALITY;
        wcsncpy_s(descriptor.lfFaceName, settings.font.c_str(), _TRUNCATE);
    }
    return CreateFontIndirectW(&descriptor);
}

int PlaylistNumberColumnWidth(HDC dc, size_t item_count, bool visible) {
    if (!visible || !dc) return 0;
    // 0047ECA4 and the cached DAT_00541314 measure one value for the whole
    // column, using item-count + 1 rather than each row's current index.
    const auto sample = std::to_wstring(item_count + 1) + L".";
    RECT extent{};
    DrawTextW(dc, sample.c_str(), static_cast<int>(sample.size()), &extent,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    return std::max(0L, extent.right - extent.left);
}

int PlaylistTextWidth(HDC dc, std::wstring_view text) {
    if (!dc || text.empty()) return 0;
    RECT extent{};
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &extent,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    return std::max(0L, extent.right - extent.left);
}

std::wstring PlaylistDurationText(const playlist::Track& track) {
    if (track.duration_ms < 0) return {};
    const int seconds = track.duration_ms / 1000;
    wchar_t duration[24]{};
    const int hours = seconds / 3600;
    if (hours > 0) {
        swprintf_s(duration, L"%d:%02d:%02d", hours,
            (seconds / 60) % 60, seconds % 60);
    } else {
        swprintf_s(duration, L"%d:%02d", seconds / 60, seconds % 60);
    }
    return duration;
}

bool PlaylistWideEquals(std::wstring_view left,
                        std::wstring_view right) noexcept {
    return left.size() == right.size() &&
        _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

PlaylistScrollbarMetrics ResolvePlaylistScrollbarMetrics(
    const PlaylistGeometry& geometry, const skin::PlaylistSkin& layout,
    size_t item_count, size_t page_size, size_t position) noexcept {
    const int button_extent = std::max(
        1, static_cast<int>(layout.scrollbar_buttons.size.cy / 2));
    const int thumb_extent = std::max(
        1, static_cast<int>(layout.scrollbar_thumb.size.cy));
    return MakePlaylistScrollbarMetrics(
        geometry.scrollbar.top, geometry.scrollbar.bottom,
        button_extent, thumb_extent,
        layout.scrollbar_thumb_resize_center,
        item_count, page_size, position);
}

void DrawPlaylistScrollbarThumb(HDC target, const skin::SkinBitmap& bitmap,
                                const RECT& bounds, int frame,
                                int resize_center, bool tile_center) {
    if (!target || !bitmap.image || bitmap.size.cx <= 0 ||
        bitmap.size.cy <= 0 || bounds.bottom <= bounds.top) return;
    const int frame_width = std::max(
        1, static_cast<int>(bitmap.size.cx / 3));
    const int destination_width = std::min(
        frame_width, std::max(0, static_cast<int>(bounds.right - bounds.left)));
    if (destination_width <= 0) return;
    frame = std::clamp(frame, 0, 2);
    const int source_x = frame * frame_width;
    const int destination_height = static_cast<int>(bounds.bottom - bounds.top);
    const int source_height = static_cast<int>(bitmap.size.cy);
    const int center = std::clamp(resize_center, 0, source_height);
    if (center <= 0 || destination_height < source_height) {
        const int height = std::min(destination_height, source_height);
        bitmap.image.Draw(target, bounds.left, bounds.top, destination_width,
                          height, source_x, 0, destination_width, height);
    } else {
        // 00488463 splits thumb_image around thumb_resize_center.  The two
        // caps remain pixel-exact; only the center is stretched or tiled.
        const int top_cap = (source_height - center) / 2;
        const int bottom_cap = source_height - top_cap - center;
        const int destination_center = std::max(
            0, destination_height - top_cap - bottom_cap);
        if (top_cap > 0) {
            bitmap.image.Draw(target, bounds.left, bounds.top, destination_width,
                              top_cap, source_x, 0, destination_width, top_cap);
        }
        const int center_top = bounds.top + top_cap;
        if (destination_center > 0) {
            if (tile_center) {
                for (int written = 0; written < destination_center;) {
                    const int chunk = std::min(center,
                        destination_center - written);
                    bitmap.image.Draw(target, bounds.left, center_top + written,
                        destination_width, chunk, source_x, top_cap,
                        destination_width, chunk);
                    written += chunk;
                }
            } else {
                bitmap.image.Draw(target, bounds.left, center_top,
                    destination_width, destination_center, source_x, top_cap,
                    frame_width, center);
            }
        }
        if (bottom_cap > 0) {
            bitmap.image.Draw(target, bounds.left, bounds.bottom - bottom_cap,
                destination_width, bottom_cap, source_x, top_cap + center,
                destination_width, bottom_cap);
        }
    }
}

bool PlaylistWideContains(std::wstring_view value,
                          std::wstring_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > value.size()) return false;
    const auto match = std::search(value.begin(), value.end(),
        needle.begin(), needle.end(), [](wchar_t left, wchar_t right) {
            return towlower(left) == towlower(right);
        });
    return match != value.end();
}

std::wstring PlaylistInfoValue(const playlist::Track& track,
                               std::wstring_view name,
                               std::wstring_view network_fallback) {
    // 004AEB8F recognizes these three built-ins with a case-sensitive
    // compare; all other names go through the metadata bag's insensitive
    // lookup.
    if (name == L"Filename") {
        const auto value = track.path.wstring();
        return value.empty() && LooksLikeUrl(value)
            ? std::wstring(network_fallback) : value;
    }
    if (name == L"Duration")
        return PlaylistDurationText(track);
    if (name == L"Format") {
        auto format = PlaylistUtf8Field(track.media_type);
        if (const size_t separator = format.find(L'|');
            separator != std::wstring::npos) {
            format.resize(separator);
        }
        const std::uint32_t bitrate = track.bitrate_bps & 0x7fffffffU;
        if (!format.empty() && bitrate != 0 && track.sample_rate_hz != 0) {
            std::wstring rate;
            if (bitrate <= 1000000U)
                rate = std::to_wstring(bitrate / 1000U) + L"K";
            else {
                wchar_t rendered[32]{};
                swprintf_s(rendered, L"%.2fM",
                    static_cast<double>(bitrate) / 1000000.0);
                rate = rendered;
            }
            if ((track.bitrate_bps & 0x80000000U) != 0) rate += L"(VBR)";
            format += L" " + std::to_wstring(track.sample_rate_hz / 1000U) +
                      L"kHz " + rate;
        }
        return format;
    }

    std::string key;
    try { key = core::WideToUtf8(name); }
    catch (const std::exception&) { return {}; }
    // 004AEB8F resolves non-built-in placeholders from the metadata bag.
    // CPlayItem's cached display title is not a Title tag: falling back to it
    // invents a title row for untagged files (including imported list labels).
    return PlaylistMetadataValue(track, key);
}

std::wstring FormatPlaylistItemTip(const playlist::Track& track,
                                   std::wstring_view pattern,
                                   std::wstring_view network_fallback,
                                   std::wstring_view missing_warning) {
    std::wstring result;
    size_t line_begin{};
    while (line_begin <= pattern.size()) {
        const size_t line_end = pattern.find(L'|', line_begin);
        std::wstring line(pattern.substr(line_begin,
            line_end == std::wstring_view::npos
                ? pattern.size() - line_begin : line_end - line_begin));
        bool populated = true;
        size_t search{};
        while (true) {
            const size_t open = line.find(L"%(", search);
            if (open == std::wstring::npos) break;
            const size_t close = line.find(L')', open + 2);
            if (close == std::wstring::npos || close == open + 2) {
                populated = false;
                break;
            }
            const auto value = PlaylistInfoValue(track,
                std::wstring_view(line).substr(open + 2, close - open - 2),
                network_fallback);
            if (value.empty()) {
                populated = false;
                break;
            }
            line.replace(open, close - open + 1, value);
            search = open + value.size();
        }
        if (populated && !line.empty()) {
            if (!result.empty()) result.push_back(L'\n');
            result += line;
        }
        if (line_end == std::wstring_view::npos) break;
        line_begin = line_end + 1;
    }
    // 004887AA appends resource 0x828e only for a non-network,
    // non-ZIP/RAR logical path whose ordinary file does not exist.  The
    // warning is appended to the already formatted info-tip text; the
    // resource deliberately supplies its own human-readable separator.
    if (!result.empty() && !LooksLikeUrl(track.path.wstring())) {
        auto lower = track.path.wstring();
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        const size_t zip = lower.find(L".zip|");
        const size_t rar = lower.find(L".rar|");
        const bool archive = (zip != std::wstring::npos && zip > 0) ||
                             (rar != std::wstring::npos && rar > 0);
        if (!archive) {
            const DWORD attributes = GetFileAttributesW(track.path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (!missing_warning.empty()) {
                    result += L"\r\n(";
                    result += missing_warning;
                    result.push_back(L')');
                }
            }
        }
    }
    return result;
}

std::optional<RECT> PlaylistRatingRect(
    HDC dc, const RECT& row, size_t item_count,
    const playlist::Track& track,
    const settings::PlaylistSettings& settings,
    std::wstring_view unknown_format) {
    if (!dc || !settings.click_rating) return std::nullopt;
    const auto title = FormatPlaylistTitle(track, settings, unknown_format);
    if (!title.rating_span || title.rating_span->second <=
        title.rating_span->first) return std::nullopt;

    const int number_width = PlaylistNumberColumnWidth(
        dc, item_count, settings.title_number);
    const auto duration = PlaylistDurationText(track);
    const int duration_width = PlaylistTextWidth(dc, duration);
    const LONG title_left = row.left + 6 + number_width;
    const LONG title_right = std::max<LONG>(title_left,
        row.right - 4 - duration_width);

    const auto [rating_begin, rating_end] = *title.rating_span;
    const int prefix_width = PlaylistTextWidth(
        dc, std::wstring_view(title.text).substr(0, rating_begin));
    const int rating_width = PlaylistTextWidth(
        dc, std::wstring_view(title.text).substr(
            rating_begin, rating_end - rating_begin));
    RECT bounds{title_left + prefix_width, row.top,
                title_left + prefix_width + rating_width, row.bottom};
    // 0047ECA4 accepts the star cell only when the end of %R has not been
    // clipped; a visible suffix is not required.
    if (bounds.right > title_right || bounds.right <= bounds.left)
        return std::nullopt;
    return bounds;
}

struct PlaylistTargetDialogState {
    const playlist::PlaylistStore* store{};
    size_t selected{playlist::PlaylistStore::npos};
};

INT_PTR CALLBACK PlaylistTargetDialogProc(HWND dialog, UINT message,
                                           WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PlaylistTargetDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<PlaylistTargetDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        if (!state || !state->store) return FALSE;
        const HWND list = GetDlgItem(dialog, kPlaylistTargetListControl);
        for (const auto& entry : state->store->Entries()) {
            SendMessageW(list, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(entry.playlist.Title().c_str()));
        }
        if (state->store->Size() != 0)
            SendMessageW(list, LB_SETCURSEL,
                         state->store->ActiveIndex(), 0);
        return TRUE;
    }
    if (message != WM_COMMAND || !state) return FALSE;
    switch (LOWORD(wparam)) {
    case IDOK: {
        const LRESULT selected = SendDlgItemMessageW(dialog,
            kPlaylistTargetListControl, LB_GETCURSEL, 0, 0);
        if (selected == LB_ERR) return TRUE;
        state->selected = static_cast<size_t>(selected);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    case kPlaylistNewList:
        state->selected = state->store->Size();
        EndDialog(dialog, IDOK);
        return TRUE;
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

struct PlaylistTextDialogState {
    int control{};
    std::wstring value;
};

INT_PTR CALLBACK PlaylistTextDialogProc(HWND dialog, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PlaylistTextDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<PlaylistTextDialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        if (!state) return FALSE;
        SetDlgItemTextW(dialog, state->control, state->value.c_str());
        const HWND edit = GetDlgItem(dialog, state->control);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        return FALSE;
    }
    if (message != WM_COMMAND || !state) return FALSE;
    switch (LOWORD(wparam)) {
    case IDOK: {
        const int length = GetWindowTextLengthW(GetDlgItem(dialog,
            state->control));
        std::wstring value(static_cast<size_t>(std::max(0, length)) + 1,
                           L'\0');
        GetDlgItemTextW(dialog, state->control, value.data(),
                        static_cast<int>(value.size()));
        value.resize(wcslen(value.c_str()));
        if (value.empty()) return TRUE;
        state->value = std::move(value);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    case 1023:
        // The URL dialog's browse button closes with the private 0x3ff
        // result; CPlayerWnd then enters the same multi-file picker as Add.
        EndDialog(dialog, 0x3ff);
        return TRUE;
    }
    return FALSE;
}

bool LooksLikeUrl(std::wstring_view value) {
    const auto separator = value.find(L"://");
    if (separator == std::wstring_view::npos || separator == 0) return false;
    return std::all_of(value.begin(), value.begin() + separator,
        [](wchar_t character) {
            return iswalnum(character) || character == L'+' ||
                   character == L'-' || character == L'.';
        });
}

std::wstring PlaylistTrackIdentity(const playlist::Track& track) {
    std::wstring value = track.path.wstring();
    // The global CPlayItem pool uses the logical filename verbatim with a
    // case-insensitive comparison; it does not absolute/lexically normalize.
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    value.push_back(L'\x1f');
    value += std::to_wstring(track.subtrack);
    return value;
}

// DoDragDrop runs a nested message loop.  The active catalogue row and the
// ListView selection are therefore not reliable source identifiers when it
// returns: automation, another window, or a shell callback can change either
// while the drag is in flight.  The native player keeps CPlayItem pointers in
// its private format.  The rebuild cannot expose those process-private
// objects, so retain the equivalent persistent playlist slot plus the
// case-insensitive path/subtrack identity and its occurrence number.
struct PlaylistDragTrackIdentity {
    std::wstring value;
    size_t occurrence{};
};

std::vector<PlaylistDragTrackIdentity> CapturePlaylistDragTrackIdentities(
    const playlist::Playlist& source, const std::set<size_t>& selected) {
    std::vector<PlaylistDragTrackIdentity> result;
    result.reserve(selected.size());
    std::map<std::wstring, size_t> occurrences;
    const auto& tracks = source.Tracks();
    for (size_t row = 0; row < tracks.size(); ++row) {
        auto identity = PlaylistTrackIdentity(tracks[row]);
        const size_t occurrence = occurrences[identity]++;
        if (selected.contains(row))
            result.push_back({std::move(identity), occurrence});
    }
    return result;
}

std::set<size_t> ResolvePlaylistDragTrackRows(
    const playlist::Playlist& source,
    const std::vector<PlaylistDragTrackIdentity>& identities) {
    std::map<std::wstring, std::vector<size_t>> rows;
    const auto& tracks = source.Tracks();
    for (size_t row = 0; row < tracks.size(); ++row)
        rows[PlaylistTrackIdentity(tracks[row])].push_back(row);

    std::set<size_t> result;
    for (const auto& identity : identities) {
        const auto found = rows.find(identity.value);
        if (found != rows.end() && identity.occurrence < found->second.size())
            result.insert(found->second[identity.occurrence]);
    }
    return result;
}

std::optional<size_t> RemapPlaylistRowAfterRemoval(
    std::optional<size_t> row, const std::set<size_t>& removed) {
    if (!row) return std::nullopt;
    const auto position = removed.lower_bound(*row);
    if (position != removed.end() && *position == *row) return std::nullopt;
    return *row - static_cast<size_t>(std::distance(removed.begin(), position));
}

std::set<size_t> RemapPlaylistRowsAfterRemoval(
    const std::set<size_t>& rows, const std::set<size_t>& removed) {
    std::set<size_t> result;
    for (const size_t row : rows) {
        if (const auto remapped = RemapPlaylistRowAfterRemoval(row, removed))
            result.insert(*remapped);
    }
    return result;
}

bool PlaylistTrackSourceValid(const playlist::Track& track) {
    if (track.path.empty()) return false;
    // CPlayItem +0x10 distinguishes a failed reader (-1) from an item whose
    // information has not been read yet (-2).
    if (track.duration_ms == -1) return false;
    // FUN_0041B6EA is intentionally only a lexical Find("://") > 0 test.
    // Keep this historical leniency local to 00485A5D's invalid-item cleanup;
    // Shell/OLE and network dispatch still require a sane URI scheme.
    const auto source = track.path.wstring();
    const auto url_marker = source.find(L"://");
    if (url_marker != std::wstring::npos && url_marker > 0) return true;
    auto extension = track.path.extension().wstring();
    std::transform(extension.begin(), extension.end(),
                   extension.begin(), towlower);
    auto source_type = PlaylistUtf8Field(track.media_type);
    if (_wcsicmp(source_type.c_str(), L"CD|CD Audio") == 0 ||
        extension == L".cda") return true;
    auto lower = track.path.wstring();
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    const size_t zip = lower.find(L".zip|");
    const size_t rar = lower.find(L".rar|");
    if ((zip != std::wstring::npos && zip >= 1) ||
        (rar != std::wstring::npos && rar >= 1)) return true;
    const DWORD attributes = GetFileAttributesW(track.path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

UINT PlaylistDropItemsFormat() {
    static const UINT format =
        RegisterClipboardFormatW(L"TTPlayer_DropItemsFormat");
    return format;
}

UINT UniformResourceLocatorWideFormat() {
    static const UINT format =
        RegisterClipboardFormatW(L"UniformResourceLocatorW");
    return format;
}

UINT ShellIdListArrayFormat() {
    static const UINT format = RegisterClipboardFormatW(L"Shell IDList Array");
    return format;
}

HGLOBAL MakePlaylistDragGlobal(const void* bytes, size_t size) {
    if (!bytes || size == 0) return nullptr;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (!memory) return nullptr;
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return nullptr;
    }
    memcpy(destination, bytes, size);
    GlobalUnlock(memory);
    return memory;
}

HGLOBAL MakePlaylistDragFileList(
    const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return nullptr;
    size_t characters = 1;
    for (const auto& path : paths) characters += path.wstring().size() + 1;
    const size_t bytes = sizeof(DROPFILES) + characters * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) return nullptr;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!drop) {
        GlobalFree(memory);
        return nullptr;
    }
    drop->pFiles = sizeof(DROPFILES);
    GetCursorPos(&drop->pt);
    drop->fNC = TRUE;
    drop->fWide = TRUE;
    auto* cursor = reinterpret_cast<wchar_t*>(
        reinterpret_cast<unsigned char*>(drop) + sizeof(DROPFILES));
    for (const auto& path : paths) {
        const auto value = path.wstring();
        memcpy(cursor, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
        cursor += value.size() + 1;
    }
    *cursor = L'\0';
    GlobalUnlock(memory);
    return memory;
}

HGLOBAL MakePlaylistShellIdListArray(
    const std::vector<std::wstring>& paths);

class PlaylistDragDataObject final : public IDataObject {
public:
    struct Entry {
        FORMATETC format{};
        HGLOBAL memory{};
    };

    PlaylistDragDataObject(std::vector<std::filesystem::path> local_paths,
                           std::wstring first_url) {
        const DWORD process = GetCurrentProcessId();
        Add(static_cast<CLIPFORMAT>(PlaylistDropItemsFormat()),
            MakePlaylistDragGlobal(&process, sizeof(process)));
        std::vector<std::wstring> shell_paths;
        shell_paths.reserve(local_paths.size());
        for (const auto& path : local_paths)
            shell_paths.push_back(path.wstring());
        if (HGLOBAL items = MakePlaylistShellIdListArray(shell_paths)) {
            Add(static_cast<CLIPFORMAT>(ShellIdListArrayFormat()), items);
        }
        if (HGLOBAL files = MakePlaylistDragFileList(local_paths))
            Add(CF_HDROP, files);
        if (!first_url.empty()) {
            Add(static_cast<CLIPFORMAT>(UniformResourceLocatorWideFormat()),
                MakePlaylistDragGlobal(first_url.c_str(),
                    (first_url.size() + 1) * sizeof(wchar_t)));
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                               void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *result = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* request,
                                      STGMEDIUM* medium) override {
        if (!request || !medium) return E_POINTER;
        const auto entry = Find(*request);
        if (entry == entries_.end()) return DV_E_FORMATETC;
        HGLOBAL duplicate = static_cast<HGLOBAL>(OleDuplicateData(
            entry->memory, entry->format.cfFormat, GMEM_MOVEABLE));
        if (!duplicate) return E_OUTOFMEMORY;
        ZeroMemory(medium, sizeof(*medium));
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = duplicate;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* request) override {
        if (!request) return E_POINTER;
        return Find(*request) == entries_.end() ? DV_E_FORMATETC : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,
                                                     FORMATETC* output) override {
        if (!output) return E_POINTER;
        output->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction,
                                             IEnumFORMATETC** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> formats;
        formats.reserve(entries_.size());
        for (const auto& entry : entries_) formats.push_back(entry.format);
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()),
                                    formats.data(), result);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*,
                                       DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~PlaylistDragDataObject() {
        for (auto& entry : entries_) {
            if (entry.memory) GlobalFree(entry.memory);
        }
    }
    void Add(CLIPFORMAT format, HGLOBAL memory) noexcept {
        if (!memory) return;
        if (!format) {
            GlobalFree(memory);
            return;
        }
        try {
            entries_.push_back({{format, nullptr, DVASPECT_CONTENT, -1,
                                 TYMED_HGLOBAL}, memory});
        } catch (const std::exception&) {
            // IDataObject owns every HGLOBAL handed to Add, including when
            // registering a private clipboard format failed or growing the
            // format vector ran out of memory.
            GlobalFree(memory);
        }
    }
    std::vector<Entry>::const_iterator Find(const FORMATETC& request) const {
        return std::find_if(entries_.begin(), entries_.end(),
            [&request](const Entry& entry) {
                return request.cfFormat == entry.format.cfFormat &&
                    (request.tymed & TYMED_HGLOBAL) != 0 &&
                    request.dwAspect == DVASPECT_CONTENT &&
                    (request.lindex == -1 || request.lindex == 0);
            });
    }

    std::atomic<ULONG> references_{1};
    std::vector<Entry> entries_;
};

class PlaylistDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                               void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *result = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape,
                                                 DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        return (keys & MK_LBUTTON) == 0 ? DRAGDROP_S_DROP : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ~PlaylistDropSource() = default;
    std::atomic<ULONG> references_{1};
};

HGLOBAL MakePlaylistShellIdListArray(
    const std::vector<std::wstring>& paths) {
    struct PidlDeleter {
        void operator()(ITEMIDLIST* item) const noexcept {
            CoTaskMemFree(item);
        }
    };
    using Pidl = std::unique_ptr<ITEMIDLIST, PidlDeleter>;

    std::vector<Pidl> items;
    try {
        items.reserve(paths.size());
        for (const auto& path : paths) {
            PIDLIST_ABSOLUTE item{};
            const HRESULT parsed = SHParseDisplayName(
                path.c_str(), nullptr, &item, 0, nullptr);
            Pidl owned(item);
            if (SUCCEEDED(parsed) && owned) {
                items.push_back(std::move(owned));
            }
        }
    } catch (const std::exception&) {
        return nullptr;
    }
    if (items.empty() ||
        items.size() > static_cast<size_t>(
            (std::numeric_limits<UINT>::max)()) - 2) return nullptr;

    // CFSTR_SHELLIDLIST stores a desktop parent PIDL followed by each child
    // PIDL. Absolute PIDLs returned by SHParseDisplayName are valid children
    // of that desktop root and allow files from different directories.
    if (items.size() + 2 >
        (std::numeric_limits<size_t>::max)() / sizeof(UINT)) return nullptr;
    const size_t offsets_size = sizeof(UINT) * (items.size() + 2);
    if (offsets_size > (std::numeric_limits<UINT>::max)() - sizeof(USHORT))
        return nullptr;
    size_t bytes = offsets_size + sizeof(USHORT);
    for (const auto& item : items) {
        const size_t item_size = ILGetSize(item.get());
        if (item_size == 0 ||
            item_size > (std::numeric_limits<UINT>::max)() - bytes)
            return nullptr;
        bytes += item_size;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) return nullptr;
    auto* data = static_cast<unsigned char*>(GlobalLock(memory));
    if (!data) {
        GlobalFree(memory);
        return nullptr;
    }
    auto* values = reinterpret_cast<UINT*>(data);
    values[0] = static_cast<UINT>(items.size());
    values[1] = static_cast<UINT>(offsets_size);
    *reinterpret_cast<USHORT*>(data + offsets_size) = 0;
    size_t cursor = offsets_size + sizeof(USHORT);
    for (size_t index = 0; index < items.size(); ++index) {
        values[index + 2] = static_cast<UINT>(cursor);
        const size_t item_size = ILGetSize(items[index].get());
        memcpy(data + cursor, items[index].get(), item_size);
        cursor += item_size;
    }
    GlobalUnlock(memory);
    return memory;
}

bool PublishShellIdListArray(const std::vector<std::wstring>& paths) {
    HGLOBAL memory = MakePlaylistShellIdListArray(paths);
    if (!memory) return false;
    const UINT format = ShellIdListArrayFormat();
    if (format != 0 && SetClipboardData(format, memory)) return true;
    GlobalFree(memory);
    return false;
}

bool ClipboardHasCurrentPlaylistProcess(HWND owner) {
    const UINT format = PlaylistDropItemsFormat();
    if (format == 0 || !IsClipboardFormatAvailable(format) ||
        !OpenClipboard(owner)) return false;
    bool matches{};
    if (const HANDLE memory = GetClipboardData(format)) {
        if (GlobalSize(memory) >= sizeof(DWORD)) {
            if (const auto* process = static_cast<const DWORD*>(
                    GlobalLock(memory))) {
                matches = *process == GetCurrentProcessId();
                GlobalUnlock(memory);
            }
        }
    }
    CloseClipboard();
    return matches;
}

bool PublishPlaylistClipboard(HWND owner,
                              const std::vector<playlist::Track>& tracks) {
    if (tracks.empty() || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    bool published{};

    std::vector<std::wstring> local_paths;
    std::wstring first_url;
    for (const auto& track : tracks) {
        const auto path = track.path.wstring();
        if (path.empty()) continue;
        if (!LooksLikeUrl(path)) local_paths.push_back(path);
        else if (first_url.empty()) first_url = path;
    }

    // 00481871 publishes this private format as exactly one DWORD.  It is a
    // same-process capability marker; the actual CPlayItem snapshots remain
    // in PlayerWindow's temporary vector rather than being serialized.
    if (const UINT format = PlaylistDropItemsFormat(); format != 0) {
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
            if (auto* process = static_cast<DWORD*>(GlobalLock(memory))) {
                *process = GetCurrentProcessId();
                GlobalUnlock(memory);
                if (SetClipboardData(format, memory)) published = true;
                else GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
    }
    if (!local_paths.empty()) {
        size_t characters = 1;
        for (const auto& path : local_paths) characters += path.size() + 1;
        const size_t bytes = sizeof(DROPFILES) + characters * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                                         bytes)) {
            if (auto* drop = static_cast<DROPFILES*>(GlobalLock(memory))) {
                drop->pFiles = sizeof(DROPFILES);
                GetCursorPos(&drop->pt);
                drop->fNC = TRUE;
                drop->fWide = TRUE;
                auto* cursor = reinterpret_cast<wchar_t*>(
                    reinterpret_cast<unsigned char*>(drop) + sizeof(DROPFILES));
                for (const auto& path : local_paths) {
                    memcpy(cursor, path.c_str(),
                           (path.size() + 1) * sizeof(wchar_t));
                    cursor += path.size() + 1;
                }
                *cursor = L'\0';
                GlobalUnlock(memory);
                if (SetClipboardData(CF_HDROP, memory)) published = true;
                else GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
        // 00481871 also exposes Shell IDList Array for Explorer-compatible
        // consumers. Playlist paste still deliberately prefers CF_HDROP.
        published = PublishShellIdListArray(local_paths) || published;
    }
    if (!first_url.empty()) {
        const UINT format = UniformResourceLocatorWideFormat();
        const size_t bytes = (first_url.size() + 1) * sizeof(wchar_t);
        if (format != 0) {
            if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                if (void* data = GlobalLock(memory)) {
                    memcpy(data, first_url.c_str(), bytes);
                    GlobalUnlock(memory);
                    if (SetClipboardData(format, memory)) published = true;
                    else GlobalFree(memory);
                } else {
                    GlobalFree(memory);
                }
            }
        }
    }
    CloseClipboard();
    return published;
}

std::vector<std::filesystem::path> ReadPlaylistClipboard(HWND owner) {
    std::vector<std::filesystem::path> paths;
    if (!OpenClipboard(owner)) return paths;
    if (const HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
        const UINT count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
        paths.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(static_cast<size_t>(length) + 1, L'\0');
            if (DragQueryFileW(drop, index, path.data(), length + 1) != 0) {
                path.resize(length);
                paths.emplace_back(std::move(path));
            }
        }
    } else if (const UINT format = UniformResourceLocatorWideFormat();
               format != 0) {
        if (const HANDLE memory = GetClipboardData(format)) {
            if (const auto* text = static_cast<const wchar_t*>(
                    GlobalLock(memory))) {
                const size_t characters = GlobalSize(memory) / sizeof(wchar_t);
                const size_t length = wcsnlen_s(text, characters);
                if (length != 0 && length < characters)
                    paths.emplace_back(std::wstring_view(text, length));
                GlobalUnlock(memory);
            }
        }
    }
    CloseClipboard();
    return paths;
}

std::wstring SafePlaylistFileName(std::wstring value) {
    for (auto& character : value) {
        if (character < 0x20 || wcschr(L"<>:\"/\\|?*", character))
            character = L'_';
    }
    while (!value.empty() && (value.back() == L' ' || value.back() == L'.'))
        value.pop_back();
    return value;
}

std::wstring FormatPlaylistResource(std::wstring_view format,
                                    const wchar_t* argument) {
    if (format.empty()) return {};
    const std::wstring stable_format(format);
    const int length = _scwprintf(stable_format.c_str(), argument);
    if (length < 0) return stable_format;
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    swprintf_s(result.data(), result.size(), stable_format.c_str(), argument);
    result.resize(static_cast<size_t>(length));
    return result;
}

std::wstring FormatPlaylistFileName(const playlist::Track& track,
                                    std::wstring pattern) {
    const std::wstring title = track.title.empty()
        ? track.path.stem().wstring() : core::Utf8ToWide(track.title);
    const std::wstring artist = track.artist.empty()
        ? std::wstring{} : core::Utf8ToWide(track.artist);
    const std::wstring album = track.album.empty()
        ? std::wstring{} : core::Utf8ToWide(track.album);
    const std::wstring number = track.track_number > 0
        ? std::to_wstring(track.track_number) : std::wstring{};
    const auto equals = [](std::wstring_view left,
                           std::wstring_view right) noexcept {
        return left.size() == right.size() &&
            _wcsnicmp(left.data(), right.data(), left.size()) == 0;
    };
    const auto long_field = [&](std::wstring_view name) {
        if (equals(name, L"Artist")) return artist;
        if (equals(name, L"Title")) return title;
        if (equals(name, L"Album")) return album;
        if (equals(name, L"Tracknumber")) return number;
        if (equals(name, L"Filename"))
            return PlaylistFileTitle(track.path);
        std::string key;
        try { key = core::WideToUtf8(name); }
        catch (const std::exception&) { return std::wstring{}; }
        return PlaylistMetadataValue(track, key);
    };

    // 004868E7 feeds the custom rename mask through the same field lexer as
    // display-title formatting.  Parse once from left to right so metadata
    // containing '%' is inserted literally rather than recursively treated
    // as another token.  In particular Genre/Date and arbitrary %(...) keys
    // must not be erased as the older reconstruction did.
    std::wstring rendered;
    rendered.reserve(pattern.size() + title.size());
    for (size_t offset = 0; offset < pattern.size();) {
        if (pattern[offset] != L'%') {
            rendered.push_back(pattern[offset++]);
            continue;
        }
        if (++offset >= pattern.size()) break;
        const wchar_t token = pattern[offset++];
        if (token == L'(') {
            const size_t close = pattern.find(L')', offset);
            if (close == std::wstring::npos) break;
            rendered += long_field(std::wstring_view(pattern).substr(
                offset, close - offset));
            offset = close + 1;
            continue;
        }
        if (token == L'A') rendered += artist;
        else if (token == L'T') rendered += title;
        else if (token == L'L') rendered += album;
        else if (token == L'I' || token == L'N') rendered += number;
        else {
            const auto field = PlaylistFormatField(track, token, {}, {});
            if (field.recognized) rendered += field.rendered;
        }
    }
    return SafePlaylistFileName(std::move(rendered));
}

bool RevealPlaylistPath(HWND owner, const std::filesystem::path& path) {
    if (path.empty()) return false;
    if (LooksLikeUrl(path.wstring()))
        return reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open",
            path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    PIDLIST_ABSOLUTE item{};
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr)))
        return false;
    PIDLIST_ABSOLUTE parent = ILClone(item);
    PCUITEMID_CHILD child = ILFindLastID(item);
    const bool valid = parent && ILRemoveLastID(parent);
    const HRESULT result = valid
        ? SHOpenFolderAndSelectItems(parent, 1, &child, 0) : E_FAIL;
    if (parent) CoTaskMemFree(parent);
    CoTaskMemFree(item);
    return SUCCEEDED(result);
}

std::vector<wchar_t> ShellPathList(
    const std::vector<std::filesystem::path>& paths) {
    size_t count = 1;
    for (const auto& path : paths) count += path.wstring().size() + 1;
    std::vector<wchar_t> list(count, L'\0');
    wchar_t* cursor = list.data();
    for (const auto& path : paths) {
        const auto value = path.wstring();
        memcpy(cursor, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
        cursor += value.size() + 1;
    }
    return list;
}

bool RemoveReplayGainTags(const plugins::PluginManager* library,
                          const std::filesystem::path& path) {
    if (!library || path.empty() || LooksLikeUrl(path.wstring())) return false;
    HRESULT result{};
    // FUN_00484009 obtains the metadata interface from a writable file
    // stream.  A playback-only reader makes the shipped FLAC add-in return
    // STG_E_ACCESSDENIED from metadata slot 6 and leaves both tags intact.
    const auto reader = library->OpenReaderForMetadata(path, &result);
    if (!reader) return false;
    const HRESULT gain = reader->SetMetadataValueDirect(
        "replaygain_track_gain", std::wstring_view{});
    const HRESULT peak = reader->SetMetadataValueDirect(
        "replaygain_track_peak", std::wstring_view{});
    return SUCCEEDED(gain) && SUCCEEDED(peak);
}
} // namespace

std::wstring PlayerWindow::PlaylistDisplayText(
    const playlist::Track& track) const {
    return FormatPlaylistTitle(track, settings_.playlist,
                               ResourceText(0x81c8)).text;
}

bool PlayerWindow::HandlePlaylistTypeToSelect(bool catalogue,
                                               wchar_t character) {
    auto& typed = catalogue ? playlist_list_type_text_
                            : playlist_track_type_text_;
    auto& last_tick = catalogue ? playlist_list_type_tick_
                                : playlist_track_type_tick_;
    const DWORD now = GetTickCount();
    // The common controls discard their incremental-search string after a
    // short pause.  Use the system double-click interval as the user-scaled
    // lower bound, while retaining the ordinary one-second ListView cadence.
    const DWORD timeout = std::max<DWORD>(1000, GetDoubleClickTime());
    if (last_tick == 0 || now - last_tick > timeout) typed.clear();
    last_tick = now;

    if (character == L'\b') {
        if (!typed.empty()) typed.pop_back();
        if (typed.empty()) return true;
    } else {
        if (character < L' ' || character == 0x7f) return false;
        if (typed.size() >= 255) typed.clear();
        typed.push_back(character);
    }

    const auto focus = catalogue ? playlist_list_focus_ : playlist_selection_;
    LVFINDINFOW find{};
    find.flags = LVFI_STRING | LVFI_PARTIAL | LVFI_WRAP;
    find.psz = typed.c_str();
    const HWND control = catalogue ? playlist_list_control_
                                   : playlist_track_control_;
    const int previous = focus ? static_cast<int>(*focus) : -1;
    const int found = control
        ? static_cast<int>(SendMessageW(control, LVM_FINDITEMW,
                                        previous,
                                        reinterpret_cast<LPARAM>(&find)))
        : FindPlaylistControlPrefix(catalogue, previous + 1, find);
    if (found >= 0) {
        if (catalogue) SwitchPlaylist(static_cast<size_t>(found));
        else SelectPlaylistRow(static_cast<size_t>(found));
    }
    return true;
}

int PlayerWindow::FindPlaylistControlPrefix(
    bool catalogue, int start, const LVFINDINFOW& find) const {
    if (!find.psz) return -1;
    const std::wstring_view prefix(find.psz);
    const size_t count = catalogue ? playlists_.Size()
                                   : VisiblePlaylistTrackCount();
    // 00488A1F first asks CPlayItem for the configured display title and
    // 00489B9B asks CPlayList for its title.  Both retry a failed prefix
    // comparison with FUN_004C1D36's GB/DBCS A-Z initial projection.
    const std::wstring unknown_title_format = catalogue
        ? std::wstring{} : ResourceText(0x81c8);
    const auto found = FindOwnerDataListPrefix(count, start,
        (find.flags & LVFI_WRAP) != 0, [this, catalogue, prefix,
                                       &unknown_title_format](size_t index) {
            if (catalogue) {
                const auto& title = playlists_.At(index).Title();
                return LegacyPinyinPrefixMatches(title, prefix);
            }
            const auto* visible_track = VisiblePlaylistTrack(index);
            if (!visible_track) return false;
            const auto title = FormatPlaylistTitle(
                *visible_track, settings_.playlist,
                unknown_title_format).text;
            return LegacyPinyinPrefixMatches(title, prefix);
        });
    return found ? static_cast<int>(*found) : -1;
}

LRESULT PlayerWindow::HandlePlaylistControlMessage(HWND control, UINT message,
                                                    WPARAM wparam,
                                                    LPARAM lparam) {
    const HWND parent = GetParent(control);
    const int identifier = GetDlgCtrlID(control);
    const bool catalogue = identifier == kPlaylistListId;
    const bool tracks = identifier == kPlaylistTrackId;
    const bool list_control = catalogue || tracks || identifier == kPlaylistTreeId;
    const bool close_button = identifier == static_cast<int>(kCmdShowPlaylist);
    const auto forward_point = [&](UINT forwarded) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        MapWindowPoints(control, parent, &point, 1);
        if (forwarded == WM_MOUSEMOVE) {
            // Preserve the receiving HWND: TrackMouseEvent on the parent
            // immediately reports LEAVE while the pointer is over this child.
            return HandlePlaylistMessage(forwarded, wparam,
                MAKELPARAM(static_cast<short>(point.x),
                           static_cast<short>(point.y)), control);
        }
        return SendMessageW(parent, forwarded, wparam,
            MAKELPARAM(static_cast<short>(point.x),
                       static_cast<short>(point.y)));
    };
    switch (message) {
    case LVM_SETEXTENDEDLISTVIEWSTYLE: {
        if (!catalogue && !tracks) return 0;
        DWORD& style = catalogue ? playlist_list_extended_style_
                                 : playlist_track_extended_style_;
        const DWORD previous = style;
        const DWORD mask = static_cast<DWORD>(wparam);
        const DWORD requested = static_cast<DWORD>(lparam);
        style = mask == 0 ? requested
                          : ((style & ~mask) | (requested & mask));
        return static_cast<LRESULT>(previous);
    }
    case LVM_GETEXTENDEDLISTVIEWSTYLE:
        if (catalogue) return playlist_list_extended_style_;
        if (tracks) return playlist_track_extended_style_;
        return 0;
    case LVM_GETITEMCOUNT:
        if (catalogue) return static_cast<LRESULT>(playlists_.Size());
        if (tracks) return static_cast<LRESULT>(VisiblePlaylistTrackCount());
        return 0;
    case LVM_GETTOOLTIPS:
        return tracks ? reinterpret_cast<LRESULT>(playlist_item_tooltip_) : 0;
    case LVM_GETSELECTEDCOUNT:
        if (catalogue) return playlist_list_selection_ ? 1 : 0;
        if (tracks)
            return static_cast<LRESULT>(playlist_selected_rows_.size());
        return 0;
    case LVM_GETCOUNTPERPAGE: {
        if (!catalogue && !tracks) return 0;
        RECT client{};
        GetClientRect(control, &client);
        constexpr size_t row_height = 16;
        return static_cast<LRESULT>(std::max<size_t>(1,
            (static_cast<size_t>(std::max<LONG>(
                 0, client.bottom - client.top)) + row_height - 1) /
                row_height));
    }
    case LVM_GETITEMSTATE: {
        if (!list_control) break;
        const size_t index = static_cast<size_t>(wparam);
        UINT state{};
        if (catalogue) {
            if (playlist_list_selection_ == index) state |= LVIS_SELECTED;
            if (playlist_list_focus_ == index)
                state |= LVIS_FOCUSED;
        } else if (tracks) {
            if (playlist_selected_rows_.contains(index)) state |= LVIS_SELECTED;
            if (playlist_selection_ == index)
                state |= LVIS_FOCUSED;
        }
        return state & static_cast<UINT>(lparam);
    }
    case LVM_SETITEMSTATE: {
        if ((!catalogue && !tracks) || !lparam) return FALSE;
        const auto* item = reinterpret_cast<const LVITEMW*>(lparam);
        const int requested = static_cast<int>(wparam);
        const size_t count = catalogue ? playlists_.Size()
                                       : VisiblePlaylistTrackCount();
        if (requested < -1 ||
            (requested >= 0 && static_cast<size_t>(requested) >= count))
            return FALSE;

        const bool all = requested == -1;
        const bool selected = (item->state & LVIS_SELECTED) != 0;
        const bool focused = (item->state & LVIS_FOCUSED) != 0;
        if ((item->stateMask & LVIS_SELECTED) != 0) {
            if (catalogue) {
                if (!selected || all) playlist_list_selection_.reset();
                else playlist_list_selection_ =
                    static_cast<size_t>(requested);
            } else if (all) {
                playlist_selected_rows_.clear();
                if (selected) {
                    for (size_t index = 0; index < count; ++index)
                        playlist_selected_rows_.insert(index);
                }
            } else if (selected) {
                playlist_selected_rows_.insert(
                    static_cast<size_t>(requested));
            } else {
                playlist_selected_rows_.erase(
                    static_cast<size_t>(requested));
            }
        }
        if ((item->stateMask & LVIS_FOCUSED) != 0) {
            if (catalogue) {
                if (!focused || all) playlist_list_focus_.reset();
                else playlist_list_focus_ = static_cast<size_t>(requested);
            } else {
                if (!focused || all) playlist_selection_.reset();
                else playlist_selection_ = static_cast<size_t>(requested);
            }
        }
        if (tracks && !settings_.playlist.library_mode)
            RememberPlaylistRow(playlists_.ActiveIndex(),
                                playlist_selection_);
        if (parent) InvalidateRect(parent, nullptr, FALSE);
        return TRUE;
    }
    case LVM_GETNEXTITEM: {
        if (!list_control) break;
        const int first = static_cast<int>(wparam) + 1;
        const UINT flags = static_cast<UINT>(lparam);
        const size_t count = catalogue ? playlists_.Size()
                                      : VisiblePlaylistTrackCount();
        for (int index = std::max(0, first);
             index < static_cast<int>(count); ++index) {
            const bool selected = catalogue
                ? playlist_list_selection_ == static_cast<size_t>(index)
                : playlist_selected_rows_.contains(static_cast<size_t>(index));
            const bool focused = (catalogue
                ? playlist_list_focus_ == static_cast<size_t>(index)
                : playlist_selection_ == static_cast<size_t>(index));
            if (((flags & LVNI_SELECTED) == 0 || selected) &&
                ((flags & LVNI_FOCUSED) == 0 || focused)) return index;
        }
        return -1;
    }
    case LVM_GETTOPINDEX:
        if (catalogue) return static_cast<LRESULT>(playlist_list_scroll_);
        if (tracks) return static_cast<LRESULT>(playlist_scroll_);
        return 0;
    case LVM_FINDITEMW: {
        if ((!catalogue && !tracks) || !lparam) return -1;
        const auto* find = reinterpret_cast<const LVFINDINFOW*>(lparam);
        const size_t count = catalogue ? playlists_.Size()
                                       : VisiblePlaylistTrackCount();
        if (count == 0) return -1;
        const int previous = static_cast<int>(wparam);
        int start = previous < 0 ? 0 : previous + 1;
        if (start >= static_cast<int>(count)) {
            if ((find->flags & LVFI_WRAP) == 0) return -1;
            start = 0;
        }
        // A real LVS_OWNERDATA control translates LVM_FINDITEMW into this
        // notification.  Preserve that parent-visible contract even though
        // the rebuilt ListCtrl is a transparent message/keyboard child.
        NMLVFINDITEMW notification{};
        notification.hdr.hwndFrom = control;
        notification.hdr.idFrom = static_cast<UINT_PTR>(identifier);
        notification.hdr.code = LVN_ODFINDITEMW;
        notification.iStart = start;
        notification.lvfi = *find;
        return SendMessageW(parent, WM_NOTIFY,
            static_cast<WPARAM>(identifier),
            reinterpret_cast<LPARAM>(&notification));
    }
    case LVM_HITTEST: {
        if (!list_control || !lparam) return -1;
        auto* hit = reinterpret_cast<LVHITTESTINFO*>(lparam);
        hit->iItem = -1;
        hit->iSubItem = 0;
        hit->flags = LVHT_NOWHERE;
        RECT client{};
        GetClientRect(control, &client);
        if (!PtInRect(&client, hit->pt) || hit->pt.y < 0) return -1;
        constexpr size_t row_height = 16;
        const size_t first = catalogue ? playlist_list_scroll_ :
            (tracks ? playlist_scroll_ : 0);
        const size_t index = first +
            static_cast<size_t>(hit->pt.y) / row_height;
        const size_t count = catalogue ? playlists_.Size()
                                       : VisiblePlaylistTrackCount();
        if (index >= count) return -1;
        hit->iItem = static_cast<int>(index);
        hit->flags = LVHT_ONITEMLABEL;
        return hit->iItem;
    }
    case LVM_GETITEMRECT: {
        if (!list_control || !lparam) return FALSE;
        const size_t count = catalogue ? playlists_.Size()
                                      : VisiblePlaylistTrackCount();
        const size_t index = static_cast<size_t>(wparam);
        if (index >= count) return FALSE;
        RECT client{};
        GetClientRect(control, &client);
        auto* bounds = reinterpret_cast<RECT*>(lparam);
        const size_t first = catalogue ? playlist_list_scroll_ :
            (tracks ? playlist_scroll_ : 0);
        const auto relative = static_cast<std::int64_t>(index) -
                              static_cast<std::int64_t>(first);
        *bounds = {0, static_cast<LONG>(relative * 16), client.right,
                   static_cast<LONG>((relative + 1) * 16)};
        return TRUE;
    }
    case LVM_EDITLABELW:
        if (catalogue && static_cast<size_t>(wparam) < playlists_.Size()) {
            BeginPlaylistListEdit(static_cast<size_t>(wparam));
            return reinterpret_cast<LRESULT>(playlist_list_edit_);
        }
        return 0;
    case LVM_GETEDITCONTROL:
        return reinterpret_cast<LRESULT>(playlist_list_edit_);
    case LVM_SETITEMCOUNT: {
        if (!catalogue && !tracks) return FALSE;
        // The actual rows live in PlaylistStore/the media-library result
        // snapshot.  Their mutation precedes this owner-data bookkeeping
        // message, just as it precedes FUN_0042594A in the original.
        const size_t count = catalogue ? playlists_.Size()
                                       : VisiblePlaylistTrackCount();
        if (catalogue) {
            if (playlist_list_selection_ &&
                *playlist_list_selection_ >= count)
                playlist_list_selection_.reset();
            if (playlist_list_focus_ && *playlist_list_focus_ >= count)
                playlist_list_focus_.reset();
        } else {
            for (auto selected = playlist_selected_rows_.begin();
                 selected != playlist_selected_rows_.end();) {
                if (*selected >= count)
                    selected = playlist_selected_rows_.erase(selected);
                else
                    ++selected;
            }
            if (playlist_selection_ && *playlist_selection_ >= count)
                playlist_selection_.reset();
        }
        RECT client{};
        GetClientRect(control, &client);
        constexpr size_t row_height = 16;
        const size_t visible = std::max<size_t>(1,
            (static_cast<size_t>(std::max<LONG>(
                0, client.bottom - client.top)) + row_height - 1) /
            row_height);
        const size_t maximum = count > visible ? count - visible : 0;
        if (catalogue)
            playlist_list_scroll_ = std::min(playlist_list_scroll_, maximum);
        else
            playlist_scroll_ = std::min(playlist_scroll_, maximum);
        if (parent) InvalidateRect(parent, nullptr, FALSE);
        return TRUE;
    }
    case LVM_REDRAWITEMS:
        if (!catalogue && !tracks) return FALSE;
        if (parent) InvalidateRect(parent, nullptr, FALSE);
        return TRUE;
    case LVM_ENSUREVISIBLE:
        if (catalogue && static_cast<size_t>(wparam) < playlists_.Size()) {
            RECT client{};
            GetClientRect(control, &client);
            constexpr size_t row_height = 16;
            const size_t visible = std::max<size_t>(1,
                (static_cast<size_t>(std::max<LONG>(0,
                    client.bottom - client.top)) + row_height - 1) /
                    row_height);
            const size_t index = static_cast<size_t>(wparam);
            playlist_list_scroll_ = OwnerDataListEnsureVisibleTop(
                playlist_list_scroll_, visible, playlists_.Size(), index);
            LayoutPlaylistListEdit();
            InvalidateRect(parent, nullptr, FALSE);
            return TRUE;
        }
        if (tracks && static_cast<size_t>(wparam) < VisiblePlaylistTrackCount()) {
            RECT client{};
            GetClientRect(control, &client);
            constexpr size_t row_height = 16;
            const size_t visible = std::max<size_t>(1,
                (static_cast<size_t>(std::max<LONG>(0,
                    client.bottom - client.top)) + row_height - 1) /
                    row_height);
            const size_t previous = playlist_scroll_;
            playlist_scroll_ = OwnerDataListEnsureVisibleTop(
                playlist_scroll_, visible, VisiblePlaylistTrackCount(),
                static_cast<size_t>(wparam));
            if (playlist_scroll_ != previous) UpdatePlaylistItemTipRects();
            InvalidateRect(parent, nullptr, FALSE);
            return TRUE;
        }
        return FALSE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(control, &paint);
        EndPaint(control, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (list_control) return forward_point(message);
        if (close_button) {
            RECT client{};
            GetClientRect(control, &client);
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (PtInRect(&client, point)) {
                if (GetCapture() != control) SetCapture(control);
            } else if (GetCapture() == control &&
                       (wparam & MK_LBUTTON) == 0) {
                ReleaseCapture();
            }
            return forward_point(message);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_HOVER,
                                 control, HOVER_DEFAULT};
        TrackMouseEvent(&tracking);
        return forward_point(message);
    }
    case WM_LBUTTONDOWN:
        if (list_control) SetFocus(control);
        if (close_button && GetCapture() != control) SetCapture(control);
        return forward_point(message);
    case WM_LBUTTONUP: {
        const LRESULT result = forward_point(message);
        if (close_button && GetCapture() == control) ReleaseCapture();
        return result;
    }
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEHOVER:
        return forward_point(message);
    case WM_SETFOCUS:
        if (list_control) InvalidateRect(parent, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        if (catalogue) {
            playlist_list_type_text_.clear();
            playlist_list_type_tick_ = 0;
        } else if (tracks) {
            playlist_track_type_text_.clear();
            playlist_track_type_tick_ = 0;
        }
        if (list_control) InvalidateRect(parent, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        if (list_control) return DLGC_WANTARROWS | DLGC_WANTCHARS;
        break;
    case WM_CHAR:
        if (catalogue || tracks) {
            static_cast<void>(HandlePlaylistTypeToSelect(
                catalogue, static_cast<wchar_t>(wparam)));
            return 0;
        }
        break;
    case WM_KEYDOWN:
    case WM_MOUSEWHEEL:
        if (list_control) return SendMessageW(parent, message, wparam, lparam);
        break;
    case WM_MOUSELEAVE:
        return HandlePlaylistMessage(message, wparam, lparam, control);
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        return SendMessageW(parent, message, wparam, lparam);
    case WM_CONTEXTMENU:
    case WM_SETCURSOR:
    case WM_NOTIFY:
        return SendMessageW(parent, message, wparam, lparam);
    default:
        return DefWindowProcW(control, message, wparam, lparam);
    }
    return DefWindowProcW(control, message, wparam, lparam);
}

LRESULT PlayerWindow::HandlePlaylistMessage(UINT message, WPARAM wparam,
                                           LPARAM lparam, HWND mouse_source) {
    const auto geometry = [this]() {
        RECT client{};
        if (playlist_window_) GetClientRect(playlist_window_, &client);
        return skin_ ? MakePlaylistGeometry(skin_->Playlist(),
            settings_.playlist.split_on_lists, client.right, client.bottom,
            VisiblePlaylistTrackCount()) : PlaylistGeometry{};
    };
    const auto scrollbar_metrics = [this, &geometry]() {
        if (!skin_) return PlaylistScrollbarMetrics{};
        const auto bounds = geometry();
        const size_t count = VisiblePlaylistTrackCount();
        const size_t page = static_cast<size_t>(
            std::max(1, bounds.visible_rows));
        return ResolvePlaylistScrollbarMetrics(
            bounds, skin_->Playlist(), count, page, playlist_scroll_);
    };
    const auto scrollbar_hit = [&geometry, &scrollbar_metrics](POINT point) {
        const auto bounds = geometry();
        if (bounds.scrollbar_width <= 0 ||
            !PtInRect(&bounds.scrollbar, point))
            return PlaylistScrollbarPart::none;
        return HitTestPlaylistScrollbar(scrollbar_metrics(), point.y);
    };
    const auto rating_bounds = [this, &geometry](size_t index)
        -> std::optional<RECT> {
        const auto* track = VisiblePlaylistTrack(index);
        if (!playlist_window_ || !track)
            return std::nullopt;
        const auto metrics = geometry();
        if (index < playlist_scroll_) return std::nullopt;
        const size_t relative = index - playlist_scroll_;
        if (relative >= static_cast<size_t>(metrics.visible_rows))
            return std::nullopt;
        RECT row{metrics.tracks.left,
                 metrics.tracks.top + static_cast<LONG>(relative) *
                     metrics.row_height,
                 metrics.tracks.right,
                 std::min(metrics.tracks.bottom,
                     metrics.tracks.top + static_cast<LONG>(relative + 1) *
                         metrics.row_height)};
        const HDC dc = GetDC(playlist_window_);
        if (!dc) return std::nullopt;
        const HFONT font = CreatePlaylistFont(settings_.playlist);
        const HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
        const auto bounds = PlaylistRatingRect(dc, row,
            VisiblePlaylistTrackCount(),
            *track, settings_.playlist,
            ResourceText(0x81c8));
        if (font) {
            SelectObject(dc, old_font);
            DeleteObject(font);
        }
        ReleaseDC(playlist_window_, dc);
        return bounds;
    };
    const auto rating_hit = [this, &rating_bounds](POINT point)
        -> std::optional<PlaylistRatingHit> {
        const auto row = PlaylistTrackAt(point);
        if (!row) return std::nullopt;
        const auto bounds = rating_bounds(*row);
        if (!bounds || !PtInRect(&*bounds, point)) return std::nullopt;
        const LONG cell = (bounds->right - bounds->left) / 5;
        if (cell <= 0) return std::nullopt;
        // FUN_0050ADA8 truncates the non-negative quotient; the narrow tail
        // left by integer division may produce six and 004AE10A rejects it.
        const int rating = (point.x - bounds->left) / cell + 1;
        if (rating < 1 || rating > 5) return std::nullopt;
        return PlaylistRatingHit{*row, rating};
    };
    static const UINT find_message = RegisterWindowMessageW(FINDMSGSTRINGW);
    if (message == find_message) {
        const auto* find = reinterpret_cast<const FINDREPLACEW*>(lparam);
        if (find && (find->Flags & FR_DIALOGTERM) != 0) {
            playlist_find_dialog_ = nullptr;
        } else if (find && (find->Flags & FR_FINDNEXT) != 0 &&
                   playlist_find_text_[0] != L'\0') {
            static_cast<void>(FindNextPlaylistTrack(find->Flags));
        }
        return 0;
    }
    switch (message) {
    case WM_ACTIVATE:
        RaiseSkinOwnerOnActivation(playlist_window_, wparam, lparam);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        if (!limits || !skin_ || !skin_->Playlist().valid) return 0;
        const SIZE native = skin_->Playlist().background.size;
        limits->ptMinTrackSize = {native.cx, native.cy};
        const RECT resize = skin_->Playlist().resize_rect;
        if (resize.right <= resize.left || resize.bottom <= resize.top) {
            limits->ptMaxTrackSize = limits->ptMinTrackSize;
        } else {
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            limits->ptMaxTrackSize = {work.right - work.left, work.bottom - work.top};
        }
        return 0;
    }
    case WM_SIZE:
        UpdatePlaylistWindowRegion();
        LayoutPlaylistListControls();
        UpdatePlaylistToolRects();
        LayoutPlaylistListEdit();
        InvalidateRect(playlist_window_, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wparam == kPlaylistScrollbarRepeatTimer) {
            if (playlist_scrollbar_pressed_ ==
                    PlaylistScrollbarPart::none ||
                playlist_scrollbar_pressed_ ==
                    PlaylistScrollbarPart::thumb ||
                GetCapture() != playlist_window_) {
                CancelPlaylistScrollbarInteraction(false);
                return 0;
            }
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(playlist_window_, &point);
            const auto hit = scrollbar_hit(point);
            const auto next_hover = hit == playlist_scrollbar_pressed_
                ? hit : PlaylistScrollbarPart::none;
            if (next_hover != playlist_scrollbar_hover_) {
                playlist_scrollbar_hover_ = next_hover;
                InvalidateRect(playlist_window_, nullptr, FALSE);
            }
            // Page-repeat naturally stops once the moving thumb reaches the
            // pointer: hit-testing the freshly computed geometry changes the
            // part from page_up/page_down to thumb.
            if (hit == playlist_scrollbar_pressed_) {
                const auto state = scrollbar_metrics();
                const int step = PlaylistScrollbarStep(
                    playlist_scrollbar_pressed_, state.page_size);
                if (step != 0) ScrollPlaylist(step);
            }
            if (!playlist_scrollbar_repeat_fast_) {
                playlist_scrollbar_repeat_fast_ = true;
                SetTimer(playlist_window_, kPlaylistScrollbarRepeatTimer,
                         kPlaylistScrollbarRepeatIntervalMs, nullptr);
            }
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(playlist_window_, &paint);
        PaintPlaylist(dc);
        EndPaint(playlist_window_, &paint);
        return 0;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header && header->code == LVN_ODFINDITEMW &&
            (header->idFrom == static_cast<UINT_PTR>(kPlaylistListId) ||
             header->idFrom == static_cast<UINT_PTR>(kPlaylistTrackId))) {
            const auto* find = reinterpret_cast<const NMLVFINDITEMW*>(lparam);
            return FindPlaylistControlPrefix(
                header->idFrom == static_cast<UINT_PTR>(kPlaylistListId),
                find->iStart, find->lvfi);
        }
        if (HandleMediaLibraryTreeNotification(header)) return 0;
        if (HandleToolTipNotification(playlist_window_, lparam)) return 0;
        break;
    }
    case WM_INITMENUPOPUP: {
        const HMENU popup = reinterpret_cast<HMENU>(wparam);
        // CPlayerWnd_OnInitMenuPopup (00461BAE) identifies this submenu by
        // its first resource command.  Populate only when the user opens it,
        // then owner-draw the rows appended after BeginPopupMenuStyle.
        if (popup && GetMenuItemID(popup, 0) == kPlaylistSendToFolder) {
            PopulatePlaylistSendToMenu(popup);
            ApplyPopupMenuStyle(popup);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        CancelPlaylistScrollbarInteraction(true);
        FinishPlaylistListEdit(false);
        SetSkinWindowVisible(playlist_window_, false);
        settings_.player.playlist_visible = false;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(playlist_window_, &point);
        const auto metrics = geometry();
        if (PtInRect(&metrics.splitter, point)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        if (const auto track = PlaylistTrackAt(point)) {
            if (const auto bounds = rating_bounds(*track);
                bounds && PtInRect(&*bounds, point)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        const unsigned int hit = PlaylistDragHitTest(point);
        if (hit != kDragMove) {
            LPCWSTR cursor = IDC_ARROW;
            if (hit == (kDragRight | kDragBottom) ||
                hit == (kDragLeft | kDragTop)) {
                cursor = IDC_SIZENWSE;
            } else if (hit == (kDragLeft | kDragBottom) ||
                       hit == (kDragRight | kDragTop)) {
                cursor = IDC_SIZENESW;
            } else if ((hit & (kDragLeft | kDragRight)) != 0) {
                cursor = IDC_SIZEWE;
            } else if ((hit & (kDragTop | kDragBottom)) != 0) {
                cursor = IDC_SIZENS;
            }
            SetCursor(LoadCursorW(nullptr, cursor));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (playlist_toolbar_menu_return_point_ &&
            (point.x != playlist_toolbar_menu_return_point_->x ||
             point.y != playlist_toolbar_menu_return_point_->y))
            playlist_toolbar_menu_return_point_.reset();
        if (playlist_mouse_tracking_window_ != mouse_source &&
            IsWindow(playlist_mouse_tracking_window_)) {
            TRACKMOUSEEVENT cancel{sizeof(cancel), TME_CANCEL | TME_LEAVE,
                                   playlist_mouse_tracking_window_, 0};
            TrackMouseEvent(&cancel);
        }
        playlist_mouse_tracking_window_ = mouse_source;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE,
                                 mouse_source, 0};
        TrackMouseEvent(&tracking);
        if (playlist_rating_gesture_.Active()) {
            const bool still_valid = (wparam & MK_LBUTTON) != 0 &&
                playlist_rating_gesture_.Continue(rating_hit(point));
            if (!still_valid && GetCapture() == playlist_window_)
                ReleaseCapture();
            return 0;
        }
        if (dragging_skin_background_ && skin_drag_window_ == playlist_window_ &&
            GetCapture() == playlist_window_) {
            ContinueSkinBackgroundDrag(playlist_window_, point);
            return 0;
        }
        if (playlist_splitter_dragging_ && GetCapture() == playlist_window_) {
            const auto metrics = geometry();
            const int maximum = std::max<int>(0, metrics.list.right - metrics.list.left -
                (metrics.splitter.right - metrics.splitter.left) - metrics.scrollbar_width - 24);
            settings_.playlist.split_on_lists = std::clamp<int>(
                point.x - metrics.list.left, 0, maximum);
            LayoutPlaylistListControls();
            UpdatePlaylistItemTipRects();
            InvalidateRect(playlist_window_, nullptr, FALSE);
            return 0;
        }
        if (playlist_scrollbar_pressed_ != PlaylistScrollbarPart::none &&
            GetCapture() == playlist_window_) {
            const auto state = scrollbar_metrics();
            if (playlist_scrollbar_dragging_ &&
                playlist_scrollbar_pressed_ ==
                    PlaylistScrollbarPart::thumb) {
                const int thumb_top =
                    playlist_scrollbar_drag_anchor_thumb_top_ +
                    point.y - playlist_scrollbar_drag_anchor_y_;
                const size_t next = PlaylistScrollbarPositionFromThumbTop(
                    state, thumb_top);
                const size_t previous = playlist_scroll_;
                playlist_scroll_ = std::min(next, state.maximum);
                if (playlist_scroll_ != previous)
                    UpdatePlaylistItemTipRects();
                // The thumb remains in pressed frame while captured even if
                // the pointer is moved horizontally outside the narrow bar.
                playlist_scrollbar_hover_ = PlaylistScrollbarPart::thumb;
                InvalidateRect(playlist_window_, nullptr, FALSE);
                return 0;
            }
            const auto hit = scrollbar_hit(point);
            const auto next_hover = hit == playlist_scrollbar_pressed_
                ? hit : PlaylistScrollbarPart::none;
            if (next_hover != playlist_scrollbar_hover_) {
                playlist_scrollbar_hover_ = next_hover;
                InvalidateRect(playlist_window_, nullptr, FALSE);
            }
            return 0;
        }
        if (playlist_scrollbar_dragging_) {
            // Capture can be transferred before WM_CAPTURECHANGED is
            // dispatched by an embedded control.  Never continue mapping
            // pointer pixels to rows without ownership.
            CancelPlaylistScrollbarInteraction(false);
            return 0;
        }
        if (playlist_list_drag_pending_ && GetCapture() == playlist_window_) {
            if (!playlist_list_dragging_) {
                const int threshold_x = GetSystemMetrics(SM_CXDRAG);
                const int threshold_y = GetSystemMetrics(SM_CYDRAG);
                playlist_list_dragging_ =
                    std::abs(point.x - playlist_list_drag_origin_.x) >=
                        threshold_x ||
                    std::abs(point.y - playlist_list_drag_origin_.y) >=
                        threshold_y;
            }
            if (playlist_list_dragging_) {
                const auto insertion = PlaylistListInsertionAt(point);
                if (playlist_list_drop_index_ != insertion) {
                    playlist_list_drop_index_ = insertion;
                    const auto metrics = geometry();
                    InvalidateRect(playlist_window_, &metrics.list_titles,
                                   FALSE);
                }
                return 0;
            }
        }
        if (playlist_track_drag_pending_ && GetCapture() == playlist_window_) {
            if (!playlist_track_dragging_ &&
                settings_.playlist.enable_drag_drop) {
                const int threshold_x = GetSystemMetrics(SM_CXDRAG);
                const int threshold_y = GetSystemMetrics(SM_CYDRAG);
                playlist_track_dragging_ =
                    std::abs(point.x - playlist_track_drag_origin_.x) >= threshold_x ||
                    std::abs(point.y - playlist_track_drag_origin_.y) >= threshold_y;
            }
            if (playlist_track_dragging_) {
                const auto metrics = geometry();
                RECT client{};
                GetClientRect(playlist_window_, &client);
                if (!PtInRect(&client, point)) {
                    // Once the pointer leaves CPlayListWnd, switch from the
                    // lightweight in-window reorder feedback to the same OLE
                    // source contract used by 004894AF. This enables drops on
                    // Explorer, devices and other shell targets without
                    // reinterpreting a cancelled internal release as a row.
                    playlist_track_drag_pending_ = false;
                    playlist_track_dragging_ = false;
                    playlist_track_drop_row_.reset();
                    playlist_list_hover_.reset();
                    if (GetCapture() == playlist_window_) ReleaseCapture();
                    BeginPlaylistOleDrag();
                    InvalidateRect(playlist_window_, nullptr, FALSE);
                    return 0;
                }
                // The native OLE path (0048218C/004822AD) permits selected
                // Files rows to cross into the PlayLists catalogue.  Keep the
                // lightweight in-window drag implementation, but expose the
                // same target before computing a right-list insertion row.
                if (!settings_.playlist.library_mode &&
                    settings_.playlist.split_on_lists > 0 &&
                    PtInRect(&metrics.list_titles, point)) {
                    const auto list = PlaylistListAt(point);
                    if (playlist_list_hover_ != list ||
                        playlist_track_drop_row_.has_value()) {
                        playlist_list_hover_ = list;
                        playlist_track_drop_row_.reset();
                        InvalidateRect(playlist_window_, nullptr, FALSE);
                    }
                    return 0;
                }
                if (!PtInRect(&metrics.tracks, point)) {
                    const bool had_feedback =
                        playlist_list_hover_.has_value() ||
                        playlist_track_drop_row_.has_value();
                    playlist_list_hover_.reset();
                    playlist_track_drop_row_.reset();
                    if (had_feedback)
                        InvalidateRect(playlist_window_, nullptr, FALSE);
                    return 0;
                }
                playlist_list_hover_.reset();
                if (point.y < metrics.tracks.top) ScrollPlaylist(-1);
                else if (point.y >= metrics.tracks.bottom) ScrollPlaylist(1);
                const int relative = std::clamp<int>(
                    static_cast<int>(point.y - metrics.tracks.top +
                        metrics.row_height / 2) / metrics.row_height,
                    0, metrics.visible_rows);
                playlist_track_drop_row_ = std::min(
                    playlist_scroll_ + static_cast<size_t>(relative),
                    VisiblePlaylistTrackCount());
                InvalidateRect(playlist_window_, &metrics.tracks, FALSE);
                return 0;
            }
        }
        const auto hovered = PlaylistTrackAt(point);
        const auto list_hovered = PlaylistListAt(point);
        const auto toolbar_hovered = PlaylistToolbarButtonAt(point);
        const auto scrollbar_hovered = scrollbar_hit(point);
        const auto metrics = geometry();
        const bool close_hovered = skin_ && skin_->Playlist().close.image &&
            PtInRect(&metrics.close, point);
        if (hovered != playlist_hover_ || list_hovered != playlist_list_hover_ ||
            toolbar_hovered != playlist_toolbar_hover_ ||
            scrollbar_hovered != playlist_scrollbar_hover_ ||
            close_hovered != playlist_close_hover_) {
            playlist_hover_ = hovered;
            playlist_list_hover_ = list_hovered;
            playlist_toolbar_hover_ = toolbar_hovered;
            playlist_scrollbar_hover_ = scrollbar_hovered;
            playlist_close_hover_ = close_hovered;
            InvalidateRect(playlist_window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        // A parent/previous button's queued LEAVE must not clear the hot
        // frame after a different child has already received MOUSEMOVE.
        if (mouse_source != playlist_mouse_tracking_window_) return 0;
        playlist_mouse_tracking_window_ = nullptr;
        if (playlist_rating_gesture_.Active()) {
            playlist_rating_gesture_.Cancel();
            if (GetCapture() == playlist_window_) ReleaseCapture();
        }
        playlist_hover_.reset();
        playlist_list_hover_.reset();
        // The common-control toolbar retains the hot item when Escape is
        // followed by leaving without another move within the toolbar.
        // Menu teardown can synthesize an unchanged-position MOUSEMOVE;
        // that must not turn the pending leave into a real hot-item change.
        if (!playlist_toolbar_menu_return_point_)
            playlist_toolbar_hover_.reset();
        playlist_toolbar_menu_return_point_.reset();
        playlist_scrollbar_hover_ = PlaylistScrollbarPart::none;
        playlist_close_hover_ = false;
        InvalidateRect(playlist_window_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const auto metrics = geometry();
        const unsigned int resize_hit = PlaylistDragHitTest(point);
        if (resize_hit != kDragMove) {
            SetFocus(playlist_window_);
            BeginSkinBackgroundDrag(playlist_window_, point, resize_hit);
            return 0;
        }
        if (skin_ && skin_->Playlist().close.image && PtInRect(&metrics.close, point)) {
            playlist_close_pressed_ = true;
            if (!GetCapture()) SetCapture(playlist_window_);
            InvalidateRect(playlist_window_, &metrics.close, FALSE);
            return 0;
        }
        if (const auto button = PlaylistToolbarButtonAt(point)) {
            // The original control is a native drop-down toolbar.  Its
            // TBN_DROPDOWN notification opens the resource submenu on button
            // down; it does not wait for a captured button-up.  This also
            // removes the hot frame while the popup owns mouse input.
            if (IsWindow(playlist_mouse_tracking_window_)) {
                TRACKMOUSEEVENT cancel{sizeof(cancel), TME_CANCEL | TME_LEAVE,
                                       playlist_mouse_tracking_window_, 0};
                TrackMouseEvent(&cancel);
            }
            playlist_mouse_tracking_window_ = nullptr;
            playlist_toolbar_menu_return_point_.reset();
            playlist_toolbar_hover_.reset();
            InvalidateRect(playlist_window_, &metrics.toolbar, FALSE);
            UpdateWindow(playlist_window_);
            constexpr int button_count = 7;
            const int width = metrics.toolbar.right - metrics.toolbar.left;
            POINT screen{
                metrics.toolbar.left + static_cast<int>(*button) * width / button_count,
                metrics.toolbar.bottom};
            if (skin_->Playlist().toolbar_items) {
                const RECT item = PlaylistToolbarItemBounds(skin_->Playlist(), metrics.toolbar, *button);
                screen = {item.left, item.bottom};
            }
            ClientToScreen(playlist_window_, &screen);
            InvokePlaylistToolbar(*button, screen);
            if (IsWindow(playlist_mouse_tracking_window_)) {
                TRACKMOUSEEVENT cancel{sizeof(cancel), TME_CANCEL | TME_LEAVE,
                                       playlist_mouse_tracking_window_, 0};
                TrackMouseEvent(&cancel);
            }
            playlist_mouse_tracking_window_ = nullptr;
            // The native toolbar restores its hot item on return from
            // TBN_DROPDOWN (notably Escape), and rearms leave tracking on
            // the next MOUSEMOVE. Do not keep a pre-menu LEAVE subscription.
            POINT cursor{};
            if (IsWindow(playlist_window_) && GetCursorPos(&cursor)) {
                const HWND under_cursor = WindowFromPoint(cursor);
                if (under_cursor == playlist_window_ ||
                    IsChild(playlist_window_, under_cursor)) {
                    ScreenToClient(playlist_window_, &cursor);
                    playlist_toolbar_hover_ = PlaylistToolbarButtonAt(cursor);
                    if (playlist_toolbar_hover_)
                        playlist_toolbar_menu_return_point_ = cursor;
                    InvalidateRect(playlist_window_, nullptr, FALSE);
                }
            }
            return 0;
        }
        if (metrics.splitter.right > metrics.splitter.left &&
            PtInRect(&metrics.splitter, point)) {
            playlist_splitter_dragging_ = true;
            SetCapture(playlist_window_);
            return 0;
        }
        if (metrics.scrollbar_width > 0 && PtInRect(&metrics.scrollbar, point) &&
            VisiblePlaylistTrackCount() > static_cast<size_t>(
                std::max(1, metrics.visible_rows))) {
            const auto state = scrollbar_metrics();
            const auto part = HitTestPlaylistScrollbar(state, point.y);
            if (part != PlaylistScrollbarPart::none) {
                if (playlist_track_control_) SetFocus(playlist_track_control_);
                playlist_scrollbar_pressed_ = part;
                playlist_scrollbar_hover_ = part;
                playlist_scrollbar_repeat_fast_ = false;
                playlist_scrollbar_dragging_ =
                    part == PlaylistScrollbarPart::thumb;
                playlist_scrollbar_drag_anchor_y_ = point.y;
                playlist_scrollbar_drag_anchor_thumb_top_ = state.thumb_top;
                SetCapture(playlist_window_);
                if (!playlist_scrollbar_dragging_) {
                    const int step = PlaylistScrollbarStep(
                        part, state.page_size);
                    if (step != 0) ScrollPlaylist(step);
                    SetTimer(playlist_window_,
                             kPlaylistScrollbarRepeatTimer,
                             std::max<UINT>(1, GetDoubleClickTime()),
                             nullptr);
                } else {
                    InvalidateRect(playlist_window_, &metrics.scrollbar,
                                   FALSE);
                }
            }
            return 0;
        }
        if (const auto list = PlaylistListAt(point)) {
            if (playlist_list_control_) SetFocus(playlist_list_control_);
            SwitchPlaylist(*list);
            // The original native control emits LVN_BEGINDRAG only after the
            // system drag rectangle is crossed.  Capture from button-down so
            // the rebuilt parent can reproduce that transition and receive a
            // release even when the pointer leaves the child.
            if (playlists_.Size() > 1) {
                playlist_list_drag_pending_ = true;
                playlist_list_dragging_ = false;
                playlist_list_drag_origin_ = point;
                playlist_list_drag_row_ = *list;
                playlist_list_drop_index_.reset();
                SetCapture(playlist_window_);
            }
            return 0;
        }
        if (const auto track = PlaylistTrackAt(point)) {
            if (playlist_track_control_) SetFocus(playlist_track_control_);
            const bool toggle = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool extend = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            playlist_track_press_was_selected_ = playlist_selected_rows_.contains(*track);
            playlist_track_press_ctrl_ = toggle;
            playlist_track_press_shift_ = extend;
            // A native multi-select ListView does not remove an already
            // selected row on Ctrl+button-down: LVN_BEGINDRAG must still be
            // able to carry that selection.  Defer the Ctrl-toggle until
            // button-up only when the pointer never crossed the drag metric.
            const bool defer_selected_ctrl_click =
                playlist_track_press_was_selected_ && toggle && !extend;
            if (!playlist_track_press_was_selected_ || extend ||
                (toggle && !defer_selected_ctrl_click)) {
                SelectPlaylistRow(*track, toggle, extend);
            } else {
                playlist_selection_ = *track;
                if (!settings_.playlist.library_mode)
                    RememberPlaylistRow(playlists_.ActiveIndex(),
                                        playlist_selection_);
            }
            if (const auto hit = rating_hit(point)) {
                // FUN_00488C19 is the Files ListCtrl's NM_CLICK handler.  It
                // computes and applies the star from the notification's
                // release point; button-down only establishes the candidate.
                playlist_rating_gesture_.Begin(*hit);
                SetCapture(playlist_window_);
                return 0;
            }
            if (playlist_selected_rows_.contains(*track) ||
                defer_selected_ctrl_click) {
                playlist_track_drag_pending_ = true;
                playlist_track_dragging_ = false;
                playlist_track_drag_origin_ = point;
                playlist_track_drag_row_ = *track;
                playlist_track_drop_row_.reset();
                SetCapture(playlist_window_);
            }
        } else if (PtInRect(&metrics.list_titles, point)) {
            // Native ListCtrl blank-area handling clears LVIS_SELECTED but
            // leaves the caret item (LVIS_FOCUSED) and keyboard focus intact.
            // NM_CLICK then carries iItem == -1, so FUN_00489A9D performs no
            // active-list switch or other follow-up work.
            if (playlist_list_control_) SetFocus(playlist_list_control_);
            playlist_list_selection_.reset();
            InvalidateRect(playlist_window_, &metrics.list_titles, FALSE);
            return 0;
        } else if (PtInRect(&metrics.tracks, point)) {
            // The Files ListCtrl follows the same native rule: a plain click
            // below the final row deselects every item but retains its focus
            // caret.  Playback/current-track state is deliberately untouched.
            if (playlist_track_control_) SetFocus(playlist_track_control_);
            playlist_selected_rows_.clear();
            playlist_track_drag_pending_ = false;
            playlist_track_dragging_ = false;
            playlist_track_drag_row_.reset();
            playlist_track_drop_row_.reset();
            InvalidateRect(playlist_window_, &metrics.tracks, FALSE);
            return 0;
        } else if (PtInRect(&metrics.list, point)) {
            // The original playlist surface is a real ListCtrl child.  Its
            // whole client rectangle consumes button messages, including
            // rows below the final item; only the uncovered parent skin can
            // enter FUN_0046EAAC's movable-window path.
            return 0;
        } else {
            SetFocus(playlist_window_);
            BeginSkinBackgroundDrag(playlist_window_, point, kDragMove);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const auto metrics = geometry();
        if (playlist_rating_gesture_.Active()) {
            const auto committed = playlist_rating_gesture_.Release(
                rating_hit(point));
            if (GetCapture() == playlist_window_) ReleaseCapture();
            if (committed && SetVisiblePlaylistRating(
                    committed->row, committed->rating)) {
                if (const auto bounds = rating_bounds(committed->row))
                    InvalidateRect(playlist_window_, &*bounds, FALSE);
            }
            return 0;
        }
        if (playlist_close_pressed_) {
            playlist_close_pressed_ = false;
            if (GetCapture() == playlist_window_) ReleaseCapture();
            if (skin_ && PtInRect(&metrics.close, point)) {
                SetSkinWindowVisible(playlist_window_, false);
                settings_.player.playlist_visible = false;
                if (window_) InvalidateRect(window_, nullptr, FALSE);
            }
            InvalidateRect(playlist_window_, nullptr, FALSE);
            return 0;
        }
        if (playlist_scrollbar_pressed_ != PlaylistScrollbarPart::none ||
            playlist_scrollbar_dragging_) {
            CancelPlaylistScrollbarInteraction(true);
            playlist_scrollbar_hover_ = scrollbar_hit(point);
            if (playlist_scrollbar_hover_ != PlaylistScrollbarPart::none)
                InvalidateRect(playlist_window_, &metrics.scrollbar, FALSE);
            return 0;
        }
        if (playlist_splitter_dragging_) {
            playlist_splitter_dragging_ = false;
            if (GetCapture() == playlist_window_) ReleaseCapture();
            return 0;
        }
        if (playlist_list_drag_pending_) {
            const bool dragged = playlist_list_dragging_;
            if (dragged) FinishPlaylistListDrag();
            playlist_list_drag_pending_ = false;
            playlist_list_dragging_ = false;
            playlist_list_drag_row_.reset();
            playlist_list_drop_index_.reset();
            if (GetCapture() == playlist_window_) ReleaseCapture();
            if (dragged) InvalidateRect(playlist_window_, nullptr, FALSE);
            return 0;
        }
        if (playlist_track_drag_pending_) {
            const bool dragged = playlist_track_dragging_;
            if (dragged) {
                FinishPlaylistTrackDrag(point);
            } else if (playlist_track_press_was_selected_ &&
                       playlist_track_drag_row_) {
                if (playlist_track_press_ctrl_ &&
                    !playlist_track_press_shift_) {
                    SelectPlaylistRow(*playlist_track_drag_row_, true, false);
                } else if (!playlist_track_press_shift_) {
                    SelectPlaylistRow(*playlist_track_drag_row_);
                }
            }
            playlist_track_drag_pending_ = false;
            playlist_track_dragging_ = false;
            playlist_track_press_ctrl_ = false;
            playlist_track_press_shift_ = false;
            playlist_track_drag_row_.reset();
            playlist_track_drop_row_.reset();
            if (GetCapture() == playlist_window_) ReleaseCapture();
            if (dragged) InvalidateRect(playlist_window_, nullptr, FALSE);
            return 0;
        }
        if (dragging_skin_background_ && skin_drag_window_ == playlist_window_) {
            EndSkinMouseCapture();
            return 0;
        }
        break;
    }
    case WM_LBUTTONDBLCLK: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (const auto track = PlaylistTrackAt(point)) {
            SelectPlaylistRow(*track, false, false,
                PlaylistSelectionTrigger::item_activated);
        } else if (!settings_.playlist.library_mode) {
            if (const auto list = PlaylistListAt(point))
                ActivatePlaylistCatalogueRow(*list);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        const int rows = -GET_WHEEL_DELTA_WPARAM(wparam) /
            WHEEL_DELTA * 3;
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(playlist_window_, &point);
        const auto metrics = geometry();
        const HWND focus = GetFocus();
        const bool catalogue = !settings_.playlist.library_mode &&
            (PtInRect(&metrics.list_titles, point) ||
             focus == playlist_list_control_ ||
             focus == playlist_list_edit_);
        if (!catalogue) {
            ScrollPlaylist(rows);
            return 0;
        }
        const size_t visible = static_cast<size_t>(metrics.visible_rows);
        const size_t maximum = playlists_.Size() > visible
            ? playlists_.Size() - visible : 0;
        const auto next = static_cast<long long>(playlist_list_scroll_) + rows;
        playlist_list_scroll_ = static_cast<size_t>(std::clamp<long long>(
            next, 0, static_cast<long long>(maximum)));
        LayoutPlaylistListEdit();
        InvalidateRect(playlist_window_, &metrics.list_titles, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        if (wparam == VK_APPS ||
            (wparam == VK_F10 &&
             (GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
            SendMessageW(playlist_window_, WM_CONTEXTMENU,
                reinterpret_cast<WPARAM>(GetFocus()),
                MAKELPARAM(-1, -1));
            return 0;
        }
        if (GetFocus() == playlist_list_control_ && playlists_.Size() != 0) {
            size_t index = playlist_list_focus_.value_or(
                playlists_.ActiveIndex());
            if (wparam == VK_UP && index > 0) --index;
            else if (wparam == VK_DOWN && index + 1 < playlists_.Size()) ++index;
            else if (wparam == VK_HOME) index = 0;
            else if (wparam == VK_END) index = playlists_.Size() - 1;
            else if (wparam == VK_F2) {
                BeginPlaylistListEdit(index);
                return 0;
            } else if (wparam == VK_DELETE) {
                playlist_context_list_ = index;
                HandlePlaylistCommand(kPlaylistDeleteList);
                return 0;
            } else if (wparam != VK_RETURN) {
                return 0;
            }
            if (wparam == VK_RETURN)
                ActivatePlaylistCatalogueRow(index);
            else
                SwitchPlaylist(index);
            SendMessageW(playlist_list_control_, LVM_ENSUREVISIBLE,
                         index, FALSE);
            return 0;
        }
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (control &&
            (wparam == L'C' || wparam == L'c')) {
            HandlePlaylistCommand(kPlaylistCopy);
        } else if (control &&
                   (wparam == L'X' || wparam == L'x')) {
            HandlePlaylistCommand(kPlaylistCut);
        } else if (control &&
                   (wparam == L'V' || wparam == L'v')) {
            HandlePlaylistCommand(kPlaylistPaste);
        } else if (wparam == VK_DELETE) {
            if (settings_.playlist.library_mode)
                static_cast<void>(
                    HandleMediaLibraryCommand(kPlaylistDeleteSelected));
            else
                HandlePlaylistCommand(kPlaylistDeleteSelected);
        } else if (wparam == VK_RETURN && playlist_selection_) {
            SelectPlaylistRow(*playlist_selection_, false, false,
                PlaylistSelectionTrigger::item_activated);
        }
        else if ((wparam == VK_UP || wparam == VK_DOWN ||
                  wparam == VK_HOME || wparam == VK_END ||
                  wparam == VK_PRIOR || wparam == VK_NEXT) &&
                 VisiblePlaylistTrackCount() != 0) {
            const size_t count = VisiblePlaylistTrackCount();
            size_t target = playlist_selection_.value_or(0);
            const size_t page = std::max<size_t>(1,
                static_cast<size_t>(geometry().visible_rows));
            if (wparam == VK_UP && target > 0) --target;
            else if (wparam == VK_DOWN && target + 1 < count) ++target;
            else if (wparam == VK_HOME) target = 0;
            else if (wparam == VK_END) target = count - 1;
            else if (wparam == VK_PRIOR)
                target = target > page ? target - page : 0;
            else if (wparam == VK_NEXT)
                target = std::min(count - 1, target + page);

            if (control && !shift) {
                // Native LVS_OWNERDATA Ctrl+navigation moves only the focus
                // caret. Existing selected rows and the range anchor remain.
                playlist_selection_ = target;
                if (!settings_.playlist.library_mode)
                    RememberPlaylistRow(playlists_.ActiveIndex(), target);
                EnsurePlaylistSelectionVisible();
                InvalidateRect(playlist_window_, nullptr, FALSE);
            } else {
                if (shift && !playlist_selection_anchor_)
                    playlist_selection_anchor_ = playlist_selection_.value_or(target);
                SelectPlaylistRow(target, control && shift, shift);
            }
        } else if (wparam == VK_SPACE && playlist_selection_) {
            // Ctrl+Space toggles the focused row without collapsing the rest;
            // ordinary Space selects it just like the native ListView.
            SelectPlaylistRow(*playlist_selection_, control, shift);
        } else if ((wparam == L'A' || wparam == L'a') && control) {
            playlist_selected_rows_.clear();
            for (size_t index = 0; index < VisiblePlaylistTrackCount(); ++index)
                playlist_selected_rows_.insert(index);
            InvalidateRect(playlist_window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (settings_.playlist.library_mode &&
            reinterpret_cast<HWND>(wparam) == playlist_tree_control_)
            return ShowMediaLibraryTreeContextMenu(point) ? 0 : 1;
        if (point.x == -1 && point.y == -1) {
            POINT caret{};
            const auto metrics = geometry();
            const HWND focus = GetFocus();
            if (focus == playlist_list_control_ && playlist_list_focus_ &&
                *playlist_list_focus_ < playlists_.Size()) {
                SendMessageW(playlist_list_control_, LVM_ENSUREVISIBLE,
                             *playlist_list_focus_, FALSE);
                caret = {metrics.list_titles.left + 8,
                    metrics.list_titles.top + static_cast<LONG>(
                        *playlist_list_focus_ - playlist_list_scroll_) *
                        metrics.row_height + metrics.row_height};
                ClientToScreen(playlist_window_, &caret);
                point = caret;
            } else if (focus == playlist_track_control_ &&
                       playlist_selection_) {
                SendMessageW(playlist_track_control_, LVM_ENSUREVISIBLE,
                             *playlist_selection_, FALSE);
                caret = {metrics.tracks.left + 8,
                    metrics.tracks.top + static_cast<LONG>(*playlist_selection_ - playlist_scroll_) *
                        metrics.row_height + metrics.row_height};
                ClientToScreen(playlist_window_, &caret);
                point = caret;
            } else {
                RECT bounds{};
                GetWindowRect(playlist_window_, &bounds);
                point = {bounds.left + 20, bounds.top + 20};
            }
        }
        POINT client = point;
        ScreenToClient(playlist_window_, &client);
        ShowPlaylistContextMenu(point, client);
        return 0;
    }
    case WM_DRAWITEM:
        if (lparam && DrawPopupMenuItem(
                *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_MEASUREITEM:
        if (lparam && MeasurePopupMenuItem(
                *reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_CAPTURECHANGED:
        playlist_rating_gesture_.Cancel();
        playlist_close_pressed_ = false;
        CancelPlaylistScrollbarInteraction(false);
        playlist_splitter_dragging_ = false;
        playlist_list_drag_pending_ = false;
        playlist_list_dragging_ = false;
        playlist_track_drag_pending_ = false;
        playlist_track_dragging_ = false;
        playlist_track_press_ctrl_ = false;
        playlist_track_press_shift_ = false;
        playlist_track_drag_row_.reset();
        playlist_track_drop_row_.reset();
        playlist_list_drag_row_.reset();
        playlist_list_drop_index_.reset();
        if (skin_drag_window_ == playlist_window_) {
            dragging_skin_background_ = false;
            skin_drag_window_ = nullptr;
            skin_drag_hit_ = 0;
            attached_drag_windows_.clear();
            return 0;
        }
        break;
    case WM_CANCELMODE:
        playlist_rating_gesture_.Cancel();
        playlist_close_pressed_ = false;
        CancelPlaylistScrollbarInteraction(true);
        playlist_splitter_dragging_ = false;
        playlist_list_drag_pending_ = false;
        playlist_list_dragging_ = false;
        playlist_track_drag_pending_ = false;
        playlist_track_dragging_ = false;
        playlist_track_press_ctrl_ = false;
        playlist_track_press_shift_ = false;
        playlist_track_drag_row_.reset();
        playlist_track_drop_row_.reset();
        playlist_list_drag_row_.reset();
        playlist_list_drop_index_.reset();
        if (skin_drag_window_ == playlist_window_) {
            EndSkinMouseCapture();
            return 0;
        }
        break;
    case WM_DESTROY:
        playlist_mouse_tracking_window_ = nullptr;
        playlist_toolbar_menu_return_point_.reset();
        CancelPlaylistScrollbarInteraction(false);
        RevokeFileDropTarget(playlist_window_);
        FinishPlaylistListEdit(false);
        if (skin_drag_window_ == playlist_window_) EndSkinMouseCapture();
        DestroyPlaylistListControls();
        DestroyPlaylistToolControls();
        RemoveToolTipTools(playlist_window_);
        if (playlist_tooltip_ && IsWindow(playlist_tooltip_))
            DestroyWindow(playlist_tooltip_);
        playlist_tooltip_ = nullptr;
        playlist_window_ = nullptr;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(playlist_window_, message, wparam, lparam);
}

bool PlayerWindow::CreatePlaylistWindow() {
    if (playlist_window_) return true;
    if (!skin_ || !skin_->Playlist().valid) return false;
    const auto& layout = skin_->Playlist();
    RECT main_bounds{};
    GetWindowRect(window_, &main_bounds);
    int x = main_bounds.left + layout.position.left;
    int y = main_bounds.top + layout.position.top;
    int width = layout.background.size.cx;
    int height = layout.background.size.cy;
    const RECT saved = settings_.player.playlist_window;
    if (saved.right > saved.left && saved.bottom > saved.top) {
        x = saved.left;
        y = saved.top;
        const bool resizable = layout.resize_rect.right > layout.resize_rect.left &&
                               layout.resize_rect.bottom > layout.resize_rect.top;
        if (resizable) {
            // FUN_0046BCBE/FUN_0048B8C7 use the background's native extent as
            // the minimum track size and retain only larger saved dimensions.
            width = std::max<int>(width, saved.right - saved.left);
            height = std::max<int>(height, saved.bottom - saved.top);
        }
    }
    if (width <= 0 || height <= 0) return false;
    playlist_window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kPlaylistWindowClass, L"PlayList",
        WS_POPUP, x, y, width, height, window_, nullptr, instance_, this);
    if (!playlist_window_) return false;
    RegisterFileDropTarget(playlist_window_, FileDropSurface::playlist);
    CreatePlaylistListControls();
    UpdatePlaylistWindowRegion();
    UpdatePlaylistToolRects();
    return true;
}

void PlayerWindow::TogglePlaylistWindow() {
    if (!playlist_window_ && !CreatePlaylistWindow()) return;
    CompleteSkinWindowFadeForReplacement();
    if (!window_ || !IsWindow(window_) || close_after_skin_window_fade_) return;
    const bool visible = IsWindowVisible(playlist_window_) != FALSE;
    // The ordinary command path passes a non--1 lParam to 004A3BBC and thus
    // enters 0046FA5D's 5-alpha Show/Hide transition.  The -1 bypass exists
    // only for internal structural visibility changes.
    SetSkinWindowVisible(playlist_window_, !visible);
    settings_.player.playlist_visible = !visible;
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void PlayerWindow::UpdatePlaylistWindowSkin(bool saved_bounds) {
    if (!skin_ || !skin_->Playlist().valid) {
        if (playlist_window_) ShowWindow(playlist_window_, SW_HIDE);
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (!playlist_window_ && !CreatePlaylistWindow()) return;
    const auto& layout = skin_->Playlist();
    ScopedSkinRedraw redraw(playlist_window_);
    const auto size = layout.background.size;
    if (size.cx > 0 && size.cy > 0) {
        RECT player{};
        GetWindowRect(window_, &player);
        int width = size.cx;
        int height = size.cy;
        const bool resizable = layout.resize_rect.right > layout.resize_rect.left &&
                               layout.resize_rect.bottom > layout.resize_rect.top;
        if (resizable) {
            width = std::max<int>(width, layout.position.right - layout.position.left);
            height = std::max<int>(height, layout.position.bottom - layout.position.top);
        }
        const RECT& saved = settings_.player.playlist_window;
        const bool have_saved = saved_bounds && saved.right > saved.left && saved.bottom > saved.top;
        if (have_saved && resizable) {
            width = std::max<int>(size.cx, saved.right - saved.left);
            height = std::max<int>(size.cy, saved.bottom - saved.top);
        }
        // CPlayerWnd's skin-rebuild path (00468363 -> 0046BCBE) passes the
        // skin's playlist_window.position translated by the newly sized main
        // HWND.  This is why selecting LX-iPlay places its 500x255 playlist at
        // (player.left, player.top + 50) instead of retaining the previous
        // detached skin's coordinates.
        SetWindowPos(playlist_window_, nullptr,
            have_saved ? saved.left : player.left + layout.position.left,
            have_saved ? saved.top : player.top + layout.position.top,
            width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    playlist_scroll_ = std::min(playlist_scroll_, VisiblePlaylistTrackCount());
    CreatePlaylistListControls();
    LayoutPlaylistListControls();
    UpdatePlaylistWindowRegion();
    UpdatePlaylistToolRects();
    redraw.Resume();
    RedrawWindow(playlist_window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    ShowWindow(playlist_window_, !mini_mode_ && settings_.player.playlist_visible
        ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void PlayerWindow::UpdatePlaylistWindowRegion() {
    if (!playlist_window_ || !skin_ || !skin_->Playlist().valid) return;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    const auto& layout = skin_->Playlist();
    HRGN region = CreateSkinWindowRegion(layout.background, layout.resize_rect,
        width, height, layout.resize_tile, skin_->TransparentColor());
    RECT bounds{};
    if (!region || GetRgnBox(region, &bounds) == NULLREGION ||
        !SetWindowRgn(playlist_window_, region, TRUE)) {
        if (region) DeleteObject(region);
    }
}

bool PlayerWindow::CreateToolTipWindow() {
    if (tooltip_ && IsWindow(tooltip_)) return true;
    if (!window_) return false;
    // FUN_0040EE16 passes both dwStyle and dwExStyle as zero.  The tooltip
    // class supplies its popup semantics itself; TTS_ALWAYSTIP/TTS_NOPREFIX
    // change inactive-window and ampersand behaviour and are not present in
    // TTPlayer 5.7.9.
    tooltip_ = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr, 0,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        window_, nullptr, instance_, nullptr);
    if (!tooltip_) return false;
    SendMessageW(tooltip_, TTM_ACTIVATE, TRUE, 0);
    // This is the shared skin-control tooltip, not Files' native infotip.
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 200);
    return true;
}

bool PlayerWindow::CreatePlaylistToolTipWindow() {
    if (playlist_tooltip_ && IsWindow(playlist_tooltip_)) return true;
    if (!playlist_window_) return false;
    // The native toolbar retains its own eight-tool tooltip. Runtime
    // enumeration shows the stock 0x84000000 style and a transparent class
    // window. FUN_0047E6FC nevertheless registers the visible toolbar text in
    // CPlayerWnd's shared tooltip; this companion remains unrelayed below.
    playlist_tooltip_ = CreateWindowExW(WS_EX_TRANSPARENT,
        TOOLTIPS_CLASSW, nullptr, 0,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        playlist_window_, nullptr, instance_, nullptr);
    if (!playlist_tooltip_) return false;
    SendMessageW(playlist_tooltip_, TTM_ACTIVATE, TRUE, 0);
    return true;
}

void PlayerWindow::CreatePlaylistListControls() {
    if (!playlist_window_) return;
    if (playlist_list_control_ && IsWindow(playlist_list_control_) &&
        playlist_track_control_ && IsWindow(playlist_track_control_)) {
        LayoutPlaylistListControls();
        return;
    }
    DestroyPlaylistListControls();

    // FUN_00482BAF creates LibraryTree 0x2800 first, followed by the visible
    // PlayLists 0x2801 and Files 0x2802 controls.  In the classic catalogue
    // mode the tree remains hidden; the two ListCtrl children use the exact
    // original styles (including TABSTOP and edit-label/list-view flags).
    // FUN_0048AA55 proves that the TreeCtrl wrapper creates a real
    // SysTreeView32.  Using the native class here restores TVM_* state,
    // expansion notifications, keyboard navigation and hit testing instead
    // of routing the tree through the generic painted-control procedure.
    playlist_tree_control_ = CreateWindowExW(0, WC_TREEVIEWW,
        L"LibraryTree", 0x40018413U, 0, 0, 0, 0, playlist_window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlaylistTreeId)),
        instance_, nullptr);
    playlist_list_control_ = CreateWindowExW(0, kPlaylistListClass,
        L"PlayLists", 0x50015205U, 0, 0, 0, 0, playlist_window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlaylistListId)),
        instance_, this);
    playlist_track_control_ = CreateWindowExW(0, kPlaylistListClass,
        L"Files", 0x50015001U, 0, 0, 0, 0, playlist_window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlaylistTrackId)),
        instance_, this);
    if (playlist_track_control_ && settings_.player.play_follow_cursor) {
        const LONG_PTR style = GetWindowLongPtrW(
            playlist_track_control_, GWL_STYLE);
        SetWindowLongPtrW(playlist_track_control_, GWL_STYLE,
                          style | LVS_SHOWSELALWAYS);
    }
    for (HWND control : {playlist_tree_control_, playlist_list_control_,
                         playlist_track_control_}) {
        if (control) SendMessageW(control, CCM_SETUNICODEFORMAT, TRUE, 0);
    }
    if (playlist_list_control_)
        SendMessageW(playlist_list_control_, LVM_SETEXTENDEDLISTVIEWSTYLE,
                     0, 0x4020);
    if (playlist_track_control_)
        SendMessageW(playlist_track_control_, LVM_SETEXTENDEDLISTVIEWSTYLE,
                     0, 0x4420);
    if (settings_.playlist.library_mode) {
        StartMediaLibraryRefresh();
        InitializeMediaLibraryTree();
    }
    playlist_list_selection_ = playlists_.Size()
        ? std::optional<size_t>{playlists_.ActiveIndex()} : std::nullopt;
    playlist_list_focus_ = playlist_list_selection_;
    LayoutPlaylistListControls();
    if (playlist_list_control_ && playlist_list_focus_)
        SendMessageW(playlist_list_control_, LVM_ENSUREVISIBLE,
                     *playlist_list_focus_, FALSE);
}

void PlayerWindow::DestroyPlaylistListControls() {
    if (playlist_track_control_) RemoveToolTipTools(playlist_track_control_);
    if (playlist_item_tooltip_ && IsWindow(playlist_item_tooltip_)) {
        DestroyWindow(playlist_item_tooltip_);
    }
    playlist_item_tooltip_ = nullptr;
    for (HWND* control : {&playlist_track_control_, &playlist_list_control_,
                          &playlist_tree_control_}) {
        if (*control && IsWindow(*control)) DestroyWindow(*control);
        *control = nullptr;
    }
    playlist_list_selection_.reset();
    playlist_list_focus_.reset();
    playlist_list_extended_style_ = 0;
    playlist_track_extended_style_ = 0;
    playlist_list_type_text_.clear();
    playlist_track_type_text_.clear();
    playlist_list_type_tick_ = 0;
    playlist_track_type_tick_ = 0;
}

void PlayerWindow::LayoutPlaylistListControls() {
    if (!playlist_window_ || !skin_ || !skin_->Playlist().valid) return;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        VisiblePlaylistTrackCount());
    const size_t visible = static_cast<size_t>(
        std::max(1, metrics.visible_rows));
    const size_t maximum = playlists_.Size() > visible
        ? playlists_.Size() - visible : 0;
    playlist_list_scroll_ = std::min(playlist_list_scroll_, maximum);
    const size_t track_count = VisiblePlaylistTrackCount();
    const size_t track_maximum = track_count > visible
        ? track_count - visible : 0;
    playlist_scroll_ = std::min(playlist_scroll_, track_maximum);
    const auto move = [](HWND control, const RECT& bounds) {
        if (!control) return;
        SetWindowPos(control, nullptr, bounds.left, bounds.top,
            std::max<LONG>(0, bounds.right - bounds.left),
            std::max<LONG>(0, bounds.bottom - bounds.top),
            SWP_NOACTIVATE | SWP_NOZORDER);
    };
    if (settings_.playlist.library_mode) {
        // 0047F766 swaps only the left-hand PlayLists control (+0x50c) for
        // LibraryTree (+0x4e8).  Files (+0x548) remains the right-hand result
        // list in both modes; stretching the tree over `metrics.list` hid the
        // result list and made the reconstructed media-library mode unusable.
        move(playlist_tree_control_, metrics.list_titles);
        move(playlist_track_control_, metrics.tracks);
        if (playlist_tree_control_) ShowWindow(playlist_tree_control_, SW_SHOW);
        if (playlist_list_control_) ShowWindow(playlist_list_control_, SW_HIDE);
        if (playlist_track_control_) ShowWindow(playlist_track_control_, SW_SHOW);
        return;
    }
    if (playlist_tree_control_) ShowWindow(playlist_tree_control_, SW_HIDE);
    if (playlist_list_control_) ShowWindow(playlist_list_control_, SW_SHOW);
    if (playlist_track_control_) ShowWindow(playlist_track_control_, SW_SHOW);
    move(playlist_list_control_, metrics.list_titles);
    move(playlist_track_control_, metrics.tracks);
    // FUN_00482BAF creates the dormant library tree at the catalogue origin
    // with its fixed 200 x 30 creation size.  The classic playlist path never
    // stretches this hidden child when the popup is resized; preserving that
    // otherwise-observable HWND rectangle matters to the private window ABI.
    if (playlist_tree_control_)
        SetWindowPos(playlist_tree_control_, nullptr, metrics.list.left,
            metrics.list.top, 200, 30,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
}

void PlayerWindow::RemoveToolTipTools(HWND owner) {
    if (!tooltip_ && !playlist_tooltip_ && !playlist_item_tooltip_) return;
    auto item = tooltip_tools_.begin();
    while (item != tooltip_tools_.end()) {
        if (item->owner != owner) {
            ++item;
            continue;
        }
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = item->owner;
        if (item->control) {
            tool.uFlags = TTF_IDISHWND;
            tool.uId = reinterpret_cast<UINT_PTR>(item->control);
        } else {
            tool.uId = item->identifier;
        }
        if (item->tooltip && IsWindow(item->tooltip))
            SendMessageW(item->tooltip, TTM_DELTOOLW, 0,
            reinterpret_cast<LPARAM>(&tool));
        item = tooltip_tools_.erase(item);
    }
}

void PlayerWindow::AddToolTipTool(HWND owner, UINT_PTR identifier,
                                  const RECT& bounds, HWND target) {
    if (!target) target = tooltip_;
    if (!target || !owner || bounds.right <= bounds.left ||
        bounds.bottom <= bounds.top) return;
    TOOLINFOW tool{sizeof(tool)};
    // FUN_004085C6 leaves explicit-rectangle tools at flags zero. The main
    // window message path supplies the framework-equivalent TTM_RELAYEVENT.
    tool.uFlags = 0;
    tool.hwnd = owner;
    tool.uId = identifier;
    tool.rect = bounds;
    tool.hinst = ResourceModule();
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    if (SendMessageW(target, TTM_ADDTOOLW, 0,
            reinterpret_cast<LPARAM>(&tool))) {
        tooltip_tools_.push_back({target, owner, nullptr, identifier, {}});
    }
}

bool PlayerWindow::PreTranslateMessage(const MSG& message) const {
    auto* queued = const_cast<MSG*>(&message);
    if (TranslatePlaylistConverterMessage(*queued)) return true;
    // PSH_MODELESS requires the application's message filter to run the
    // property-sheet dialog manager.  Omitting this was also enough to make
    // keyboard navigation appear hung while the audio/UI thread remained up.
    if (options_window_ && IsWindow(options_window_) &&
        PropSheet_IsDialogMessage(options_window_, queued)) {
        return true;
    }
    // 00449DA8 gives the active modeless Find/Replace dialog exclusive first
    // refusal.  Even an unconsumed message returns immediately from the
    // original filter instead of continuing through the editor accelerator.
    if (lyric_find_dialog_ && IsWindow(lyric_find_dialog_) &&
        GetActiveWindow() == lyric_find_dialog_) {
        return IsDialogMessageW(lyric_find_dialog_, queued) != FALSE;
    }
    if (lyric_editor_accelerators_ && lyric_editor_ &&
        IsWindow(lyric_editor_) && GetFocus() == lyric_editor_) {
        return TranslateAcceleratorW(window_, lyric_editor_accelerators_,
                                     queued) != FALSE;
    }
    // DAT_0053ABF8 is the 54-entry application/global shortcut table.  The
    // first default entry is F1 -> E140 (Options).  Application shortcuts are
    // translated from the thread's queued MSG; global variants arrive via
    // WM_HOTKEY after RegisterConfiguredHotKeys installs them.
    if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
        int modifiers{};
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) modifiers |= HOTKEYF_SHIFT;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            modifiers |= HOTKEYF_CONTROL;
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= HOTKEYF_ALT;
        for (const auto& binding : settings_.hotkey.key_map) {
            if (binding.command < 0 || binding.application.virtual_key == 0)
                continue;
            if (binding.application.virtual_key != static_cast<int>(message.wParam) ||
                (binding.application.modifiers &
                    (HOTKEYF_SHIFT | HOTKEYF_CONTROL | HOTKEYF_ALT)) != modifiers)
                continue;
            SendMessageW(window_, WM_COMMAND,
                         static_cast<WPARAM>(binding.command), 0);
            return true;
        }
    }
    for (const HWND target : {tooltip_, playlist_item_tooltip_}) {
        if (!target || !IsWindow(target)) continue;
        // Preserve hwnd/time/screen coordinates from the original queued MSG;
        // relaying synthesized child-to-parent messages would duplicate input.
        SendMessageW(target, TTM_RELAYEVENT, 0,
            reinterpret_cast<LPARAM>(queued));
    }
    return false;
}

void PlayerWindow::AddToolTipControl(HWND control, UINT_PTR identifier) {
    if (!control || !IsWindow(control)) return;
    const HWND owner = GetParent(control);
    const HWND target = tooltip_;
    if (!target) return;
    TOOLINFOW tool{sizeof(tool)};
    // FUN_004085C6 sees a zero explicit uId, changes hwnd to the child parent,
    // stores the child HWND in uId and ORs flag 1 (TTF_IDISHWND).
    // The original framework relays child mouse messages through its message
    // filter. The reconstructed child procedures do the same explicitly;
    // adding TTF_SUBCLASS here would relay every event a second time.
    tool.uFlags = TTF_IDISHWND;
    tool.hwnd = owner;
    tool.uId = reinterpret_cast<UINT_PTR>(control);
    tool.hinst = ResourceModule();
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    if (SendMessageW(target, TTM_ADDTOOLW, 0,
            reinterpret_cast<LPARAM>(&tool))) {
        tooltip_tools_.push_back({target, tool.hwnd, control, identifier, {}});
    }
}

void PlayerWindow::AddPlaylistToolTipControl(HWND control,
                                             UINT_PTR identifier,
                                             std::wstring text) {
    if (!playlist_tooltip_ || !tooltip_ || !control || !IsWindow(control) ||
        text.empty())
        return;
    const auto add = [this, control, identifier](HWND target,
                                                 std::wstring value) {
        tooltip_tools_.push_back({target, playlist_window_, control,
                                  identifier, std::move(value)});
        auto item = std::prev(tooltip_tools_.end());
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND;
        tool.hwnd = playlist_window_;
        tool.uId = reinterpret_cast<UINT_PTR>(control);
        tool.hinst = ResourceModule();
        // FUN_0047E6FC passes the GetMenuStringW buffer directly instead of
        // LPSTR_TEXTCALLBACK, which main/equalizer controls use.
        tool.lpszText = item->text.data();
        if (!SendMessageW(target, TTM_ADDTOOLW, 0,
                reinterpret_cast<LPARAM>(&tool)))
            tooltip_tools_.erase(item);
    };
    // The playlist-owned native toolbar tooltip retains the same eight tools,
    // while CPlayerWnd's shared tooltip is the one receiving relayed events.
    add(playlist_tooltip_, text);
    add(tooltip_, std::move(text));
}

void PlayerWindow::UpdateMainToolRects() {
    if (!window_ || !CreateToolTipWindow()) return;
    RemoveToolTipTools(window_);
    if (!skin_) return;

    std::set<UINT_PTR> registered;
    for (const auto& element : ActiveSkinElements()) {
        if (IsSuppressedSkinControl(element.name)) continue;
        UINT_PTR identifier{};
        if (element.name == L"pause" && FindActiveSkinElement(L"play")) continue;
        if (element.name == L"play" || element.name == L"pause") identifier = kCmdPlay;
        else if (element.name == L"stop") identifier = kCmdStopPlayback;
        else if (element.name == L"prev") identifier = kCmdPrevious;
        else if (element.name == L"next") identifier = kCmdNext;
        else if (element.name == L"mute") identifier = kCmdMute;
        else if (element.name == L"open") identifier = kCmdOpenFile;
        else if (element.name == L"lyric") identifier = kCmdShowLyrics;
        else if (element.name == L"equalizer") identifier = kCmdShowEqualizer;
        else if (element.name == L"playlist") identifier = kCmdShowPlaylist;
        else if (element.name == L"browser") identifier = kCmdShowBrowser;
        else if (element.name == L"set") identifier = kCmdOptions;
        else if (element.name.starts_with(L"mode_")) {
            constexpr std::wstring_view modes[] = {L"mode_single", L"mode_loop",
                L"mode_slider", L"mode_circle", L"mode_random"};
            const int mode = std::clamp(settings_.player.play_mode, 0, 4);
            if (element.name != modes[mode]) continue;
            identifier = kCmdPlayModeFirst + mode;
        }
        else if (element.name == L"exit") identifier = 8;
        else if (element.name == L"minimize") identifier = 0x7dd3;
        else if (element.name == L"minimode") identifier = kCmdMiniMode;
        else if (element.name == L"progress") identifier = 0x7dda;
        else if (element.name == L"volume") identifier = 0x7ddb;
        else if (element.name == L"icon") identifier = 0x7dd8;
        if (identifier && registered.insert(identifier).second)
            AddToolTipTool(window_, identifier, element.bounds);
    }
}

void PlayerWindow::DestroyPlaylistToolControls() {
    if (playlist_window_) RemoveToolTipTools(playlist_window_);
    for (const auto& [identifier, control] : playlist_tool_controls_) {
        static_cast<void>(identifier);
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    playlist_tool_controls_.clear();
}

void PlayerWindow::UpdatePlaylistToolRects() {
    if (!playlist_window_ || !CreateToolTipWindow() ||
        !CreatePlaylistToolTipWindow()) return;
    // 0047E6FC rebinds the existing toolbar. This function is also called
    // during WM_SIZE: recreating HWNDs here lost focus/capture on every resize.
    RemoveToolTipTools(playlist_window_);
    std::set<UINT_PTR> placed;
    const auto park_unused = [&] {
        for (const auto& [identifier, control] : playlist_tool_controls_) {
            if (!placed.contains(identifier))
                SetWindowPos(control, nullptr, -1000, -1000, 0, 0,
                             SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };
    if (!skin_ || !skin_->Playlist().valid) { park_unused(); return; }
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        VisiblePlaylistTrackCount());
    const auto add_control = [this, &placed](UINT_PTR identifier, UINT control_id,
                                    const RECT& bounds) {
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
        const auto found = std::find_if(playlist_tool_controls_.begin(), playlist_tool_controls_.end(),
            [identifier](const auto& entry) { return entry.first == identifier; });
        HWND control = found == playlist_tool_controls_.end() ? nullptr : found->second;
        if (!control) {
            control = CreateWindowExW(0, kEqualizerButtonClass, nullptr,
                WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, playlist_window_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance_, this);
            if (control) playlist_tool_controls_.emplace_back(identifier, control);
        }
        if (control) {
            placed.insert(identifier);
            SetWindowPos(control, nullptr, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            std::wstring text = identifier == kPlaylistToolFirst + 7
                ? ResourceText(8)
                : MenuPositionText(ResourceModule(), kMenuPlaylistToolbar,
                    static_cast<UINT>(identifier - kPlaylistToolFirst));
            AddPlaylistToolTipControl(control, identifier, std::move(text));
        } else {
            AddToolTipTool(playlist_window_, identifier, bounds);
        }
    };
    if (skin_->Playlist().close.image)
        add_control(kPlaylistToolFirst + 7, kCmdShowPlaylist, metrics.close);
    UpdatePlaylistItemTipRects();
    if (!skin_->Playlist().toolbar.image ||
        metrics.toolbar.right <= metrics.toolbar.left) { park_unused(); return; }
    constexpr int count = 7;
    for (int index = 0; index < count; ++index) {
        const RECT bounds = PlaylistToolbarItemBounds(skin_->Playlist(), metrics.toolbar, index);
        add_control(kPlaylistToolFirst + index,
            static_cast<UINT>(kPlaylistToolFirst + index), bounds);
    }
    park_unused();
}

void PlayerWindow::UpdatePlaylistItemTipRects() {
    if (!playlist_window_) return;
    if (tooltip_ || playlist_item_tooltip_) {
        auto item = tooltip_tools_.begin();
        while (item != tooltip_tools_.end()) {
            const bool playlist_item = (item->owner == playlist_window_ ||
                                       item->owner == playlist_track_control_) &&
                !item->control && item->identifier >= kCmdFirstTrack &&
                item->identifier < kPlaylistToolFirst;
            if (!playlist_item) {
                ++item;
                continue;
            }
            TOOLINFOW tool{sizeof(tool)};
            tool.hwnd = item->owner;
            tool.uId = item->identifier;
            if (item->tooltip && IsWindow(item->tooltip)) {
                SendMessageW(item->tooltip, TTM_DELTOOLW, 0,
                             reinterpret_cast<LPARAM>(&tool));
            }
            item = tooltip_tools_.erase(item);
        }
    }
    if (!skin_ || !skin_->Playlist().valid) return;

    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        VisiblePlaylistTrackCount());
    if (!settings_.playlist.library_mode &&
        settings_.playlist.read_info_mode != 2 && metrics.visible_rows > 0) {
        // FUN_00487C0D queues an unread (-2) non-network CPlayItem only when
        // custom drawing reaches a visible row.
        QueuePlaylistInfoRange(playlists_.ActiveIndex(), playlist_scroll_,
            static_cast<size_t>(metrics.visible_rows));
    }
    if (!settings_.playlist.item_tips || !playlist_track_control_) return;
    if (!playlist_item_tooltip_ || !IsWindow(playlist_item_tooltip_)) {
        // Files is owner-data in 00482BAF (LVS_EX_INFOTIP, 0x4420), and
        // 004887AA handles its LVN_GETINFOTIPW. Our painted ListCtrl needs
        // an equivalent tooltip owned by Files, not tools on its parent:
        // the queued mouse MSG names Files and uses Files client coordinates.
        // Native common-controls v6 supplies NOPREFIX/USEVISUALSTYLE and a
        // 400-pixel infotip width; leave delay times at their system defaults.
        playlist_item_tooltip_ = CreateWindowExW(WS_EX_TRANSPARENT,
            TOOLTIPS_CLASSW, nullptr, TTS_NOPREFIX | TTS_USEVISUALSTYLE,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            playlist_track_control_, nullptr, instance_, nullptr);
        if (!playlist_item_tooltip_) return;
        SendMessageW(playlist_item_tooltip_, TTM_SETMAXTIPWIDTH, 0, 400);
        SendMessageW(playlist_item_tooltip_, TTM_ACTIVATE, TRUE, 0);
    }
    for (int row = 0; row < metrics.visible_rows; ++row) {
        const size_t index = playlist_scroll_ + static_cast<size_t>(row);
        if (index >= VisiblePlaylistTrackCount()) break;
        RECT bounds{metrics.tracks.left,
                    metrics.tracks.top + row * metrics.row_height,
                    metrics.tracks.right,
                    std::min(metrics.tracks.bottom,
                        metrics.tracks.top + (row + 1) * metrics.row_height)};
        MapWindowPoints(playlist_window_, playlist_track_control_,
                        reinterpret_cast<POINT*>(&bounds), 2);
        AddToolTipTool(playlist_track_control_, kCmdFirstTrack + index, bounds,
                      playlist_item_tooltip_);
    }
}

bool PlayerWindow::HandleToolTipNotification(HWND owner, LPARAM notification) {
    if (!notification) return false;
    auto* header = reinterpret_cast<NMHDR*>(notification);
    const HWND lyric_editor_tooltip = lyric_editor_toolbar_
        ? reinterpret_cast<HWND>(SendMessageW(
              lyric_editor_toolbar_, TB_GETTOOLTIPS, 0, 0))
        : nullptr;
    if ((header->hwndFrom != tooltip_ &&
         header->hwndFrom != playlist_tooltip_ &&
         header->hwndFrom != playlist_item_tooltip_ &&
         header->hwndFrom != lyric_editor_tooltip) ||
        (header->code != TTN_GETDISPINFOW &&
         header->code != TTN_GETDISPINFOA)) return false;
    // The notification is delivered to the tooltip control's owner (the main
    // player).  Original SkinButton/SkinSlider tools report their HWND in
    // idFrom because FUN_004085C6 sets TTF_IDISHWND.
    UINT_PTR identifier = header->idFrom;
    if (header->hwndFrom == lyric_editor_tooltip)
        owner = lyric_window_;
    if (const auto item = std::find_if(tooltip_tools_.begin(),
            tooltip_tools_.end(), [header](const auto& tool) {
                if (tool.tooltip != header->hwndFrom) return false;
                return tool.control
                    ? reinterpret_cast<UINT_PTR>(tool.control) == header->idFrom
                    : tool.identifier == header->idFrom;
            }); item != tooltip_tools_.end()) {
        owner = item->owner;
        identifier = item->identifier;
    }
    if (owner == playlist_track_control_) owner = playlist_window_;
    if (!settings_.playlist.library_mode && owner == playlist_window_ &&
        identifier >= kCmdFirstTrack &&
        identifier < kPlaylistToolFirst) {
        // LVN_GETINFOTIPW at 004887AA explicitly resolves just the requested
        // item when it has not yet passed through the visible-row queue.
        RequestPlaylistTrackInfo(playlists_.ActiveIndex(),
            static_cast<size_t>(identifier - kCmdFirstTrack), true);
    }
    tooltip_text_ = ToolTipText(owner, identifier);
    const bool playlist_item_tip = owner == playlist_window_ &&
        identifier >= kCmdFirstTrack && identifier < kPlaylistToolFirst;
    // Main/skin command tips follow FUN_0046AC04's fixed 0x50-WCHAR copy.
    // Files item tips instead originate at LVN_GETINFOTIPW (004887AA), where
    // the ListView provides a larger caller buffer.  Our owner-drawn list has
    // no such notification, so keep the equivalent full text in a stable
    // member and return its pointer to the tooltip control.
    if (header->code == TTN_GETDISPINFOW) {
        auto* display = reinterpret_cast<NMTTDISPINFOW*>(notification);
        if (playlist_item_tip) {
            display->lpszText = tooltip_text_.data();
        } else {
            wcsncpy_s(display->szText, tooltip_text_.c_str(), _TRUNCATE);
            display->lpszText = display->szText;
        }
    } else {
        auto* display = reinterpret_cast<NMTTDISPINFOA*>(notification);
        if (playlist_item_tip) {
            const int length = WideCharToMultiByte(CP_ACP, 0,
                tooltip_text_.c_str(), -1, nullptr, 0, nullptr, nullptr);
            tooltip_text_ansi_.assign(length > 0
                ? static_cast<size_t>(length) : 1, '\0');
            if (length > 0) {
                WideCharToMultiByte(CP_ACP, 0, tooltip_text_.c_str(), -1,
                    tooltip_text_ansi_.data(), length, nullptr, nullptr);
            }
            display->lpszText = tooltip_text_ansi_.data();
        } else {
            WideCharToMultiByte(CP_ACP, 0, tooltip_text_.c_str(), -1,
                display->szText, static_cast<int>(std::size(display->szText)),
                nullptr, nullptr);
            display->szText[std::size(display->szText) - 1] = '\0';
            display->lpszText = display->szText;
        }
    }
    return true;
}

std::wstring PlayerWindow::ToolTipText(HWND owner, UINT_PTR tool) const {
    if (owner == equalizer_window_) return EqualizerToolText(tool);
    if (owner == lyric_window_) {
        if (tool == kCmdShowLyrics) return ResourceText(8);
        return ToolTipWithHotKey(static_cast<UINT>(tool),
            ResourceCommandLabel(ResourceModule(), static_cast<UINT>(tool)));
    }
    if (owner == playlist_window_) {
        if (tool >= kCmdFirstTrack && tool < kPlaylistToolFirst) {
            const size_t row = static_cast<size_t>(tool - kCmdFirstTrack);
            const auto* visible_track = VisiblePlaylistTrack(row);
            if (!settings_.playlist.item_tips || !visible_track) return {};
            const auto& track = *visible_track;
            const bool network = LooksLikeUrl(track.path.wstring());
            return FormatPlaylistItemTip(track,
                ResourceText(network ? 0x8296 : 0x81c9),
                ResourceText(0x8297), ResourceText(0x828e));
        }
        if (tool == kPlaylistToolFirst + 7) return ResourceText(8);
        if (tool >= kPlaylistToolFirst && tool < kPlaylistToolFirst + 7)
            return MenuPositionText(ResourceModule(), kMenuPlaylistToolbar,
                static_cast<UINT>(tool - kPlaylistToolFirst));
        return {};
    }
    if (owner != window_) return {};
    if (const auto skin_text = SkinMenuToolTipText(static_cast<UINT>(tool));
        !skin_text.empty()) return skin_text;
    if (tool == kCmdPlay) {
        const auto state = audio_.State();
        const bool playing = state == audio::PlaybackState::playing;
        if (!IsSkinElementEnabled(playing ? L"pause" : L"play")) return {};
        const UINT command = playing ? kCmdPause : kCmdPlay;
        return ToolTipWithHotKey(command,
            ResourceCommandLabel(ResourceModule(), command));
    }
    if (tool == kCmdPrevious && !IsSkinElementEnabled(L"prev")) return {};
    if (tool == kCmdNext && !IsSkinElementEnabled(L"next")) return {};
    if (tool == 0x7dda && !IsSkinElementEnabled(L"progress")) return {};
    if (tool == kCmdMiniMode)
        return ToolTipWithHotKey(kCmdMiniMode,
            ResourceListItem(ResourceModule(), 0x81c2, mini_mode_ ? 0 : 1));
    if (tool == 0x7ddb) {
        const auto format = ResourceText(0x81bc);
        wchar_t text[128]{};
        if (!format.empty()) swprintf_s(text, format.c_str(),
            std::clamp(settings_.player.volume, 0, 100));
        return text;
    }
    if (tool == 8) return ResourceText(8);
    return ToolTipWithHotKey(static_cast<UINT>(tool),
        ResourceCommandLabel(ResourceModule(), static_cast<UINT>(tool)));
}

std::optional<size_t> PlayerWindow::PlaylistTrackAt(POINT point) const {
    if (!skin_ || !skin_->Playlist().valid) return std::nullopt;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        VisiblePlaylistTrackCount());
    if (!PtInRect(&metrics.tracks, point)) return std::nullopt;
    const size_t index = playlist_scroll_ +
        static_cast<size_t>((point.y - metrics.tracks.top) / metrics.row_height);
    return index < VisiblePlaylistTrackCount() ? std::optional<size_t>{index}
                                                : std::nullopt;
}

std::optional<size_t> PlayerWindow::PlaylistListAt(POINT point) const {
    if (settings_.playlist.library_mode || !skin_ ||
        !skin_->Playlist().valid) return std::nullopt;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        VisiblePlaylistTrackCount());
    if (!PtInRect(&metrics.list_titles, point)) return std::nullopt;
    const size_t index = playlist_list_scroll_ + static_cast<size_t>(
        (point.y - metrics.list_titles.top) / metrics.row_height);
    return index < playlists_.Size() ? std::optional<size_t>{index} : std::nullopt;
}

std::optional<size_t> PlayerWindow::PlaylistListInsertionAt(POINT point) const {
    if (settings_.playlist.library_mode || !skin_ ||
        !skin_->Playlist().valid) return std::nullopt;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        VisiblePlaylistTrackCount());
    if (!PtInRect(&metrics.list_titles, point) || metrics.row_height <= 0)
        return std::nullopt;

    // FUN_0041CB83 accepts the row under the pointer and one synthetic row
    // immediately below the final item.  Empty space any farther down is not
    // a drop target.  The returned value is an insertion position [0, size].
    const size_t insertion = playlist_list_scroll_ + static_cast<size_t>(
        (point.y - metrics.list_titles.top) / metrics.row_height);
    return insertion <= playlists_.Size()
        ? std::optional<size_t>{insertion} : std::nullopt;
}

std::optional<size_t> PlayerWindow::PlaylistToolbarButtonAt(POINT point) const {
    if (!skin_ || !skin_->Playlist().toolbar.image) return std::nullopt;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        VisiblePlaylistTrackCount());
    if (!PtInRect(&metrics.toolbar, point)) return std::nullopt;
    if (skin_->Playlist().toolbar_items) {
        for (size_t index = 0; index < 7; ++index) {
            const RECT item = PlaylistToolbarItemBounds(skin_->Playlist(), metrics.toolbar, index);
            if (PtInRect(&item, point)) return index;
        }
        return std::nullopt;
    }
    constexpr size_t button_count = 7;
    const int width = metrics.toolbar.right - metrics.toolbar.left;
    if (width <= 0) return std::nullopt;
    const size_t index = std::min(button_count - 1,
        static_cast<size_t>((point.x - metrics.toolbar.left) *
                            static_cast<int>(button_count) / width));
    // FUN_00482BAF gives the native toolbar a 0x1e-pixel row.  Tall toolbar
    // skins use the lower portion for the fifth (quick-find) control while
    // the other six buttons remain in the upper row.
    if (metrics.toolbar.bottom - metrics.toolbar.top > 0x1e) {
        const bool lower_row = point.y >= metrics.toolbar.top + 0x1e;
        if ((index == 4) != lower_row) return std::nullopt;
    }
    return index;
}

void PlayerWindow::RememberPlaylistRow(size_t playlist_index,
                                       std::optional<size_t> row) {
    if (playlist_index < playlists_.Size())
        playlists_.At(playlist_index).SetCurrentRow(row);
}

void PlayerWindow::SelectPlaylistRow(size_t index, bool toggle, bool extend,
                                     PlaylistSelectionTrigger trigger) {
    if (index >= VisiblePlaylistTrackCount()) return;
    if (extend && playlist_selection_anchor_) {
        const auto first = std::min(index, *playlist_selection_anchor_);
        const auto last = std::max(index, *playlist_selection_anchor_);
        if (!toggle) playlist_selected_rows_.clear();
        for (size_t selected = first; selected <= last; ++selected)
            playlist_selected_rows_.insert(selected);
    } else if (toggle) {
        if (playlist_selected_rows_.contains(index)) playlist_selected_rows_.erase(index);
        else playlist_selected_rows_.insert(index);
        playlist_selection_anchor_ = index;
    } else {
        playlist_selected_rows_.clear();
        playlist_selected_rows_.insert(index);
        playlist_selection_anchor_ = index;
    }
    playlist_selection_ = index;
    if (!settings_.playlist.library_mode)
        RememberPlaylistRow(playlists_.ActiveIndex(), playlist_selection_);
    EnsurePlaylistSelectionVisible();
    if (playlist_view_) SendMessageW(playlist_view_, LB_SETCURSEL, index, 0);
    if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
    if (ShouldStartPlaylistPlayback(
            trigger, settings_.player.play_follow_cursor)) {
        if (settings_.playlist.library_mode)
            ActivateMediaLibraryResult(index, true);
        else
            SelectTrack(index, true);
    }
}

void PlayerWindow::RestorePlaylistRowSelection() {
    // 0047F294 treats the transient +0x20 caret and persisted +0x1c playing
    // marker differently.  A live caret is restored as selected/focused by
    // FUN_0047FC71.  Without one, the native code clears LVIS state and only
    // sends LVM_ENSUREVISIBLE for +0x1c; it does not select that row.
    const auto caret = ActivePlaylist().CurrentRow();
    const auto visible = caret ? caret : ActivePlaylist().PlayingRow();
    if (!visible || *visible >= ActivePlaylist().Tracks().size()) return;
    if (!caret) {
        if (!playlist_window_ || !skin_) return;
        RECT client{};
        GetClientRect(playlist_window_, &client);
        const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
            settings_.playlist.split_on_lists, client.right, client.bottom,
            ActivePlaylist().Tracks().size());
        if (*visible < playlist_scroll_) playlist_scroll_ = *visible;
        const size_t rows = static_cast<size_t>(metrics.visible_rows);
        if (*visible >= playlist_scroll_ + rows)
            playlist_scroll_ = *visible - rows + 1;
        return;
    }
    // CPlayListWnd's active-list transition (0047F294) restores the native
    // ListCtrl caret/selection directly.  It does not travel through the
    // user-selection command and therefore must not honor PlayFollowCursor.
    playlist_selection_ = *caret;
    playlist_selection_anchor_ = *caret;
    playlist_selected_rows_.clear();
    playlist_selected_rows_.insert(*caret);
    EnsurePlaylistSelectionVisible();
    if (playlist_view_) SendMessageW(playlist_view_, LB_SETCURSEL, *caret, 0);
    if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
}

void PlayerWindow::EnsurePlaylistSelectionVisible() {
    if (!playlist_selection_ || !skin_) return;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        VisiblePlaylistTrackCount());
    const size_t previous = playlist_scroll_;
    if (*playlist_selection_ < playlist_scroll_) playlist_scroll_ = *playlist_selection_;
    const size_t visible = static_cast<size_t>(metrics.visible_rows);
    if (*playlist_selection_ >= playlist_scroll_ + visible)
        playlist_scroll_ = *playlist_selection_ - visible + 1;
    if (playlist_scroll_ != previous) UpdatePlaylistItemTipRects();
}

void PlayerWindow::SwitchPlaylist(size_t index) {
    if (index >= playlists_.Size()) return;
    // Selecting the already-active catalogue row is still a native selection
    // transition after a blank-area click.  FUN_00489AE2 skips only the
    // expensive active-playlist switch; it does not undo ListCtrl state.
    playlist_list_selection_ = index;
    playlist_list_focus_ = index;
    if (playlist_list_control_)
        SendMessageW(playlist_list_control_, LVM_ENSUREVISIBLE, index, FALSE);
    if (!playlists_.SetActive(index)) {
        if (playlist_window_)
            InvalidateRect(playlist_window_, nullptr, FALSE);
        return;
    }
    settings_.player.active_playlist = static_cast<int>(playlists_.ActiveSlot());
    playlist_selection_.reset();
    playlist_selection_anchor_.reset();
    playlist_selected_rows_.clear();
    playlist_scroll_ = 0;
    RestorePlaylistRowSelection();
    LayoutPlaylistListControls();
    RefreshPlaylist();
}

void PlayerWindow::ActivatePlaylistCatalogueRow(size_t index) {
    if (index >= playlists_.Size()) return;

    // PlayLists/LVN_ITEMACTIVATE is routed through FUN_00489A9D: first make
    // the activated catalogue row current (FUN_0047F294), then pass the
    // restored Files caret through the same activation path as a Files
    // double-click (FUN_0047FEA3).  Merely changing catalogue selection still
    // uses SwitchPlaylist and therefore never starts playback.
    SwitchPlaylist(index);
    if (!playlist_selection_ ||
        *playlist_selection_ >= VisiblePlaylistTrackCount()) return;
    SelectPlaylistRow(*playlist_selection_, false, false,
        PlaylistSelectionTrigger::item_activated);
}

void PlayerWindow::BeginPlaylistListEdit(size_t index) {
    if (!playlist_window_ || !skin_ || index >= playlists_.Size()) return;
    FinishPlaylistListEdit(false);
    if (playlist_list_control_)
        SendMessageW(playlist_list_control_, LVM_ENSUREVISIBLE, index, FALSE);

    playlist_list_edit_index_ = index;
    playlist_list_edit_ = CreateWindowExW(0, WC_EDITW,
        playlists_.At(index).Title().c_str(),
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_LEFT | ES_AUTOHSCROLL,
        0, 0, 0, 0, playlist_window_, nullptr, instance_, nullptr);
    if (!playlist_list_edit_) {
        playlist_list_edit_index_.reset();
        return;
    }

    NONCLIENTMETRICSW nonclient{sizeof(nonclient)};
    LOGFONTW descriptor{};
    if (settings_.playlist.font_descriptor_valid) {
        descriptor = settings_.playlist.font_descriptor;
    } else {
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 0, &nonclient, 0))
            descriptor = nonclient.lfMessageFont;
        descriptor.lfHeight = settings_.playlist.font_height;
        descriptor.lfWeight = FW_NORMAL;
        descriptor.lfQuality = ANTIALIASED_QUALITY;
        wcsncpy_s(descriptor.lfFaceName, settings_.playlist.font.c_str(),
                  _TRUNCATE);
    }
    playlist_list_edit_font_ = CreateFontIndirectW(&descriptor);
    if (playlist_list_edit_font_)
        SetControlFont(playlist_list_edit_, playlist_list_edit_font_);
    SetWindowSubclass(playlist_list_edit_, PlaylistEditProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    LayoutPlaylistListEdit();
    SendMessageW(playlist_list_edit_, EM_SETSEL, 0, -1);
    SetFocus(playlist_list_edit_);
    InvalidateRect(playlist_window_, nullptr, FALSE);
}

void PlayerWindow::LayoutPlaylistListEdit() {
    if (!playlist_list_edit_ || !playlist_list_edit_index_ || !skin_ ||
        !playlist_window_) return;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        ActivePlaylist().Tracks().size());
    if (*playlist_list_edit_index_ < playlist_list_scroll_) {
        ShowWindow(playlist_list_edit_, SW_HIDE);
        return;
    }
    const LONG top = metrics.list_titles.top + static_cast<LONG>(
        *playlist_list_edit_index_ - playlist_list_scroll_) *
        metrics.row_height;
    if (top < metrics.list_titles.top || top >= metrics.list_titles.bottom) {
        ShowWindow(playlist_list_edit_, SW_HIDE);
        return;
    }
    SIZE extent{};
    const int length = GetWindowTextLengthW(playlist_list_edit_);
    std::wstring title(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
    if (length > 0) GetWindowTextW(playlist_list_edit_, title.data(), length + 1);
    const HDC dc = GetDC(playlist_list_edit_);
    const HGDIOBJ old_font = dc && playlist_list_edit_font_
        ? SelectObject(dc, playlist_list_edit_font_) : nullptr;
    if (dc && length > 0)
        GetTextExtentPoint32W(dc, title.c_str(), length, &extent);
    if (dc && playlist_list_edit_font_) SelectObject(dc, old_font);
    if (dc) ReleaseDC(playlist_list_edit_, dc);
    const LONG left = metrics.list_titles.left + 5;
    const LONG right = std::min<LONG>(metrics.list_titles.right,
        left + std::max<LONG>(14, extent.cx + 14));
    const RECT edit{left, top, right,
                    std::min(metrics.list_titles.bottom,
                             top + metrics.row_height)};
    SetWindowPos(playlist_list_edit_, HWND_TOP, edit.left, edit.top,
        std::max<LONG>(1, edit.right - edit.left),
        std::max<LONG>(1, edit.bottom - edit.top),
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PlayerWindow::FinishPlaylistListEdit(bool commit) {
    if (!playlist_list_edit_ || playlist_list_edit_finishing_) return;
    playlist_list_edit_finishing_ = true;
    const HWND edit = playlist_list_edit_;
    const auto index = playlist_list_edit_index_;
    std::wstring title;
    if (commit) {
        const int length = GetWindowTextLengthW(edit);
        if (length > 0) {
            title.resize(static_cast<size_t>(length) + 1);
            const int copied = GetWindowTextW(edit, title.data(), length + 1);
            title.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
        }
    }
    RemoveWindowSubclass(edit, PlaylistEditProc, 1);
    playlist_list_edit_ = nullptr;
    playlist_list_edit_index_.reset();
    DestroyWindow(edit);
    if (playlist_list_edit_font_) {
        DeleteObject(playlist_list_edit_font_);
        playlist_list_edit_font_ = nullptr;
    }
    playlist_list_edit_finishing_ = false;

    // LVN_ENDLABELEDITW at 00489B06 rejects Escape (null pszText) and an
    // empty label, but assigns every other string verbatim.
    if (commit && index && !title.empty())
        playlists_.Rename(*index, std::move(title));
    if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
}

void PlayerWindow::CancelPlaylistScrollbarInteraction(bool release_capture) {
    const bool changed = playlist_scrollbar_dragging_ ||
        playlist_scrollbar_pressed_ != PlaylistScrollbarPart::none ||
        playlist_scrollbar_hover_ != PlaylistScrollbarPart::none;
    if (playlist_window_)
        KillTimer(playlist_window_, kPlaylistScrollbarRepeatTimer);
    playlist_scrollbar_dragging_ = false;
    playlist_scrollbar_pressed_ = PlaylistScrollbarPart::none;
    playlist_scrollbar_hover_ = PlaylistScrollbarPart::none;
    playlist_scrollbar_repeat_fast_ = false;
    if (release_capture && playlist_window_ &&
        GetCapture() == playlist_window_) {
        ReleaseCapture();
    }
    if (changed && playlist_window_)
        InvalidateRect(playlist_window_, nullptr, FALSE);
}

void PlayerWindow::ScrollPlaylist(int rows) {
    if (!skin_) return;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        VisiblePlaylistTrackCount());
    const size_t visible = static_cast<size_t>(
        std::max(1, metrics.visible_rows));
    const size_t count = VisiblePlaylistTrackCount();
    const size_t maximum = count > visible ? count - visible : 0;
    const auto next = static_cast<long long>(playlist_scroll_) + rows;
    const size_t previous = playlist_scroll_;
    playlist_scroll_ = static_cast<size_t>(std::clamp<long long>(next, 0, maximum));
    if (playlist_scroll_ != previous) UpdatePlaylistItemTipRects();
    if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
}

void PlayerWindow::PaintPlaylist(HDC dc) const {
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) return;
    const HDC canvas = CreateCompatibleDC(dc);
    const HBITMAP buffer = CreateCompatibleBitmap(dc, width, height);
    const HGDIOBJ old_buffer = SelectObject(canvas, buffer);
    FillRect(canvas, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    if (!skin_ || !skin_->Playlist().valid) {
        BitBlt(dc, 0, 0, width, height, canvas, 0, 0, SRCCOPY);
        SelectObject(canvas, old_buffer);
        DeleteObject(buffer);
        DeleteDC(canvas);
        return;
    }
    const auto& layout = skin_->Playlist();
    if (layout.background.image) {
        if (width == layout.background.size.cx && height == layout.background.size.cy) {
            layout.background.image.Draw(canvas, 0, 0, width, height,
                                         0, 0, width, height);
        } else {
            DrawResizableSkinBitmap(canvas, layout.background, layout.resize_rect,
                                    width, height, layout.resize_tile);
        }
    }

    const auto metrics = MakePlaylistGeometry(layout, settings_.playlist.split_on_lists,
                                               width, height,
                                               VisiblePlaylistTrackCount());
    if (metrics.list.right > metrics.list.left && metrics.list.bottom > metrics.list.top) {
        const RECT group = metrics.list_titles;
        // The left TreeCtrl and the track ListCtrl both receive Color_Bkgnd.
        // Color_Bkgnd2 is only the odd-row fill in the track control.
        const HBRUSH group_brush = CreateSolidBrush(settings_.playlist.background_color);
        FillRect(canvas, &group, group_brush);
        DeleteObject(group_brush);
        const RECT track_surface = metrics.tracks;
        const HBRUSH track_brush = CreateSolidBrush(settings_.playlist.background_color);
        FillRect(canvas, &track_surface, track_brush);
        DeleteObject(track_brush);
        SetBkMode(canvas, TRANSPARENT);
        SetTextColor(canvas, settings_.playlist.text_color);
        const HFONT font = CreatePlaylistFont(settings_.playlist);
        const HGDIOBJ old_font = font ? SelectObject(canvas, font) : nullptr;
        const int number_column_width = PlaylistNumberColumnWidth(canvas,
            VisiblePlaylistTrackCount(), settings_.playlist.title_number);
        const std::wstring unknown_title_format = ResourceText(0x81c8);
        for (int visible_row = 0; !settings_.playlist.library_mode &&
             visible_row < metrics.visible_rows; ++visible_row) {
            const size_t index = playlist_list_scroll_ +
                static_cast<size_t>(visible_row);
            if (index >= playlists_.Entries().size()) break;
            RECT row_bounds{group.left,
                group.top + static_cast<LONG>(visible_row) * metrics.row_height,
                group.right,
                std::min(group.bottom, group.top +
                    static_cast<LONG>(visible_row + 1) * metrics.row_height)};
            if (row_bounds.top >= group.bottom) break;
            const bool control_focused = GetFocus() == playlist_list_control_;
            const bool selected = playlist_list_selection_ == index &&
                                  control_focused;
            const bool focused = playlist_list_focus_ == index &&
                                 control_focused;
            if (selected && layout.selected.image) {
                TileBitmap(canvas, layout.selected, row_bounds);
            } else if (selected) {
                // 004897C1 uses the same selected-image/vertical-gradient
                // branch as the Files ListCtrl.  LVS_SHOWSELALWAYS is absent,
                // so it is visible only while PlayLists owns focus.
                DrawVerticalGradient(canvas, row_bounds,
                    settings_.playlist.selected_color,
                    settings_.playlist.background_color);
            } else if ((index & 1U) != 0) {
                const HBRUSH brush = CreateSolidBrush(
                    settings_.playlist.alternate_background_color);
                FillRect(canvas, &row_bounds, brush);
                DeleteObject(brush);
            }
            if (playlist_list_edit_ && playlist_list_edit_index_ == index) continue;
            COLORREF color = index == playlists_.ActiveIndex()
                ? settings_.playlist.highlight_color : settings_.playlist.text_color;
            if (selected) {
                color = layout.selected_text_color.value_or(GetSysColor(COLOR_HIGHLIGHTTEXT));
                if (color == settings_.playlist.selected_color)
                    color = settings_.playlist.highlight_color;
            }
            SetTextColor(canvas, color);
            RECT group_text = row_bounds;
            InflateRect(&group_text, -2, 0);
            DrawTextW(canvas, playlists_.Entries()[index].playlist.Title().c_str(), -1,
                      &group_text, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            if (focused) DrawSolidFrame(canvas, row_bounds, color);
        }

        if (playlist_list_dragging_ && playlist_list_drop_index_ &&
            !settings_.playlist.library_mode) {
            const auto relative = static_cast<std::int64_t>(
                *playlist_list_drop_index_) -
                static_cast<std::int64_t>(playlist_list_scroll_);
            const LONG y = group.top + static_cast<LONG>(
                relative * metrics.row_height);
            if (y >= group.top && y < group.bottom) {
                // FUN_0041CFD2 paints a two-pixel insertion mark with
                // InvertRect, so it remains visible on every skin colour.
                RECT insertion_line{group.left, y, group.right,
                    std::min<LONG>(group.bottom, y + 2)};
                InvertRect(canvas, &insertion_line);
            }
        }

        if (layout.splitter_bar.image) {
            TileBitmap(canvas, layout.splitter_bar, metrics.splitter);
            if (layout.splitter_arrow.image) {
                // The 14x7 legacy asset is a two-frame strip (collapsed /
                // expanded), not a single 14-pixel glyph.
                const int arrow_width = std::max(1L, layout.splitter_arrow.size.cx / 2);
                const int arrow_y = metrics.list.top + std::max(0L, (metrics.list.bottom - metrics.list.top -
                                                             layout.splitter_arrow.size.cy) / 2);
                layout.splitter_arrow.image.Draw(canvas,
                    metrics.splitter.right - arrow_width, arrow_y,
                    arrow_width, layout.splitter_arrow.size.cy, 0, 0,
                    arrow_width, layout.splitter_arrow.size.cy, skin_->TransparentColor());
            }
        } else {
            // Default SplitterCtrl paint: Color_Text fades toward
            // Color_Bkgnd, with the text color repeated on the far edge.
            DrawHorizontalGradient(canvas, metrics.splitter,
                settings_.playlist.text_color,
                settings_.playlist.background_color);
        }

        for (int row = 0; row < metrics.visible_rows; ++row) {
            const size_t index = playlist_scroll_ + static_cast<size_t>(row);
            const auto* visible_track = VisiblePlaylistTrack(index);
            if (!visible_track) break;
            RECT row_bounds{metrics.tracks.left,
                            metrics.tracks.top + row * metrics.row_height,
                            metrics.tracks.right,
                            std::min(metrics.tracks.bottom,
                                metrics.tracks.top + (row + 1) * metrics.row_height)};
            // FUN_00487C0D reads LVIS_SELECTED/LVIS_FOCUSED, then suppresses
            // both for painting when Files has lost focus and
            // LVS_SHOWSELALWAYS is absent. The selection remains in the
            // control model and becomes visible again when focus returns.
            const bool control_focused = GetFocus() == playlist_track_control_;
            const bool show_selection_always = playlist_track_control_ &&
                (GetWindowLongW(playlist_track_control_, GWL_STYLE) &
                 LVS_SHOWSELALWAYS) != 0;
            const bool selected = playlist_selected_rows_.contains(index) &&
                (control_focused || show_selection_always);
            if (selected && layout.selected.image) {
                TileBitmap(canvas, layout.selected, row_bounds);
            } else if (selected) {
                // FUN_00487C0D -> FUN_0045032C creates one vertical gradient
                // per selected item when the skin has no selected_image.
                DrawVerticalGradient(canvas, row_bounds,
                    settings_.playlist.selected_color,
                    settings_.playlist.background_color);
            }
            else if ((index & 1U) != 0) {
                const HBRUSH brush = CreateSolidBrush(settings_.playlist.alternate_background_color);
                FillRect(canvas, &row_bounds, brush);
                DeleteObject(brush);
            }
            const auto& track = *visible_track;
            const bool playing = VisiblePlaylistPlayingRow() ==
                std::optional<size_t>{index};
            COLORREF selected_text = layout.selected_text_color.value_or(GetSysColor(COLOR_HIGHLIGHTTEXT));
            if (selected_text == settings_.playlist.selected_color)
                selected_text = settings_.playlist.highlight_color;
            const COLORREF number_color = selected ? selected_text :
                (playing ? settings_.playlist.highlight_color :
                           settings_.playlist.number_color);
            const COLORREF title_color = selected ? selected_text :
                (playing ? settings_.playlist.highlight_color :
                           settings_.playlist.text_color);
            const COLORREF duration_color = selected ? selected_text :
                settings_.playlist.duration_color;
            SetTextColor(canvas, number_color);
            RECT number_bounds = row_bounds;
            // The original reserves the first six pixels for the playback
            // state glyph whether or not the current row is playing.
            number_bounds.left += 6;
            if (playing) {
                const int middle = (row_bounds.top + row_bounds.bottom) / 2;
                const POINT arrow[]{{row_bounds.left + 1, middle - 3},
                                    {row_bounds.left + 1, middle + 3},
                                    {row_bounds.left + 4, middle}};
                const HBRUSH arrow_brush = CreateSolidBrush(title_color);
                const HGDIOBJ old_brush = SelectObject(canvas, arrow_brush);
                const HGDIOBJ old_pen = SelectObject(canvas, GetStockObject(NULL_PEN));
                Polygon(canvas, arrow, static_cast<int>(std::size(arrow)));
                SelectObject(canvas, old_pen);
                SelectObject(canvas, old_brush);
                DeleteObject(arrow_brush);
            }
            number_bounds.right = std::min<LONG>(number_bounds.right,
                number_bounds.left + number_column_width);
            if (settings_.playlist.title_number) {
                const auto number = std::to_wstring(index + 1) + L".";
                DrawTextW(canvas, number.c_str(), -1, &number_bounds,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            RECT title_bounds = row_bounds;
            title_bounds.left = number_bounds.right;
            const auto formatted_title = FormatPlaylistTitle(
                track, settings_.playlist, unknown_title_format);
            const std::wstring& title = formatted_title.text;
            RECT duration_bounds = row_bounds;
            const std::wstring duration_text = PlaylistDurationText(track);
            // 00487C0D reserves the same four-pixel right inset even when
            // the duration field is empty. With a duration its measured
            // width is subtracted from that fixed edge.
            duration_bounds.right = row_bounds.right - 4;
            duration_bounds.left = duration_bounds.right -
                PlaylistTextWidth(canvas, duration_text);
            title_bounds.right = std::max(title_bounds.left,
                                          duration_bounds.left);
            SetTextColor(canvas, title_color);
            DrawTextW(canvas, title.c_str(), -1, &title_bounds,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (!duration_text.empty()) {
                SetTextColor(canvas, duration_color);
                DrawTextW(canvas, duration_text.c_str(), -1, &duration_bounds,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            if (playlist_selection_ && *playlist_selection_ == index &&
                control_focused)
                DrawSolidFrame(canvas, row_bounds, title_color);
        }
        if ((playlist_track_dragging_ || playlist_external_dragging_) &&
            playlist_track_drop_row_) {
            const long relative = static_cast<long>(*playlist_track_drop_row_) -
                                  static_cast<long>(playlist_scroll_);
            const int y = std::clamp<int>(metrics.tracks.top +
                relative * metrics.row_height, metrics.tracks.top,
                metrics.tracks.bottom - 1);
            const HPEN pen = CreatePen(PS_SOLID, 2, settings_.playlist.highlight_color);
            const HGDIOBJ old_pen = SelectObject(canvas, pen);
            MoveToEx(canvas, metrics.tracks.left, y, nullptr);
            LineTo(canvas, metrics.tracks.right, y);
            SelectObject(canvas, old_pen);
            DeleteObject(pen);
        }
        if (font) {
            SelectObject(canvas, old_font);
            DeleteObject(font);
        }
    }

    if (layout.toolbar.image) {
        // FUN_0047E6FC/0047AB54 install the skin bitmap on the toolbar, whose
        // custom-draw path treats the package transparent color as a mask.
        // SRCCOPY exposes the common #ff00ff key as a purple rectangle.
        DrawPlaylistToolbarBitmap(canvas, layout.toolbar, metrics.toolbar,
                                  skin_->TransparentColor(), std::nullopt, 255, &layout);
        const auto hot = playlist_toolbar_hover_;
        if (layout.toolbar.image.IsGdiPlus() && layout.toolbar_hot.image.IsGdiPlus() &&
            layout.toolbar_animation.Enabled()) {
            // 004AD31C/004AD423: each of seven toolbar cells has its own
            // reversible hot counter, not a four-state sprite strip.
            for (size_t index = 0; index < 7; ++index) {
                const auto& fade = SkinHoverState(playlist_window_,
                    L"toolbar" + std::to_wstring(index), metrics.toolbar,
                    hot && *hot == index ? 1 : 0, layout.toolbar_animation);
                DrawPlaylistToolbarBitmap(canvas, layout.toolbar_hot, metrics.toolbar,
                    skin_->TransparentColor(), index, fade.HotOpacity(layout.toolbar_animation), &layout);
            }
        } else if (hot && layout.toolbar_hot.image && layout.toolbar_hot.size.cx > 0) {
            DrawPlaylistToolbarBitmap(canvas, layout.toolbar_hot, metrics.toolbar,
                                      skin_->TransparentColor(), *hot, 255, &layout);
        }
    }

    const size_t track_count = VisiblePlaylistTrackCount();
    if (metrics.scrollbar_width > 0 &&
        track_count > static_cast<size_t>(
            std::max(1, metrics.visible_rows))) {
        if (layout.scrollbar_bar.image)
            TileBitmap(canvas, layout.scrollbar_bar, metrics.scrollbar);
        const auto state = ResolvePlaylistScrollbarMetrics(
            metrics, layout, track_count,
            static_cast<size_t>(std::max(1, metrics.visible_rows)),
            playlist_scroll_);
        if (layout.scrollbar_buttons.image) {
            const int source_frame_width = std::max(
                1, static_cast<int>(layout.scrollbar_buttons.size.cx / 3));
            const int destination_width = std::min(
                source_frame_width, metrics.scrollbar_width);
            const int button_source_height = std::max(
                1, static_cast<int>(layout.scrollbar_buttons.size.cy / 2));
            const int up_frame = PlaylistScrollbarImageFrame(
                PlaylistScrollbarPart::line_up, playlist_scrollbar_hover_,
                playlist_scrollbar_pressed_);
            const int down_frame = PlaylistScrollbarImageFrame(
                PlaylistScrollbarPart::line_down, playlist_scrollbar_hover_,
                playlist_scrollbar_pressed_);
            layout.scrollbar_buttons.image.Draw(canvas,
                   metrics.scrollbar.left, metrics.scrollbar.top,
                   destination_width, state.button_extent,
                   up_frame * source_frame_width, 0,
                   destination_width, state.button_extent);
            layout.scrollbar_buttons.image.Draw(canvas, metrics.scrollbar.left,
                   metrics.scrollbar.bottom - state.button_extent,
                   destination_width, state.button_extent,
                   down_frame * source_frame_width, button_source_height,
                   destination_width, state.button_extent);
        }
        if (layout.scrollbar_thumb.image) {
            RECT thumb_bounds{metrics.scrollbar.left, state.thumb_top,
                              metrics.scrollbar.right, state.thumb_bottom};
            const int frame = PlaylistScrollbarImageFrame(
                PlaylistScrollbarPart::thumb, playlist_scrollbar_hover_,
                playlist_scrollbar_pressed_);
            DrawPlaylistScrollbarThumb(
                canvas, layout.scrollbar_thumb, thumb_bounds, frame,
                layout.scrollbar_thumb_resize_center,
                layout.scrollbar_thumb_resize_tile);
        }
    }

    if (layout.title.image)
        DrawElementFrame(canvas, layout.title, metrics.title, 0, skin_->TransparentColor());
    if (layout.close.image) {
        const int state = playlist_close_pressed_ && playlist_close_hover_
            ? 2 : (playlist_close_hover_ ? 1 : 0);
        DrawAnimatedSkinFrame(canvas, layout.close, metrics.close, state, playlist_window_);
    }
    BitBlt(dc, 0, 0, width, height, canvas, 0, 0, SRCCOPY);
    SelectObject(canvas, old_buffer);
    DeleteObject(buffer);
    DeleteDC(canvas);
}

void PlayerWindow::PreparePlaylistMenu(HMENU menu) const {
    if (!menu) return;
    const bool has_tracks = VisiblePlaylistTrackCount() != 0;
    const bool has_selection = !playlist_selected_rows_.empty();
    const playlist::Track* focused_track = nullptr;
    if (playlist_selection_)
        focused_track = VisiblePlaylistTrack(*playlist_selection_);
    if (!focused_track && has_selection)
        focused_track = VisiblePlaylistTrack(*playlist_selected_rows_.begin());
    const bool focused_network = focused_track &&
        IsLegacyNetworkTrack(*focused_track);
    const bool all_selected_network = has_selection && std::all_of(
        playlist_selected_rows_.begin(), playlist_selected_rows_.end(),
        [this](size_t row) {
            const auto* track = VisiblePlaylistTrack(row);
            return track && IsLegacyNetworkTrack(*track);
        });
    EnableCommand(menu, kPlaylistPlay, has_selection);
    EnableCommand(menu, kPlaylistProperties, has_selection);
    EnableCommand(menu, kPlaylistBrowseFile, has_selection);
    EnableCommand(menu, kPlaylistDeleteSelected, has_selection);
    EnableCommand(menu, kPlaylistClear, has_tracks);
    // Menu 0x8b leaves both cleanup commands enabled even for an empty list.
    // Their command handlers perform the original count<=1/no-match no-op.
    EnableCommand(menu, kPlaylistDeleteDuplicates, true);
    EnableCommand(menu, kPlaylistDeleteInvalid, true);
    EnableCommand(menu, kPlaylistSortTitle, has_tracks);
    EnableCommand(menu, kPlaylistSortFile, has_tracks);
    EnableCommand(menu, kPlaylistSortPath, has_tracks);
    EnableCommand(menu, kPlaylistSortAlbum, has_tracks);
    EnableCommand(menu, kPlaylistSortRating, has_tracks);
    EnableCommand(menu, kPlaylistSortFileTime, has_tracks);
    EnableCommand(menu, kPlaylistSortTrackNumber, has_tracks);
    EnableCommand(menu, kPlaylistSortDuration, has_tracks);
    EnableCommand(menu, kPlaylistShuffle, has_tracks);
    EnableCommand(menu, kPlaylistSortLists, playlists_.Size() > 1);
    EnableCommand(menu, kPlaylistCut, has_selection);
    EnableCommand(menu, kPlaylistCopy, has_selection);
    EnableCommand(menu, kPlaylistPaste,
        IsClipboardFormatAvailable(PlaylistDropItemsFormat()) ||
        IsClipboardFormatAvailable(CF_HDROP) ||
        IsClipboardFormatAvailable(UniformResourceLocatorWideFormat()));
    EnableCommand(menu, kPlaylistMoveToList, has_selection);
    EnableCommand(menu, kPlaylistCopyToList, has_selection);
    EnableCommand(menu, kPlaylistRenameTitle, has_selection);
    EnableCommand(menu, kPlaylistRenameArtistTitle, has_selection);
    EnableCommand(menu, kPlaylistRenameTitleArtist, has_selection);
    EnableCommand(menu, kPlaylistRenameCustom, has_selection);
    EnableCommand(menu, kPlaylistSendToFolder, has_selection);
    EnableCommand(menu, kPlaylistConvert,
        has_selection && PlaylistConvertCommandAvailable(sound_library_));
    EnableCommand(menu, kPlaylistReplayGainScan,
        has_selection && ReplayGainScanCommandAvailable(
            sound_library_, ttpcomm_module_));
    EnableCommand(menu, kPlaylistReplayGainRemove,
        has_selection && sound_library_ != nullptr &&
        (!playlist_metadata_working_ ||
         !playlist_metadata_working_->load(std::memory_order_acquire)));
    EnableCommand(menu, kPlaylistFreeDb,
        has_selection && focused_track &&
        SupportsLegacyFreeDbQuery(*focused_track));
    EnableCommand(menu, kPlaylistDownload,
        has_selection && focused_track);
    EnableCommand(menu, kPlaylistReportOnline,
        focused_network && all_selected_network);
    for (UINT command = kPlaylistRatingFirst;
         command <= kPlaylistRatingLast; ++command)
        EnableCommand(menu, command, has_selection);
    EnableCommand(menu, kPlaylistSelectAll, has_tracks);
    EnableCommand(menu, kPlaylistSelectNone, has_selection);
    EnableCommand(menu, kPlaylistSelectInvert, has_tracks);
    EnableCommand(menu, kPlaylistFind, has_tracks);
    EnableCommand(menu, kPlaylistFindNext,
        has_tracks && playlist_find_text_[0] != L'\0');
    EnableCommand(menu, kPlaylistQuickFind, has_tracks);
    EnableCommand(menu, kPlaylistDeleteList, playlists_.Size() > 1);
    EnableCommand(menu, kPlaylistRenameList, playlist_context_list_.has_value());
    EnableCommand(menu, kPlaylistActivateList, playlist_context_list_ &&
        *playlist_context_list_ != playlists_.ActiveIndex());
    EnableCommand(menu, kPlaylistDeleteFiles,
        !settings_.playlist.disable_delete_file);

    if (playlist_selected_rows_.size() == 1) {
        const size_t row = *playlist_selected_rows_.begin();
        if (const auto* track = VisiblePlaylistTrack(row)) {
            const int rating = track->rating;
            if (rating >= 1 && rating <= 5 &&
                FindCommandMenu(menu, kPlaylistRatingFirst)) {
                CheckMenuRadioItem(menu, kPlaylistRatingFirst,
                    kPlaylistRatingLast,
                    kPlaylistRatingFirst + static_cast<UINT>(rating - 1),
                    MF_BYCOMMAND);
            }
        }
        CheckCommand(menu, kPlaylistClickRating,
                     settings_.playlist.click_rating);
    }

    UINT checked = kPlaylistModeSequential;
    if (settings_.player.play_mode == 0) checked = kPlaylistModeSingle;
    else if (settings_.player.play_mode == 1) checked = kPlaylistModeRepeatOne;
    else if (settings_.player.play_mode == 3) checked = kPlaylistModeRepeatAll;
    else if (settings_.player.play_mode == 4) checked = kPlaylistModeShuffle;
    if (FindCommandMenu(menu, checked)) {
        CheckMenuRadioItem(menu, kPlaylistModeSingle, kPlaylistModeShuffle,
                           checked, MF_BYCOMMAND);
    }
    CheckCommand(menu, kPlaylistAutoSwitchList, settings_.player.auto_switch_list);
    CheckCommand(menu, kPlaylistPlayFollowCursor,
                 settings_.player.play_follow_cursor);
    CheckCommand(menu, kPlaylistLibraryMode, settings_.playlist.library_mode);

    // Keep resource-default state for commands whose handlers are still
    // being recovered.  The original exposes these entries as enabled; the
    // earlier rebuild grayed them merely because dispatch was incomplete,
    // which made the visible menu diverge before a command was invoked.
}

void PlayerWindow::PopulatePlaylistSendToMenu(HMENU menu) {
    playlist_send_to_catalog_.PopulateMenu(
        menu, playlist_window_, kPlaylistSendToFirst, kPlaylistSendToLast,
        [this](UINT command, HICON icon) {
            if (!icon) return;
            EnsurePopupMenuImages();
            if (!popup_menu_images_) return;

            const auto existing = std::find_if(
                popup_menu_image_commands_.begin(),
                popup_menu_image_commands_.end(),
                [command](const auto& value) {
                    return value.first == command;
                });
            int image = -1;
            if (existing != popup_menu_image_commands_.end()) {
                image = ImageList_ReplaceIcon(
                    popup_menu_images_, existing->second, icon);
            }
            if (image < 0) {
                image = ImageList_AddIcon(popup_menu_images_, icon);
                if (image < 0) return;
                if (existing != popup_menu_image_commands_.end()) {
                    existing->second = image;
                } else {
                    popup_menu_image_commands_.emplace_back(command, image);
                }
            }
            // Position zero was owner-drawn before WM_INITMENUPOPUP.  Refresh
            // that already-created visual record so its newly resolved
            // folder icon is visible on the first menu opening too.
            for (auto& visual : popup_menu_items_) {
                if (visual.command == command) visual.image = image;
            }
        });
}

void PlayerWindow::ShowPlaylistContextMenu(POINT screen_point, POINT client_point) {
    playlist_send_to_catalog_.Clear();
    playlist_context_list_.reset();
    HMENU menu{};
    bool blank_catalogue{};
    bool converted_blank_menu{};
    if (const auto track = PlaylistTrackAt(client_point)) {
        if (playlist_track_control_) SetFocus(playlist_track_control_);
        // Native ListCtrl right-click keeps an existing multi-selection.  It
        // moves the caret only when the clicked row was not already selected.
        if (!playlist_selected_rows_.contains(*track)) SelectPlaylistRow(*track);
        const UINT resource = playlist_selected_rows_.size() > 1
            ? kMenuPlaylistItems : kMenuPlaylistItem;
        menu = DetachFirstPopup(LoadMenuW(ResourceModule(), MAKEINTRESOURCEW(resource)));
        if (menu) {
            SetMenuDefaultItem(menu, kPlaylistPlay, FALSE);
            if (playlist_selected_rows_.size() == 1) {
                if (const auto* visible_track = VisiblePlaylistTrack(*track))
                    TrimSingleTrackMenu(menu, visible_track->path);
            } else {
                const bool all_urls = std::all_of(
                    playlist_selected_rows_.begin(),
                    playlist_selected_rows_.end(), [this](size_t row) {
                        const auto* track = VisiblePlaylistTrack(row);
                        return track && LooksLikeUrl(track->path.wstring());
                    });
                if (all_urls) {
                    // 00488FEF removes positions 11 (Rename), 11 again
                    // (Send To after the first removal), and the separator
                    // which then occupies position 11 in menu resource 0x99.
                    DeleteMenu(menu, 11, MF_BYPOSITION);
                    DeleteMenu(menu, 11, MF_BYPOSITION);
                    DeleteMenu(menu, 11, MF_BYPOSITION);
                }
            }
        }
    } else if (const auto list = PlaylistListAt(client_point)) {
        if (playlist_list_control_) SetFocus(playlist_list_control_);
        playlist_context_list_ = *list;
        menu = DetachFirstPopup(LoadMenuW(ResourceModule(),
            MAKEINTRESOURCEW(kMenuPlaylistLists)));
        if (menu) SetMenuDefaultItem(menu, kPlaylistActivateList, FALSE);
    } else {
        RECT client{};
        GetClientRect(playlist_window_, &client);
        const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
            settings_.playlist.split_on_lists, client.right, client.bottom,
            VisiblePlaylistTrackCount());
        blank_catalogue = !settings_.playlist.library_mode &&
            PtInRect(&metrics.list_titles, client_point);
        if (blank_catalogue) {
            // PlayLists sends NM_RCLICK with iItem == -1 for its blank area.
            // 00489DFA still loads catalogue menu 0x9c, then disables the
            // item-dependent Save/Delete/Rename commands.
            if (playlist_list_control_) SetFocus(playlist_list_control_);
            menu = DetachFirstPopup(LoadMenuW(ResourceModule(),
                MAKEINTRESOURCEW(kMenuPlaylistLists)));
            if (menu)
                SetMenuDefaultItem(menu, kPlaylistActivateList, FALSE);
        }
    }
    if (!menu) {
        // FUN_00488FEF's iItem == -1 branch turns the seven top-level popups
        // from menu 0x8B into one context popup.
        menu = ConvertMenuBarToPopup(LoadMenuW(ResourceModule(),
            MAKEINTRESOURCEW(kMenuPlaylistToolbar)));
        converted_blank_menu = menu != nullptr;
    }
    if (!menu) return;
    if (blank_catalogue) {
        // FUN_00489D2E compares NMITEMACTIVATE::iItem (-1 here) with the
        // active catalogue index, so Activate remains enabled/default even
        // though Save/Delete/Rename are item-dependent and disabled.  The
        // command later resolves the still-selected catalogue row itself.
        EnableCommand(menu, kPlaylistSaveList, false);
        EnableCommand(menu, kPlaylistDeleteList, false);
        EnableCommand(menu, kPlaylistRenameList, false);
        EnableCommand(menu, kPlaylistActivateList, true);
    } else if (converted_blank_menu) {
        // Files/NM_RCLICK with iItem == -1 follows FUN_00488FEF's 0x8B
        // branch.  It preserves every resource-default command state and
        // changes only the physical-delete policy before converting the
        // seven top-level popups.
        EnableCommand(menu, kPlaylistDeleteFiles,
                      !settings_.playlist.disable_delete_file);
    } else {
        PreparePlaylistMenu(menu);
    }
    BeginPopupMenuStyle(menu);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screen_point.x, screen_point.y, 0, playlist_window_, nullptr);
    EndPopupMenuStyle();
    DestroyMenu(menu);
    if (command != 0 && !HandlePlaylistCommand(command))
        HandleContextCommand(command);
    playlist_send_to_catalog_.Clear();
    playlist_context_list_.reset();
}

void PlayerWindow::DeleteSelectedPlaylistRows() {
    if (playlist_selected_rows_.empty()) return;
    // FUN_00485759 remembers the last selected ListView row, then subtracts
    // selectedCount - 1.  After deletion it wraps an out-of-range caret to
    // row zero.  This differs visibly from always choosing first_removed for
    // a sparse selection such as {1, 3}.
    const size_t selection_after_delete =
        *playlist_selected_rows_.rbegin() + 1 -
        playlist_selected_rows_.size();
    const bool affects_playing = playing_playlist_index_ &&
        *playing_playlist_index_ == playlists_.ActiveIndex();
    const auto old_current = affects_playing ? current_ : std::nullopt;
    size_t removed_before_current = 0;
    bool removed_current = false;
    for (auto iterator = playlist_selected_rows_.rbegin();
         iterator != playlist_selected_rows_.rend(); ++iterator) {
        if (old_current) {
            if (*iterator == *old_current) removed_current = true;
            else if (*iterator < *old_current) ++removed_before_current;
        }
        ActivePlaylist().Remove(*iterator);
    }
    playlists_.MarkDirty();
    if (removed_current) {
        // The native cleanup commands drop the list's playing marker but do
        // not stop the already-open decoder or clear the persisted filename.
        DetachPlayingPlaylistItem();
    } else if (current_) {
        *current_ -= removed_before_current;
    }
    playlist_selected_rows_.clear();
    if (ActivePlaylist().Tracks().empty()) {
        playlist_selection_.reset();
        playlist_selection_anchor_.reset();
    } else {
        playlist_selection_ = selection_after_delete <
                ActivePlaylist().Tracks().size()
            ? selection_after_delete : 0;
        playlist_selection_anchor_ = playlist_selection_;
        playlist_selected_rows_.insert(*playlist_selection_);
    }
    RememberPlaylistRow(playlists_.ActiveIndex(), playlist_selection_);
    EnsurePlaylistSelectionVisible();
    RefreshPlaylist();
}

void PlayerWindow::RemovePlaylistRowsPreservingView(
    const std::set<size_t>& rows) {
    if (rows.empty()) return;
    const bool affects_playing = playing_playlist_index_ &&
        *playing_playlist_index_ == playlists_.ActiveIndex();
    const auto old_current = affects_playing ? current_ : std::nullopt;
    size_t removed_before_current{};
    bool removed_current{};
    for (const size_t row : rows) {
        if (!old_current) break;
        if (row < *old_current) ++removed_before_current;
        else if (row == *old_current) removed_current = true;
    }
    for (auto iterator = rows.rbegin(); iterator != rows.rend(); ++iterator) {
        if (*iterator < ActivePlaylist().Tracks().size())
            ActivePlaylist().Remove(*iterator);
    }
    playlists_.MarkDirty();
    if (removed_current) {
        // 004858DF/00485A5D clear only the Files list's playing-row marker.
        // The decoder keeps playing the detached CPlayItem and the persisted
        // source identity is not cleared by either cleanup operation.
        DetachPlayingPlaylistItem();
    } else if (current_) {
        *current_ -= std::min(*current_, removed_before_current);
    }

    // 004858DF/00485A5D change the owner-data item count directly. Native
    // ListView selection and caret state therefore stay on numerical rows;
    // they are not remapped to follow the surviving CPlayItem objects.
    const size_t count = ActivePlaylist().Tracks().size();
    for (auto selected = playlist_selected_rows_.begin();
         selected != playlist_selected_rows_.end();) {
        if (*selected >= count) selected = playlist_selected_rows_.erase(selected);
        else ++selected;
    }
    if (playlist_selection_ && *playlist_selection_ >= count)
        playlist_selection_.reset();
    if (playlist_selection_anchor_ && *playlist_selection_anchor_ >= count)
        playlist_selection_anchor_.reset();
    ActivePlaylist().SetCurrentRow(playlist_selection_);
    EnsurePlaylistSelectionVisible();
    RefreshPlaylist();
}

void PlayerWindow::DetachPlayingPlaylistItem() {
    // The decoder owns the opened CPlayItem independently of CPlayList's
    // Files vector.  004858DF/00485A5D and 00485BF7 invalidate only the
    // per-list playing row; CPlayerWnd keeps the source-list owner so later
    // completion still observes that list's (possibly zero) item count.
    if (playing_playlist_index_ &&
        *playing_playlist_index_ < playlists_.Size()) {
        const size_t owner = *playing_playlist_index_;
        if (playlists_.At(owner).SetPlayingRow(std::nullopt))
            playlists_.MarkDirty(owner);
    }
    current_.reset();
}

void PlayerWindow::ClearActivePlaylist() {
    const bool clears_playing = playing_playlist_index_ &&
        *playing_playlist_index_ == playlists_.ActiveIndex();
    ActivePlaylist().Clear();
    playlists_.MarkDirty();
    if (clears_playing) DetachPlayingPlaylistItem();
    playlist_selection_.reset();
    playlist_selection_anchor_.reset();
    playlist_selected_rows_.clear();
    playlist_scroll_ = 0;
    RefreshPlaylist();
}

bool PlayerWindow::FindNextPlaylistTrack(DWORD flags) {
    const std::wstring_view needle(playlist_find_text_);
    const size_t count = VisiblePlaylistTrackCount();
    if (needle.empty() || count == 0) return false;

    const bool forward = (flags & FR_DOWN) != 0;
    size_t start = forward ? 0 : count - 1;
    if (playlist_selection_ && *playlist_selection_ < count) {
        start = forward ? (*playlist_selection_ + 1) % count
                        : (*playlist_selection_ + count - 1) % count;
    }
    const std::wstring unknown_title_format = ResourceText(0x81c8);
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t index = forward
            ? (start + offset) % count
            : (start + count - offset) % count;
        const auto* visible_track = VisiblePlaylistTrack(index);
        if (!visible_track) continue;
        const auto& track = *visible_track;
        const auto matches = [needle](std::wstring_view value) {
            return PlaylistWideContains(value, needle);
        };

        const auto display_title = FormatPlaylistTitle(
            track, settings_.playlist, unknown_title_format).text;
        const auto file_name = track.path.filename().wstring();
        if (matches(display_title) || matches(file_name)) {
            SelectPlaylistRow(index);
            return true;
        }

        // FUN_004860E5 keeps quick-find on the lightweight visible-title
        // path.  Ordinary find additionally examines the file source and
        // the title/artist/album fields already resident on CPlayItem.  It
        // deliberately does not synchronously open a reader on the UI STA.
        if (playlist_find_quick_) continue;
        if (matches(track.path.wstring()) ||
            matches(PlaylistUtf8Field(track.title)) ||
            matches(PlaylistUtf8Field(track.artist)) ||
            matches(PlaylistUtf8Field(track.album)) ||
            matches(PlaylistMetadataValue(track, "Title")) ||
            matches(PlaylistMetadataValue(track, "Artist")) ||
            matches(PlaylistMetadataValue(track, "Album"))) {
            SelectPlaylistRow(index);
            return true;
        }
    }

    // FUN_004860E5 reports resource 0x8198 only for the ordinary Find path.
    // Quick Find deliberately treats a miss as a silent selection update.
    // The native code also guards the modal notification against re-entry.
    if (!playlist_find_quick_) {
        static bool showing_not_found{};
        if (!showing_not_found) {
            showing_not_found = true;
            const HWND owner = playlist_find_dialog_ &&
                    IsWindow(playlist_find_dialog_)
                ? playlist_find_dialog_ : playlist_window_;
            MessageBoxW(owner, ResourceText(0x8198).c_str(),
                ResourceText(0x80).c_str(), MB_OK | MB_ICONINFORMATION);
            showing_not_found = false;
        }
    }
    return false;
}

void PlayerWindow::ShowPlaylistFindDialog(bool quick) {
    if (playlist_find_dialog_ && IsWindow(playlist_find_dialog_)) {
        // FUN_00486056 always ends the previous modeless find controller
        // before opening another one.  This is observable when switching
        // between Find and Quick Find; merely foregrounding the old dialog
        // leaves it bound to the wrong search semantics.
        const HWND previous = playlist_find_dialog_;
        SendMessageW(previous, WM_CLOSE, 0, 0);
        if (IsWindow(previous)) DestroyWindow(previous);
    }
    playlist_find_dialog_ = nullptr;
    playlist_find_quick_ = quick;
    playlist_find_ = {};
    playlist_find_.lStructSize = sizeof(playlist_find_);
    playlist_find_.hwndOwner = playlist_window_;
    playlist_find_.lpstrFindWhat = playlist_find_text_;
    playlist_find_.wFindWhatLen = static_cast<WORD>(std::size(playlist_find_text_));
    playlist_find_.Flags = FR_DOWN | FR_HIDEWHOLEWORD | FR_HIDEMATCHCASE;
    playlist_find_dialog_ = FindTextW(&playlist_find_);
}

void PlayerWindow::BeginPlaylistOleDrag() {
    if (playlist_selected_rows_.empty()) return;
    const bool media_library_source = settings_.playlist.library_mode;
    const auto media_library_identities = media_library_source
        ? CaptureSelectedMediaLibraryIdentities()
        : std::vector<std::wstring>{};
    const size_t playlist_source_slot = media_library_source
        ? playlist::PlaylistStore::npos : playlists_.ActiveSlot();
    const auto playlist_source_identities = media_library_source
        ? std::vector<PlaylistDragTrackIdentity>{}
        : CapturePlaylistDragTrackIdentities(
              ActivePlaylist(), playlist_selected_rows_);
    std::vector<std::filesystem::path> local_paths;
    std::wstring first_url;
    for (const size_t row : playlist_selected_rows_) {
        const auto* track = VisiblePlaylistTrack(row);
        if (!track) continue;
        const auto& path = track->path;
        if (path.empty()) continue;
        if (LooksLikeUrl(path.wstring())) {
            if (first_url.empty()) first_url = path.wstring();
        } else {
            local_paths.push_back(path);
        }
    }
    if (local_paths.empty() && first_url.empty()) return;

    auto* data = new (std::nothrow) PlaylistDragDataObject(
        std::move(local_paths), std::move(first_url));
    auto* source = new (std::nothrow) PlaylistDropSource;
    if (!data || !source) {
        if (data) data->Release();
        if (source) source->Release();
        return;
    }

    // 004894AF asks IDragSourceHelper for the standard shell drag image.  A
    // helper failure is non-fatal; DoDragDrop still exposes the same formats.
    IDragSourceHelper* helper{};
    if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper))) && helper) {
        POINT origin = playlist_track_drag_origin_;
        if (playlist_track_control_)
            MapWindowPoints(playlist_window_, playlist_track_control_,
                            &origin, 1);
        static_cast<void>(helper->InitializeFromWindow(
            playlist_track_control_ ? playlist_track_control_ : playlist_window_,
            &origin, data));
        helper->Release();
    }

    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(data, source,
        DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    source->Release();
    data->Release();
    // 004894AF dispatches 0x7F13 for a completed drop when pdwEffect is zero
    // or contains MOVE.  COPY/LINK and cancel/error results keep the source.
    if (ShouldRemovePlaylistDragSource(result, effect)) {
        if (media_library_source) {
            RemoveMediaLibraryTracksByIdentity(media_library_identities);
            return;
        }

        const auto source_index = playlists_.IndexOfSlot(playlist_source_slot);
        if (!source_index) return;
        auto& source_playlist = playlists_.At(*source_index);
        const auto source_rows = ResolvePlaylistDragTrackRows(
            source_playlist, playlist_source_identities);
        if (source_rows.empty()) return;

        // Snapshot the state which exists *after* DoDragDrop's nested loop.
        // It may no longer be the drag-start selection.  Only remap that
        // state around deletion; never pass it to DeleteSelectedPlaylistRows
        // and accidentally remove newly selected rows or another catalogue.
        const bool source_is_active = !settings_.playlist.library_mode &&
            playlists_.ActiveIndex() == *source_index;
        const auto visible_selection = source_is_active
            ? playlist_selected_rows_ : std::set<size_t>{};
        const auto visible_caret = source_is_active
            ? playlist_selection_ : std::optional<size_t>{};
        const auto visible_anchor = source_is_active
            ? playlist_selection_anchor_ : std::optional<size_t>{};
        const bool source_is_playing = playing_playlist_index_ &&
            *playing_playlist_index_ == *source_index;
        const auto live_playing_row = source_is_playing
            ? current_ : std::optional<size_t>{};
        const auto remapped_playing_row = source_is_playing
            ? RemapPlaylistRowAfterRemoval(live_playing_row, source_rows)
            : std::optional<size_t>{};

        for (auto row = source_rows.rbegin(); row != source_rows.rend(); ++row)
            source_playlist.Remove(*row);
        playlists_.MarkDirty(*source_index);

        if (source_is_playing && live_playing_row) {
            if (!remapped_playing_row) {
                // Match command 0x7F13: removing the playing CPlayItem clears
                // the list marker but keeps both the decoder and its owning
                // CPlayList pointer alive as a detached playback source.
                DetachPlayingPlaylistItem();
            } else {
                current_ = remapped_playing_row;
            }
        }

        if (!source_is_active) {
            // Playlist::Remove has already remapped the inactive list's
            // transient caret and playing marker.  Leave the now-active
            // catalogue's ListView state untouched.
            if (playlist_window_)
                InvalidateRect(playlist_window_, nullptr, FALSE);
            return;
        }

        const bool had_visible_selection = !visible_selection.empty();
        playlist_selected_rows_ = RemapPlaylistRowsAfterRemoval(
            visible_selection, source_rows);
        playlist_selection_ = RemapPlaylistRowAfterRemoval(
            visible_caret, source_rows);
        playlist_selection_anchor_ = RemapPlaylistRowAfterRemoval(
            visible_anchor, source_rows);

        // Playlist::Remove preserves a current row by moving the caret to the
        // item which follows a deleted row (or the new final row).  Use that
        // result when the ListView caret itself was part of the move.
        if (!playlist_selection_ && visible_caret)
            playlist_selection_ = source_playlist.CurrentRow();
        if (playlist_selected_rows_.empty() && had_visible_selection &&
            !source_playlist.Tracks().empty()) {
            const size_t fallback = playlist_selection_.value_or(std::min(
                *source_rows.begin(), source_playlist.Tracks().size() - 1));
            playlist_selection_ = fallback;
            playlist_selected_rows_.insert(fallback);
        } else if (!playlist_selection_ && !playlist_selected_rows_.empty()) {
            playlist_selection_ = *playlist_selected_rows_.begin();
        }
        if (!playlist_selection_anchor_)
            playlist_selection_anchor_ = playlist_selection_;
        RememberPlaylistRow(*source_index, playlist_selection_);
        if (playlist_view_) {
            SendMessageW(playlist_view_, LB_SETCURSEL,
                playlist_selection_.value_or(static_cast<size_t>(-1)), 0);
        }
        EnsurePlaylistSelectionVisible();
        RefreshPlaylist();
    }
}

void PlayerWindow::FinishPlaylistListDrag() {
    if (!playlist_list_drag_row_ || !playlist_list_drop_index_) return;
    const auto move = PlanPlaylistCatalogMove(
        *playlist_list_drag_row_, *playlist_list_drop_index_,
        playlists_.Size(), (GetKeyState(VK_CONTROL) & 0x8000) != 0);
    if (!move || move->source == move->destination) return;

    const auto slot_at = [this](const std::optional<size_t>& index)
        -> std::optional<size_t> {
        if (!index || *index >= playlists_.Size()) return std::nullopt;
        return playlists_.Entries()[*index].slot;
    };
    const auto playing_slot = slot_at(playing_playlist_index_);
    const auto selected_slot = slot_at(playlist_list_selection_);
    const auto focus_slot = slot_at(playlist_list_focus_);
    const auto context_slot = slot_at(playlist_context_list_);
    const auto edit_slot = slot_at(playlist_list_edit_index_);

    if (!playlists_.ReorderList(move->source, move->destination)) return;
    const auto find_slot = [this](const std::optional<size_t>& slot)
        -> std::optional<size_t> {
        return slot ? playlists_.IndexOfSlot(*slot) : std::nullopt;
    };
    playing_playlist_index_ = find_slot(playing_slot);
    playlist_list_selection_ = find_slot(selected_slot);
    playlist_list_focus_ = find_slot(focus_slot);
    playlist_context_list_ = find_slot(context_slot);
    playlist_list_edit_index_ = find_slot(edit_slot);

    // FUN_00489CC7 redraws the affected range and switches by the selected
    // row after each adjacent move.  Stable slots let the rebuild perform the
    // equivalent operation once without ever changing playback identity.
    settings_.player.active_playlist =
        static_cast<int>(playlists_.ActiveSlot());
    if (playlist_list_control_ && playlist_list_focus_)
        SendMessageW(playlist_list_control_, LVM_ENSUREVISIBLE,
                     *playlist_list_focus_, FALSE);
    LayoutPlaylistListEdit();
    InvalidateRect(playlist_window_, nullptr, FALSE);
}

void PlayerWindow::FinishPlaylistTrackDrag(POINT point) {
    if (playlist_selected_rows_.empty()) return;
    // The original media-library Files model is a query CPlayList: it can be
    // an OLE drag source, but it is not reordered into the numbered active
    // playlist when a captured drag is released back over its own rows.
    if (settings_.playlist.library_mode) return;
    RECT client{};
    GetClientRect(playlist_window_, &client);
    const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
        settings_.playlist.split_on_lists, client.right, client.bottom,
        ActivePlaylist().Tracks().size());

    if (!settings_.playlist.library_mode &&
        settings_.playlist.split_on_lists > 0 &&
        PtInRect(&metrics.list_titles, point)) {
        const size_t source_index = playlists_.ActiveIndex();
        const auto target_row = PlaylistListAt(point);
        // 004822AD rejects an internal drop back onto its source catalogue
        // row.  It otherwise snapshots the selected CPlayItem objects before
        // command 0x7F13 removes them for MOVE.
        if (target_row && *target_row == source_index) return;
        std::vector<playlist::Track> transferred;
        transferred.reserve(playlist_selected_rows_.size());
        const auto& source_tracks = ActivePlaylist().Tracks();
        for (const size_t index : playlist_selected_rows_) {
            if (index < source_tracks.size())
                transferred.push_back(source_tracks[index]);
        }
        if (transferred.empty()) return;

        const bool copy = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (!copy) DeleteSelectedPlaylistRows();

        size_t target_index{};
        if (target_row) {
            target_index = *target_row;
            SwitchPlaylist(target_index);
        } else {
            auto format = ResourceText(0x8192);
            wchar_t title[128]{};
            if (!format.empty())
                swprintf_s(title, format.c_str(), playlists_.Size() + 1);
            target_index = playlists_.NewList(title[0]
                ? std::wstring(title) : ResourceText(0x813a));
            settings_.player.playlist_scan_count =
                static_cast<int>(playlists_.Size());
            settings_.player.active_playlist =
                static_cast<int>(playlists_.ActiveSlot());
            SwitchPlaylist(target_index);
        }
        static_cast<void>(CommitImportedTracks(
            std::move(transferred), target_index,
            playlists_.At(target_index).Tracks().size(), false,
            ImportPlayback::none));
        playlist_list_hover_.reset();
        return;
    }

    // 0048218C enters OLE DoDragDrop and 004822AD commits only when the
    // Files/PlayLists drop target returns a real effect.  Releasing capture
    // over toolbar, scrollbar, splitter, window chrome, or outside the
    // playlist is a cancelled drag and must not be interpreted as an
    // insertion at the first/last track.
    if (!PtInRect(&metrics.tracks, point)) {
        playlist_list_hover_.reset();
        playlist_track_drop_row_.reset();
        return;
    }

    size_t insertion = playlist_track_drop_row_.value_or(
        ActivePlaylist().Tracks().size());
    if (!playlist_track_drop_row_) {
        insertion = std::min(playlist_scroll_ + static_cast<size_t>(std::clamp<int>(
            static_cast<int>(point.y - metrics.tracks.top +
                metrics.row_height / 2) / metrics.row_height,
            0, metrics.visible_rows)),
            ActivePlaylist().Tracks().size());
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        // Files' OLE drop target advertises COPY while Ctrl is held.  A copy
        // back into the same Files list clones the selected CPlayItem range
        // at the insertion mark; it must not route through Reorder(), which
        // removes the source rows first.
        std::vector<playlist::Track> copies;
        copies.reserve(playlist_selected_rows_.size());
        for (const size_t row : playlist_selected_rows_) {
            if (row < ActivePlaylist().Tracks().size())
                copies.push_back(ActivePlaylist().Tracks()[row]);
        }
        static_cast<void>(CommitImportedTracks(std::move(copies),
            playlists_.ActiveIndex(), insertion, false,
            ImportPlayback::none));
        return;
    }
    const bool reorders_playing = playing_playlist_index_ &&
        *playing_playlist_index_ == playlists_.ActiveIndex();
    auto reordered = ActivePlaylist().Reorder(playlist_selected_rows_, insertion);
    if (reordered.empty()) return;
    playlists_.MarkDirty();
    playlist_selected_rows_ = std::move(reordered);
    playlist_selection_ = *playlist_selected_rows_.begin();
    playlist_selection_anchor_ = playlist_selection_;
    RememberPlaylistRow(playlists_.ActiveIndex(), playlist_selection_);
    if (reorders_playing) current_ = ActivePlaylist().PlayingRow();
    EnsurePlaylistSelectionVisible();
    RefreshPlaylist();
}

bool PlayerWindow::HandlePlaylistCommand(UINT command) {
    if (settings_.playlist.library_mode &&
        HandleMediaLibraryCommand(command)) return true;
    if (command >= kPlaylistSendToFirst &&
        command <= kPlaylistSendToLast) {
        return HandlePlaylistSendToCommand(command);
    }
    if (HandleLegacyPlaylistNetworkCommand(command)) return true;
    if (command == kPlaylistPlay) {
        if (playlist_selection_) SelectTrack(*playlist_selection_, true);
        return true;
    }
    if (command == kPlaylistProperties) {
        ShowPlaylistProperties();
        return true;
    }
    if (command == kPlaylistBrowseFile) {
        if (playlist_selected_rows_.empty()) return true;
        const size_t row = *playlist_selected_rows_.begin();
        if (row >= ActivePlaylist().Tracks().size()) return true;
        auto path = ActivePlaylist().Tracks()[row].path.wstring();
        if (LooksLikeUrl(path)) return true;
        auto lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        size_t archive = lower.find(L".zip|");
        if (archive == std::wstring::npos) archive = lower.find(L".rar|");
        if (archive != std::wstring::npos) path.resize(archive + 4);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            MessageBoxW(playlist_window_, ResourceText(0x8ca1).c_str(),
                        ResourceText(0x8ca0).c_str(),
                        MB_OK | MB_ICONEXCLAMATION);
            return true;
        }
        const std::wstring arguments = L"/e,/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(),
                      nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (command == kPlaylistAddFile) {
        ChooseFiles(false, playlist_window_);
        return true;
    }
    if (command == kPlaylistAddFolder) {
        ChooseFolder();
        return true;
    }
    if (command == kPlaylistLocalSearch) {
        ShowPlaylistLocalSearch();
        return true;
    }
    if (command == kPlaylistAddUrl) {
        PlaylistTextDialogState state{kPlaylistUrlControl, L"http://"};
        const INT_PTR result = DialogBoxParamW(ResourceModule(),
            MAKEINTRESOURCEW(204), playlist_window_, PlaylistTextDialogProc,
            reinterpret_cast<LPARAM>(&state));
        if (result == IDOK) {
            const std::filesystem::path path(state.value);
            static_cast<void>(ImportFiles({path}, playlists_.ActiveIndex(),
                std::nullopt, false, ImportPlayback::if_idle));
        } else if (result == 0x3ff) {
            ChooseFiles(false, playlist_window_);
        }
        return true;
    }
    if (command == kPlaylistOnlineSearch) {
        auto address = ResourceText(0x8298);
        while (!address.empty() && address.back() == L'/') address.pop_back();
        if (!address.empty())
            address += L"/lrcsearch.html?f=client-search";
        if (!address.empty())
            ShellExecuteW(playlist_window_, L"open", address.c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (command == kPlaylistConvert) {
        if (playlist_selected_rows_.empty() ||
            !PlaylistConvertCommandAvailable(sound_library_)) return true;
        std::vector<playlist::Track> tracks;
        std::vector<std::wstring> display_titles;
        tracks.reserve(playlist_selected_rows_.size());
        for (const size_t row : playlist_selected_rows_) {
            if (const auto* track = VisiblePlaylistTrack(row)) {
                tracks.push_back(*track);
                display_titles.push_back(PlaylistDisplayText(*track));
            }
        }
        if (tracks.empty()) return true;
        ShowPlaylistConverter(
            window_, ResourceModule(), ttpcomm_module_,
            sound_library_, std::move(tracks), settings_.convert,
            settings_.equalizer, [this](const std::filesystem::path& path) {
            if (settings_.playlist.library_mode) {
                std::vector<playlist::Track> converted;
                static_cast<void>(CollectImportedTracks(path, converted));
                static_cast<void>(CommitMediaLibraryTracks(
                    std::move(converted), VisiblePlaylistTrackCount(), false));
            } else {
                static_cast<void>(ImportFiles(
                    {path}, playlists_.ActiveIndex(), std::nullopt, false,
                    ImportPlayback::none));
            }
        }, std::move(display_titles));
        return true;
    }
    if (command == kPlaylistReplayGainScan) {
        if (playlist_selected_rows_.empty()) return true;
        std::vector<playlist::Track> tracks;
        tracks.reserve(playlist_selected_rows_.size());
        for (const size_t row : playlist_selected_rows_) {
            if (const auto* track = VisiblePlaylistTrack(row))
                tracks.push_back(*track);
        }
        static_cast<void>(ShowPlaylistReplayGainScanner(
            playlist_window_, ResourceModule(), ttpcomm_module_,
            sound_library_, std::move(tracks), settings_.playback.skip_scan_gain));
        return true;
    }
    if (command == kPlaylistReplayGainRemove) {
        if (playlist_selected_rows_.empty() || !sound_library_ ||
            (playlist_metadata_working_ && playlist_metadata_working_->load(
                std::memory_order_acquire))) return true;
        std::vector<std::filesystem::path> paths;
        paths.reserve(playlist_selected_rows_.size());
        for (const size_t row : playlist_selected_rows_) {
            if (const auto* track = VisiblePlaylistTrack(row))
                paths.push_back(track->path);
        }
        if (paths.empty()) return true;

        auto library = sound_library_->RetainForBackground();
        if (!library) return true;
        std::shared_ptr<std::atomic_bool> working;
        std::shared_ptr<std::atomic_bool> cancel;
        try {
            working = std::make_shared<std::atomic_bool>(true);
            cancel = std::make_shared<std::atomic_bool>(false);
        } catch (const std::exception&) {
            return true;
        }
        playlist_metadata_working_ = working;
        playlist_metadata_cancel_ = cancel;
        try {
            std::thread(
                [library = std::move(library), working, cancel,
                 paths = std::move(paths)] {
                const HRESULT initialized = CoInitializeEx(
                    nullptr, COINIT_MULTITHREADED);
                for (const auto& path : paths) {
                    if (cancel->load(std::memory_order_acquire)) break;
                    static_cast<void>(RemoveReplayGainTags(library.get(), path));
                }
                if (SUCCEEDED(initialized)) CoUninitialize();
                working->store(false, std::memory_order_release);
                }).detach();
        } catch (const std::system_error&) {
            working->store(false, std::memory_order_release);
        }
        return true;
    }
    if (command == kPlaylistDeleteSelected) { DeleteSelectedPlaylistRows(); return true; }
    if (command == kPlaylistClear) { ClearActivePlaylist(); return true; }
    if (command == kPlaylistDeleteDuplicates || command == kPlaylistDeleteInvalid) {
        std::set<size_t> remove;
        std::set<std::wstring> identities;
        for (size_t index = 0; index < ActivePlaylist().Tracks().size(); ++index) {
            const auto& track = ActivePlaylist().Tracks()[index];
            if (command == kPlaylistDeleteInvalid) {
                if (!PlaylistTrackSourceValid(track))
                    remove.insert(index);
            } else {
                if (!identities.insert(PlaylistTrackIdentity(track)).second)
                    remove.insert(index);
            }
        }
        RemovePlaylistRowsPreservingView(remove);
        return true;
    }
    if (command == kPlaylistDeleteFiles) {
        if (settings_.playlist.disable_delete_file) return true;
        const int answer = MessageBoxW(playlist_window_,
            ResourceText(0x819a).c_str(), ResourceText(0x80).c_str(),
            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) return true;
        if (playlist_selected_rows_.empty()) {
            if (VisiblePlaylistTrackCount() != 0) {
                MessageBoxW(playlist_window_, ResourceText(0x8197).c_str(),
                            ResourceText(0x8ca0).c_str(),
                            MB_OK | MB_ICONEXCLAMATION);
            }
            return true;
        }

        std::vector<playlist::Track> selected_tracks;
        selected_tracks.reserve(playlist_selected_rows_.size());
        for (const size_t row : playlist_selected_rows_) {
            if (const auto* track = VisiblePlaylistTrack(row))
                selected_tracks.push_back(*track);
        }
        const auto library_identities = settings_.playlist.library_mode
            ? CaptureSelectedMediaLibraryIdentities()
            : std::vector<std::wstring>{};
        std::optional<std::wstring> playing_identity;
        if (const auto* playing = PlaybackTrackForUi())
            playing_identity = PlaylistTrackIdentity(*playing);
        const bool deletes_playing = settings_.playlist.library_mode
            ? playing_identity && std::any_of(
                selected_tracks.begin(), selected_tracks.end(),
                [&playing_identity](const auto& track) {
                    return PlaylistTrackIdentity(track) == *playing_identity;
                })
            : playing_playlist_index_ && current_ &&
                *playing_playlist_index_ == playlists_.ActiveIndex() &&
                playlist_selected_rows_.contains(*current_);
        if (deletes_playing) {
            // 00485C92 closes the active source before invoking the shell;
            // a later shell cancellation therefore still leaves it stopped.
            Stop();
        }

        std::vector<std::filesystem::path> files;
        std::set<std::wstring> unique;
        const auto append_existing = [&](const std::filesystem::path& path) {
            if (path.empty() || LooksLikeUrl(path.wstring())) return;
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return;
            std::error_code error;
            auto absolute = std::filesystem::absolute(path, error);
            if (error) absolute = path;
            auto key = absolute.lexically_normal().wstring();
            std::transform(key.begin(), key.end(), key.begin(), towlower);
            if (unique.insert(std::move(key)).second) files.push_back(path);
        };
        for (const auto& track : selected_tracks) {
            auto extension = track.path.extension().wstring();
            std::transform(extension.begin(), extension.end(),
                           extension.begin(), towlower);
            const bool removable_source =
                extension != L".cue" && track.subtrack == 0;
            if (removable_source)
                append_existing(track.path);
            if (answer == IDYES && removable_source) {
                const bool is_playing_item = playing_identity &&
                    PlaylistTrackIdentity(track) == *playing_identity;
                const auto associated = FindPlaylistAssociatedLyrics(
                    track, settings_.lyric,
                    is_playing_item ? associated_lyric_path_
                                    : std::filesystem::path{});
                for (const auto& lyric : associated)
                    append_existing(lyric);
            }
        }

        bool removed = files.empty();
        if (!files.empty()) {
            auto source = ShellPathList(files);
            SHFILEOPSTRUCTW operation{};
            operation.hwnd = playlist_window_;
            operation.wFunc = FO_DELETE;
            operation.pFrom = source.data();
            operation.fFlags = FOF_ALLOWUNDO;
            removed = SHFileOperationW(&operation) == 0 &&
                      !operation.fAnyOperationsAborted;
        }
        if (removed) {
            if (settings_.playlist.library_mode)
                RemoveMediaLibraryTracksByIdentity(library_identities);
            else
                DeleteSelectedPlaylistRows();
        }
        return true;
    }
    if (command == kPlaylistCopy || command == kPlaylistCut) {
        std::vector<playlist::Track> copied;
        copied.reserve(playlist_selected_rows_.size());
        for (const size_t index : playlist_selected_rows_) {
            if (index < ActivePlaylist().Tracks().size())
                copied.push_back(ActivePlaylist().Tracks()[index]);
        }
        playlist_clipboard_tracks_ = copied;
        static_cast<void>(PublishPlaylistClipboard(playlist_window_, copied));
        playlist_clipboard_sequence_ = GetClipboardSequenceNumber();
        if (command == kPlaylistCut && !playlist_selected_rows_.empty())
            DeleteSelectedPlaylistRows();
        return true;
    }
    if (command == kPlaylistPaste) {
        // 00486321 calls 0047FC61 (first LVNI_SELECTED row), not the focus
        // caret.  When selection is empty its -1 result appends at the end.
        const size_t insertion = playlist_selected_rows_.empty()
            ? VisiblePlaylistTrackCount()
            : *playlist_selected_rows_.begin();
        const bool private_format =
            IsClipboardFormatAvailable(PlaylistDropItemsFormat()) != FALSE;
        if (private_format) {
            // 00481F2A fails the private-format branch when its sole DWORD
            // is another process ID; it does not deserialize a Track or
            // silently reinterpret that payload as file text.
            if (ClipboardHasCurrentPlaylistProcess(playlist_window_) &&
                !playlist_clipboard_tracks_.empty() &&
                playlist_clipboard_sequence_ == GetClipboardSequenceNumber()) {
                if (settings_.playlist.library_mode) {
                    static_cast<void>(CommitMediaLibraryTracks(
                        playlist_clipboard_tracks_, insertion, true));
                } else {
                    static_cast<void>(CommitImportedTracks(
                        playlist_clipboard_tracks_, playlists_.ActiveIndex(),
                        insertion, false, ImportPlayback::if_idle));
                }
            }
        } else {
            const auto paths = ReadPlaylistClipboard(playlist_window_);
            if (settings_.playlist.library_mode) {
                std::vector<playlist::Track> tracks;
                for (const auto& path : paths)
                    static_cast<void>(CollectImportedTracks(path, tracks));
                static_cast<void>(CommitMediaLibraryTracks(
                    std::move(tracks), insertion, true));
            } else {
                static_cast<void>(ImportFiles(paths, playlists_.ActiveIndex(),
                    insertion, false, ImportPlayback::if_idle));
            }
        }
        if (playlist_track_control_) SetFocus(playlist_track_control_);
        return true;
    }
    if (command == kPlaylistMoveToList || command == kPlaylistCopyToList) {
        if (playlist_selected_rows_.empty()) return true;
        PlaylistTargetDialogState state{&playlists_};
        if (DialogBoxParamW(ResourceModule(), MAKEINTRESOURCEW(223),
                playlist_window_, PlaylistTargetDialogProc,
                reinterpret_cast<LPARAM>(&state)) != IDOK) return true;
        const size_t source_index = playlists_.ActiveIndex();
        if (state.selected == source_index) {
            MessageBoxW(playlist_window_, ResourceText(0x8199).c_str(),
                        ResourceText(0x80).c_str(), MB_OK | MB_ICONEXCLAMATION);
            return true;
        }
        std::vector<playlist::Track> transferred;
        transferred.reserve(playlist_selected_rows_.size());
        for (const size_t index : playlist_selected_rows_) {
            if (index < ActivePlaylist().Tracks().size())
                transferred.push_back(ActivePlaylist().Tracks()[index]);
        }
        if (transferred.empty()) return true;

        size_t target_index = state.selected;
        if (target_index == playlists_.Size()) {
            auto format = ResourceText(0x8192);
            wchar_t title[128]{};
            if (!format.empty())
                swprintf_s(title, format.c_str(), playlists_.Size() + 1);
            target_index = playlists_.NewList(title[0]
                ? std::wstring(title) : ResourceText(0x813a));
            // Creating the target is an implementation detail of this dialog;
            // the original source catalogue stays active.
            playlists_.SetActive(source_index);
            settings_.player.playlist_scan_count =
                static_cast<int>(playlists_.Size());
        }
        const bool imported = CommitImportedTracks(std::move(transferred),
            target_index, playlists_.At(target_index).Tracks().size(), false,
            ImportPlayback::none);
        if (imported && command == kPlaylistMoveToList) {
            playlists_.SetActive(source_index);
            DeleteSelectedPlaylistRows();
        }
        settings_.player.active_playlist =
            static_cast<int>(playlists_.ActiveSlot());
        if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
        return true;
    }
    if (command == kPlaylistNewList) {
        auto format = ResourceText(0x8192);
        wchar_t title[128]{};
        if (!format.empty()) swprintf_s(title, format.c_str(), playlists_.Size() + 1);
        const size_t created = playlists_.NewList(
            title[0] ? title : ResourceText(0x813a));
        settings_.player.playlist_scan_count = static_cast<int>(playlists_.Size());
        settings_.player.active_playlist = static_cast<int>(playlists_.ActiveSlot());
        playlist_selection_.reset();
        playlist_selection_anchor_.reset();
        playlist_selected_rows_.clear();
        playlist_scroll_ = 0;
        RefreshPlaylist();
        // FUN_0048468A resets the list item count, activates the new final
        // item, focuses ListCtrl 10241 and sends LVM_EDITLABELW(index).
        BeginPlaylistListEdit(created);
        return true;
    }
    if (command == kPlaylistAddList || command == kPlaylistOpenList) {
        // 00484740 appends every selected playlist, whereas 00484B23 loads a
        // single file into (and replaces) the current catalogue entry.
        ChoosePlaylistFile(command == kPlaylistOpenList);
        return true;
    }
    if (command == kPlaylistActivateList) {
        // FUN_00485148 queries the selected PlayLists row at command time.
        // A blank-area context menu has no hit item but may retain that native
        // selection, so it must not depend solely on playlist_context_list_.
        const auto target = playlist_context_list_
            ? playlist_context_list_ : playlist_list_selection_;
        if (target && *target < playlists_.Size()) SwitchPlaylist(*target);
        return true;
    }
    if (command == kPlaylistRenameList) {
        // FUN_0048516E edits the selected catalogue item in place.  A title
        // context menu selects its hit item before command routing; toolbar
        // invocation falls back to the active catalogue item.
        BeginPlaylistListEdit(
            playlist_context_list_.value_or(playlists_.ActiveIndex()));
        return true;
    }
    if (command == kPlaylistDeleteList) {
        const size_t target = playlist_context_list_.value_or(playlists_.ActiveIndex());
        if (playlists_.Delete(target)) {
            if (playing_playlist_index_) {
                if (*playing_playlist_index_ == target) {
                    Stop();
                    ClearPersistedPlaybackIdentity();
                    current_.reset();
                    playing_playlist_index_.reset();
                    opened_track_.reset();
                } else if (*playing_playlist_index_ > target) {
                    --*playing_playlist_index_;
                }
            }
            settings_.player.playlist_scan_count = static_cast<int>(playlists_.Size());
            settings_.player.active_playlist = static_cast<int>(playlists_.ActiveSlot());
            playlist_selection_.reset();
            playlist_selection_anchor_.reset();
            playlist_selected_rows_.clear();
            playlist_scroll_ = 0;
            RestorePlaylistRowSelection();
            RefreshPlaylist();
        }
        return true;
    }
    if (command == kPlaylistSaveList) {
        const size_t target = playlist_context_list_.value_or(
            playlists_.ActiveIndex());
        if (target >= playlists_.Size()) return true;
        auto filter = BuildDialogFilter(ResourceModule(), {
            {0x8125, L"*.ttpl;*.ttbl;*.m3u;*.m3u8"},
            {0x8124, L"*.*"}});
        ModernSaveFileOptions dialog;
        dialog.owner = playlist_window_;
        dialog.filters = ParseLegacyDialogFilter(
            std::span<const wchar_t>(filter.data(), filter.size()));
        dialog.default_extension = L"ttpl";
        dialog.initial_path = PreferredDialogHistory(
            settings_.history.playlist_path, file_dialog_initial_directory_);
        if (const auto path = ModernSaveFile(dialog)) {
            settings_.history.playlist_path = *path;
            file_dialog_initial_directory_ = *path;
            try {
                playlists_.At(target).SaveToFile(
                    *path, settings_.playlist.tag_title_format,
                    settings_.playlist.default_title_format,
                    settings_.playlist.save_relative_path,
                    settings_.playlist.save_tags);
            } catch (const std::exception&) {
                const auto message = ResourceText(0x828e);
                MessageBoxW(playlist_window_, message.c_str(),
                            ResourceText(0x80).c_str(), MB_OK | MB_ICONERROR);
            }
        }
        return true;
    }
    if (command == kPlaylistSaveAllLists) {
        ModernFolderOptions dialog;
        dialog.owner = playlist_window_;
        dialog.initial_path = PreferredDialogHistory(
            settings_.history.playlist_path, file_dialog_initial_directory_);
        const auto selected = ModernPickFolder(dialog);
        if (!selected) return true;
        settings_.history.playlist_path = selected->path;
        file_dialog_initial_directory_ = selected->path;
        const auto prompt = FormatPlaylistResource(ResourceText(0x8196),
                                                    selected->path.c_str());
        if (MessageBoxW(playlist_window_, prompt.c_str(),
                        ResourceText(0x80).c_str(),
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return true;
        }
        bool failed{};
        for (const auto& entry : playlists_.Entries()) {
            auto name = SafePlaylistFileName(entry.playlist.Title());
            if (name.empty()) name = ResourceText(0x813a);
            const auto target = selected->path / (name + L".ttpl");
            try {
                entry.playlist.SaveToFile(
                    target, settings_.playlist.tag_title_format,
                    settings_.playlist.default_title_format,
                    settings_.playlist.save_relative_path,
                    settings_.playlist.save_tags);
            } catch (const std::exception&) {
                failed = true;
            }
        }
        if (failed) {
            MessageBoxW(playlist_window_, ResourceText(0x828e).c_str(),
                        ResourceText(0x80).c_str(), MB_OK | MB_ICONERROR);
        }
        return true;
    }
    if ((command >= kPlaylistSortTitle &&
         command <= kPlaylistSortDuration) ||
        command == kPlaylistShuffle) {
        if (ActivePlaylist().Tracks().size() <= 1) return true;
        const bool sorts_playing = playing_playlist_index_ &&
            *playing_playlist_index_ == playlists_.ActiveIndex();
        if (command == kPlaylistShuffle) {
            playlist_last_sort_command_ = 0;
            playlist_sort_ascending_ = false;
            static_cast<void>(ActivePlaylist().Shuffle());
        } else {
            const bool ascending = command == playlist_last_sort_command_
                ? !playlist_sort_ascending_ : true;
            playlist_last_sort_command_ = command;
            playlist_sort_ascending_ = ascending;
            const auto key = static_cast<playlist::SortKey>(
                command - kPlaylistSortTitle);
            if (key == playlist::SortKey::display_title) {
                const std::wstring unknown = ResourceText(0x81c8);
                std::vector<std::wstring> titles;
                titles.reserve(ActivePlaylist().Tracks().size());
                for (const auto& track : ActivePlaylist().Tracks()) {
                    titles.push_back(FormatPlaylistTitle(
                        track, settings_.playlist, unknown).text);
                }
                static_cast<void>(ActivePlaylist().Sort(
                    key, ascending, titles));
            } else {
                static_cast<void>(ActivePlaylist().Sort(key, ascending));
            }
        }
        playlists_.MarkDirty();
        if (sorts_playing) current_ = ActivePlaylist().PlayingRow();
        // ListView_SortItems keeps LVIS_SELECTED/LVIS_FOCUSED on numerical
        // rows.  004866FC only searches for the playing object again and
        // ensures that new row is visible; selection identity is not moved.
        RememberPlaylistRow(playlists_.ActiveIndex(), playlist_selection_);
        if (const auto playing = ActivePlaylist().PlayingRow(); playing && skin_) {
            RECT client{};
            GetClientRect(playlist_window_, &client);
            const auto metrics = MakePlaylistGeometry(skin_->Playlist(),
                settings_.playlist.split_on_lists, client.right,
                client.bottom, ActivePlaylist().Tracks().size());
            if (*playing < playlist_scroll_) playlist_scroll_ = *playing;
            const size_t visible = static_cast<size_t>(metrics.visible_rows);
            if (*playing >= playlist_scroll_ + visible)
                playlist_scroll_ = *playing - visible + 1;
        }
        RefreshPlaylist();
        return true;
    }
    if (command == kPlaylistSortLists) {
        if (playlists_.Size() <= 1) return true;
        const auto slot_at = [this](std::optional<size_t> index)
            -> std::optional<size_t> {
            if (!index || *index >= playlists_.Entries().size())
                return std::nullopt;
            return playlists_.Entries()[*index].slot;
        };
        const auto playing_slot = slot_at(playing_playlist_index_);
        const auto selected_slot = slot_at(playlist_list_selection_);
        const auto focus_slot = slot_at(playlist_list_focus_);
        const auto context_slot = slot_at(playlist_context_list_);
        playlists_.SortByTitle(playlist_list_sort_ascending_);
        playlist_list_sort_ascending_ = !playlist_list_sort_ascending_;
        const auto find_slot = [this](std::optional<size_t> slot)
            -> std::optional<size_t> {
            if (!slot) return std::nullopt;
            const auto& entries = playlists_.Entries();
            const auto found = std::find_if(entries.begin(), entries.end(),
                [slot](const auto& entry) { return entry.slot == *slot; });
            return found == entries.end() ? std::nullopt :
                std::optional<size_t>{static_cast<size_t>(found - entries.begin())};
        };
        playing_playlist_index_ = find_slot(playing_slot);
        playlist_list_selection_ = find_slot(selected_slot);
        playlist_list_focus_ = find_slot(focus_slot);
        playlist_context_list_ = find_slot(context_slot);
        settings_.player.active_playlist =
            static_cast<int>(playlists_.ActiveSlot());
        if (playlist_list_control_ && playlist_list_focus_)
            SendMessageW(playlist_list_control_, LVM_ENSUREVISIBLE,
                         *playlist_list_focus_, FALSE);
        if (playlist_window_) InvalidateRect(playlist_window_, nullptr, FALSE);
        return true;
    }
    if (command >= kPlaylistRenameTitle &&
        command <= kPlaylistRenameCustom) {
        if (playlist_selected_rows_.empty()) return true;
        std::wstring pattern;
        if (command == kPlaylistRenameTitle) pattern = L"%T";
        else if (command == kPlaylistRenameArtistTitle)
            pattern = L"%A - %T";
        else if (command == kPlaylistRenameTitleArtist)
            pattern = L"%T - %A";
        else {
            PlaylistTextDialogState state{kPlaylistRenamePatternControl,
                                          playlist_rename_pattern_};
            if (DialogBoxParamW(ResourceModule(), MAKEINTRESOURCEW(224),
                    playlist_window_, PlaylistTextDialogProc,
                    reinterpret_cast<LPARAM>(&state)) != IDOK) return true;
            playlist_rename_pattern_ = state.value;
            pattern = std::move(state.value);
        }

        std::vector<playlist::Track> selected_tracks;
        selected_tracks.reserve(playlist_selected_rows_.size());
        for (const size_t row : playlist_selected_rows_) {
            if (const auto* track = VisiblePlaylistTrack(row))
                selected_tracks.push_back(*track);
        }
        std::optional<std::wstring> playing_identity;
        if (const auto* playing = PlaybackTrackForUi())
            playing_identity = PlaylistTrackIdentity(*playing);
        const bool renames_playing = settings_.playlist.library_mode
            ? playing_identity && std::any_of(
                selected_tracks.begin(), selected_tracks.end(),
                [&playing_identity](const auto& track) {
                    return PlaylistTrackIdentity(track) == *playing_identity;
                })
            : playing_playlist_index_ && current_ &&
                *playing_playlist_index_ == playlists_.ActiveIndex() &&
                playlist_selected_rows_.contains(*current_);
        if (renames_playing) {
            Stop();
        }
        bool changed{};
        for (const auto& snapshot : selected_tracks) {
            if (LooksLikeUrl(snapshot.path.wstring())) continue;
            std::wstring base;
            try {
                base = FormatPlaylistFileName(snapshot, pattern);
            } catch (const std::exception&) {
                continue;
            }
            if (base.empty()) continue;
            const auto target = snapshot.path.parent_path() /
                (base + snapshot.path.extension().wstring());
            if (_wcsicmp(snapshot.path.c_str(), target.c_str()) == 0) continue;
            if (!MoveFileW(snapshot.path.c_str(), target.c_str())) continue;

            for (size_t list = 0; list < playlists_.Size(); ++list) {
                auto& tracks = playlists_.At(list).Tracks();
                unsigned int edits{};
                for (size_t row = 0; row < tracks.size(); ++row) {
                    if (_wcsicmp(tracks[row].path.c_str(),
                                 snapshot.path.c_str()) == 0 &&
                        playlists_.At(list).SetPath(row, target)) {
                        ++edits;
                    }
                }
                if (edits != 0) playlists_.MarkDirty(list, edits);
            }
            static_cast<void>(UpdateMediaLibraryTrackPath(snapshot, target));
            if (opened_track_ &&
                _wcsicmp(opened_track_->path.c_str(),
                         snapshot.path.c_str()) == 0) {
                opened_track_->path = target;
            }
            changed = true;
        }
        if (changed) {
            RefreshPlaylist();
            RefreshPlaybackUi();
        }
        return true;
    }
    if (command >= kPlaylistRatingFirst &&
        command <= kPlaylistRatingLast) {
        const int rating = static_cast<int>(
            command - kPlaylistRatingFirst + 1);
        unsigned int changed{};
        for (const size_t row : playlist_selected_rows_) {
            if (ActivePlaylist().SetRating(row, rating)) ++changed;
        }
        if (changed != 0) {
            playlists_.MarkDirty(playlists_.ActiveIndex(), changed);
            if (playlist_window_)
                InvalidateRect(playlist_window_, nullptr, FALSE);
        }
        return true;
    }
    if (command == kPlaylistClickRating) {
        settings_.playlist.click_rating = !settings_.playlist.click_rating;
        return true;
    }
    if (command == kPlaylistLibraryMode) {
        // 0048788C exits a full-screen host before 0047F766 switches the
        // ordinary PlayLists/Files controls and the media-library tree.
        // Decoder and current-playlist identity are left untouched.
        const bool entering = !settings_.playlist.library_mode;
        if (entering && !settings_.library.enabled) {
            if (MessageBoxW(playlist_window_, ResourceText(0x81cd).c_str(),
                    ResourceText(0x80).c_str(),
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return true;
            }
            settings_.library.enabled = true;
        }
        if (fullscreen_mode_ != 0) SetFullScreenMode(0);
        SetMediaLibraryMode(entering);
        return true;
    }
    if (command == kPlaylistSendToFolder) {
        if (playlist_selected_rows_.empty()) return true;
        ModernFolderOptions dialog;
        dialog.owner = playlist_window_;
        dialog.initial_path = PreferredDialogHistory(
            settings_.history.folder, file_dialog_initial_directory_);
        const auto selected = ModernPickFolder(dialog);
        if (!selected) return true;
        settings_.history.folder = selected->path;
        file_dialog_initial_directory_ = selected->path;

        std::vector<playlist::Track> selected_tracks;
        selected_tracks.reserve(playlist_selected_rows_.size());
        for (const size_t row : playlist_selected_rows_) {
            if (const auto* track = VisiblePlaylistTrack(row))
                selected_tracks.push_back(*track);
        }
        std::vector<std::filesystem::path> local_paths;
        std::wstring first_url;
        local_paths.reserve(selected_tracks.size());
        for (const auto& track : selected_tracks) {
            if (track.path.empty()) continue;
            if (LooksLikeUrl(track.path.wstring())) {
                if (first_url.empty()) first_url = track.path.wstring();
            } else {
                local_paths.push_back(track.path);
            }
        }
        if (local_paths.empty() && first_url.empty()) return true;

        std::optional<std::wstring> playing_identity;
        if (HasPlaybackTrack()) {
            playing_identity = PlaylistTrackIdentity(
                PlaybackPlaylist().Tracks()[*current_]);
        }
        const auto explicit_lyric = associated_lyric_path_;

        // Resolve lyric sources while the audio paths still exist.  A Shell
        // target is allowed to return MOVE; after that Drop the original
        // source path no longer exists and the deterministic local-lyric
        // lookup cannot be reconstructed from the selected CPlayItem alone.
        std::vector<std::filesystem::path> lyrics;
        std::set<std::wstring> unique_lyrics;
        const auto append_lyric = [&](const std::filesystem::path& path) {
            if (path.empty() || LooksLikeUrl(path.wstring())) return;
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return;
            std::error_code error;
            auto absolute = std::filesystem::absolute(path, error);
            if (error) absolute = path;
            auto key = absolute.lexically_normal().wstring();
            std::transform(key.begin(), key.end(), key.begin(), towlower);
            if (unique_lyrics.insert(std::move(key)).second)
                lyrics.push_back(path);
        };
        for (const auto& track : selected_tracks) {
            const bool is_playing_item = playing_identity &&
                *playing_identity == PlaylistTrackIdentity(track);
            for (const auto& lyric : FindPlaylistAssociatedLyrics(
                    track, settings_.lyric,
                    is_playing_item ? explicit_lyric
                                    : std::filesystem::path{})) {
                append_lyric(lyric);
            }
        }

        auto* data = new (std::nothrow) PlaylistDragDataObject(
            std::move(local_paths), std::move(first_url));
        if (!data) return true;
        IDropTarget* target{};
        const HRESULT bound = BindShellDropTargetForDirectory(
            playlist_window_, selected->path.c_str(), &target);
        if (FAILED(bound) || !target) {
            data->Release();
            return true;
        }
        const auto outcome = DropPlaylistDataOnShellTarget(target, data);
        target->Release();
        data->Release();
        if (!outcome.drop_called || FAILED(outcome.result)) return true;

        // FUN_0048713E completes the primary Shell Drop before it dispatches
        // command 0x7F13 for an accepted MOVE.  The optional lyric transfer
        // is a distinct, later SHFileOperation and never replaces a rejected
        // target with a fixed FO_COPY operation.
        if (outcome.remove_source) {
            if (settings_.playlist.library_mode)
                static_cast<void>(
                    HandleMediaLibraryCommand(kPlaylistDeleteSelected));
            else
                DeleteSelectedPlaylistRows();
        }

        const DWORD destination_attributes =
            GetFileAttributesW(selected->path.c_str());
        if (destination_attributes == INVALID_FILE_ATTRIBUTES ||
            (destination_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return true;
        }
        const auto question = FormatPlaylistResource(ResourceText(0x819c),
                                                      selected->path.c_str());
        if (MessageBoxW(playlist_window_, question.c_str(),
                ResourceText(0x80).c_str(),
                MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return true;
        }

        if (!lyrics.empty()) {
            auto source = ShellPathList(lyrics);
            std::vector<std::filesystem::path> lyric_destinations;
            lyric_destinations.reserve(lyrics.size());
            for (const auto& lyric : lyrics)
                lyric_destinations.push_back(selected->path /
                                               lyric.filename());
            auto destination = ShellPathList(lyric_destinations);
            SHFILEOPSTRUCTW operation{};
            operation.hwnd = playlist_window_;
            operation.wFunc = FO_COPY;
            operation.pFrom = source.data();
            operation.pTo = destination.data();
            // 0048713E writes 0x41: parallel source/destination multistrings
            // and an undoable Shell operation, retaining normal collision UI.
            operation.fFlags = FOF_MULTIDESTFILES | FOF_ALLOWUNDO;
            static_cast<void>(SHFileOperationW(&operation));
        }
        return true;
    }
    if (command == kPlaylistSelectAll || command == kPlaylistSelectNone ||
        command == kPlaylistSelectInvert) {
        if (command == kPlaylistSelectNone) {
            playlist_selected_rows_.clear();
        } else if (command == kPlaylistSelectAll) {
            playlist_selected_rows_.clear();
            for (size_t index = 0; index < ActivePlaylist().Tracks().size(); ++index)
                playlist_selected_rows_.insert(index);
            // FUN_00486209 returns keyboard focus to the Files ListCtrl after
            // applying LVIS_SELECTED to every row.
            if (playlist_track_control_) SetFocus(playlist_track_control_);
        } else {
            std::set<size_t> inverted;
            for (size_t index = 0; index < ActivePlaylist().Tracks().size(); ++index)
                if (!playlist_selected_rows_.contains(index)) inverted.insert(index);
            playlist_selected_rows_ = std::move(inverted);
        }
        InvalidateRect(playlist_window_, nullptr, FALSE);
        return true;
    }
    if (command == kPlaylistFind || command == kPlaylistQuickFind) {
        ShowPlaylistFindDialog(command == kPlaylistQuickFind);
        return true;
    }
    if (command == kPlaylistFindNext) {
        if (playlist_find_dialog_ && IsWindow(playlist_find_dialog_)) {
            PostMessageW(playlist_find_dialog_, WM_COMMAND, IDOK, 0);
        } else if (playlist_find_text_[0] != L'\0') {
            static_cast<void>(FindNextPlaylistTrack(playlist_find_.Flags));
        } else {
            ShowPlaylistFindDialog(playlist_find_quick_);
        }
        return true;
    }
    if (command >= kPlaylistModeSingle && command <= kPlaylistModeShuffle) {
        settings_.player.play_mode = static_cast<int>(command - kPlaylistModeSingle);
        return true;
    }
    if (command == kPlaylistAutoSwitchList) {
        settings_.player.auto_switch_list = !settings_.player.auto_switch_list;
        return true;
    }
    if (command == kPlaylistPlayFollowCursor) {
        settings_.player.play_follow_cursor = !settings_.player.play_follow_cursor;
        if (playlist_track_control_) {
            LONG_PTR style = GetWindowLongPtrW(
                playlist_track_control_, GWL_STYLE);
            if (settings_.player.play_follow_cursor)
                style |= LVS_SHOWSELALWAYS;
            else
                style &= ~static_cast<LONG_PTR>(LVS_SHOWSELALWAYS);
            SetWindowLongPtrW(playlist_track_control_, GWL_STYLE, style);
            // FUN_00483AF8 repaints selected rows which intersect the client
            // immediately after modifying style bit 8.  Our owner paint uses
            // that same bit, so invalidating the visible surface is exact and
            // avoids walking native virtual rows a second time.
            if (playlist_window_)
                InvalidateRect(playlist_window_, nullptr, FALSE);
        }
        return true;
    }
    return false;
}

bool PlayerWindow::HandlePlaylistSendToCommand(UINT command) {
    if (!playlist_send_to_catalog_.Contains(command)) return true;

    std::vector<std::filesystem::path> local_paths;
    std::wstring first_url;
    local_paths.reserve(playlist_selected_rows_.size());
    for (const size_t row : playlist_selected_rows_) {
        if (row >= ActivePlaylist().Tracks().size()) continue;
        const auto& path = ActivePlaylist().Tracks()[row].path;
        if (path.empty()) continue;
        if (LooksLikeUrl(path.wstring())) {
            if (first_url.empty()) first_url = path.wstring();
        } else {
            local_paths.push_back(path);
        }
    }
    if (local_paths.empty() && first_url.empty()) return true;

    auto* data = new (std::nothrow) PlaylistDragDataObject(
        std::move(local_paths), std::move(first_url));
    if (!data) return true;
    const auto outcome = playlist_send_to_catalog_.Drop(command, data);
    data->Release();

    // 0048713E routes an accepted MOVE through command 0x7F13.  COPY, LINK,
    // a rejected DragEnter and a failed Drop all preserve the source rows.
    if (outcome && outcome->remove_source) DeleteSelectedPlaylistRows();
    return true;
}

void PlayerWindow::InvokePlaylistToolbar(size_t button, POINT screen_point) {
    if (button >= 7) return;
    playlist_send_to_catalog_.Clear();
    HMENU menu = DetachPopup(LoadMenuW(ResourceModule(),
        MAKEINTRESOURCEW(kMenuPlaylistToolbar)), static_cast<int>(button));
    if (!menu) return;
    PreparePlaylistMenu(menu);
    BeginPopupMenuStyle(menu);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screen_point.x, screen_point.y, 0, playlist_window_, nullptr);
    EndPopupMenuStyle();
    DestroyMenu(menu);
    if (command != 0 && !HandlePlaylistCommand(command))
        HandleContextCommand(command);
    playlist_send_to_catalog_.Clear();
}

void PlayerWindow::ChoosePlaylistFile(bool replace_active) {
    auto filter = BuildDialogFilter(ResourceModule(), {
        {0x8125, L"*.ttpl;*.ttbl;*.m3u;*.m3u8"},
        {0x8124, L"*.*"}});
    ModernOpenFileOptions dialog;
    dialog.owner = playlist_window_;
    dialog.filters = ParseLegacyDialogFilter(
        std::span<const wchar_t>(filter.data(), filter.size()));
    dialog.initial_path = PreferredDialogHistory(
        settings_.history.playlist_path, file_dialog_initial_directory_);
    dialog.allow_multiple = !replace_active;
    const auto paths = ModernOpenFiles(dialog);
    if (!paths || paths->empty()) return;
    settings_.history.playlist_path = FileSelectionHistory(*paths);
    file_dialog_initial_directory_ = settings_.history.playlist_path;
    try {
        bool loaded{};
        if (replace_active) {
            const size_t target = playlists_.ActiveIndex();
            const bool replaced_playing = playing_playlist_index_ &&
                *playing_playlist_index_ == target;
            loaded = playlists_.ReplaceList(target, paths->front(),
                {settings_.playlist.ignore_bad_files});
            if (loaded && replaced_playing) {
                Stop();
                ClearPersistedPlaybackIdentity();
                current_.reset();
                playing_playlist_index_.reset();
                opened_track_.reset();
            }
        } else {
            for (const auto& path : *paths) {
                if (playlists_.AddList(path,
                    {settings_.playlist.ignore_bad_files}) !=
                    playlist::PlaylistStore::npos) loaded = true;
            }
        }
        if (!loaded) return;
        settings_.player.playlist_scan_count = static_cast<int>(playlists_.Size());
        settings_.player.active_playlist = static_cast<int>(playlists_.ActiveSlot());
        playlist_scroll_ = 0;
        playlist_selection_ = ActivePlaylist().CurrentRow()
            ? ActivePlaylist().CurrentRow() : ActivePlaylist().PlayingRow();
        playlist_selected_rows_.clear();
        playlist_selection_anchor_ = playlist_selection_;
        if (playlist_selection_) playlist_selected_rows_.insert(*playlist_selection_);
        RefreshPlaylist();
    } catch (const std::exception&) {
        const auto message = ResourceText(0x828e);
        MessageBoxW(playlist_window_, message.c_str(), ResourceText(0x80).c_str(),
                    MB_OK | MB_ICONERROR);
    }
}

void PlayerWindow::LoadStoredPlaylist() {
    auto directory = FindRuntimePath(L"PlayList");
    if (directory.empty()) {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                                static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size()) return;
        executable.resize(length);
        directory = std::filesystem::path(executable).parent_path() / L"PlayList";
    }
    playlists_.Load(directory, settings_.player.playlist_scan_count,
                    settings_.player.active_playlist,
                    settings_.playlist.legacy_playlist_generation);
    const auto default_title = ResourceText(0x813a);
    for (size_t index = 0; index < playlists_.Size(); ++index) {
        if (playlists_.At(index).Title().empty())
            playlists_.At(index).SetTitle(default_title);
    }
    settings_.player.playlist_scan_count = static_cast<int>(playlists_.Size());
    settings_.player.active_playlist = static_cast<int>(playlists_.ActiveSlot());
    RefreshPlaylist();
    if (const auto row = ActivePlaylist().PlayingRow()) {
        // The TTBL fourth DWORD is CPlayList +0x1c (last-playing row), not
        // the transient +0x20 ListCtrl caret.  Restore the player/display
        // identity without turning that persisted marker into a selected
        // row.  0047F294 clears LVIS state and merely ensures +0x1c is
        // visible when +0x20 is absent.
        SelectTrack(*row, false);
        RememberPlaylistRow(playlists_.ActiveIndex(), std::nullopt);
        playlist_selection_.reset();
        playlist_selection_anchor_.reset();
        playlist_selected_rows_.clear();
        if (playlist_view_)
            SendMessageW(playlist_view_, LB_SETCURSEL,
                         static_cast<WPARAM>(-1), 0);
        if (playlist_window_)
            InvalidateRect(playlist_window_, nullptr, FALSE);
    }
}

void PlayerWindow::SaveStoredPlaylist() {
    playlists_.FlushDirty(true);
}

LRESULT CALLBACK PlayerWindow::PlaylistWindowProc(HWND window, UINT message,
                                                   WPARAM wparam, LPARAM lparam) {
    PlayerWindow* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<PlayerWindow*>(create->lpCreateParams);
        self->playlist_window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandlePlaylistMessage(message, wparam, lparam, window)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK PlayerWindow::PlaylistEditProc(HWND window, UINT message,
                                                 WPARAM wparam, LPARAM lparam,
                                                 UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<PlayerWindow*>(data);
    if (!self) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        self->FinishPlaylistListEdit(true);
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        self->FinishPlaylistListEdit(false);
        return 0;
    }
    if (message == WM_KILLFOCUS && !self->playlist_list_edit_finishing_) {
        // The native ListView label editor ends a focus-loss edit with a
        // null pszText.  00489B06 consequently leaves the stored title
        // untouched; only Enter supplies text for a commit.
        self->FinishPlaylistListEdit(false);
        return 0;
    }
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_CHAR || message == WM_KEYUP || message == WM_PASTE ||
        message == WM_CUT || message == WM_CLEAR || message == WM_SETTEXT)
        self->LayoutPlaylistListEdit();
    return result;
}

} // namespace ttplayer::ui
