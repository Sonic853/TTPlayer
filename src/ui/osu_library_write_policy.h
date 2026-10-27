#pragma once

#include "track_metadata.h"
#include "ttplayer/core/text.h"
#include "ttplayer/lyrics/online_search.h"
#include "../core/environment_path.h"
#include <wincrypt.h>

namespace ttplayer::ui::detail {

inline bool OsuPathWithin(const std::filesystem::path& path,
                          const std::filesystem::path& directory) {
    if (path.empty() || directory.empty()) return false;
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) normalized = path.lexically_normal();
    error.clear();
    auto root = std::filesystem::weakly_canonical(directory, error);
    if (error) root = directory.lexically_normal();
    auto part = normalized.begin();
    for (const auto& component : root) {
        if (part == normalized.end() || _wcsicmp(part->c_str(), component.c_str()) != 0)
            return false;
        ++part;
    }
    return true;
}

// The configured root may have changed since this track was loaded. Protect
// its actual audio/beatmap/background directories as well as both game roots.
inline bool IsOsuLibrarySourcePath(const playlist::Track& track,
    const settings::Settings& settings, const std::filesystem::path& target) {
    if (!IsOsuLibraryTrack(track) || target.empty()) return false;
    for (const auto* configured : {&settings.osu_library.stable_songs,
                                   &settings.osu_library.lazer_files}) {
        if (!configured->empty() && OsuPathWithin(target,
                core::ExpandEnvironmentPath(*configured))) return true;
    }
    if (OsuPathWithin(target, track.path.parent_path())) return true;
    for (const auto& [key, value] : track.metadata) {
        if ((key == "TTPlayer.Osu.Beatmap" || key == "TTPlayer.Osu.Background") &&
            !value.empty()) {
            try {
                const auto path = std::filesystem::path(core::Utf8ToWide(value));
                if (path.is_absolute() && OsuPathWithin(target, path.parent_path())) return true;
            } catch (...) {}
        }
    }
    return false;
}

inline std::filesystem::path OsuLibraryLyricDirectory(
    const playlist::Track& track, const settings::Settings& settings,
    const std::filesystem::path& runtime) {
    if (!IsOsuLibraryTrack(track)) return {};
    // This hash is a cache key, not a security check. SHA-1 via the XP CryptoAPI
    // keeps arbitrary Unicode identities out of filenames and avoids collisions
    // between different beatmaps sharing a title or the same audio file.
    std::string identity = core::WideToUtf8(track.path.lexically_normal().native());
    identity += '\0';
    for (const auto& [key, value] : track.metadata)
        if (key == "TTPlayer.Osu.Source" || key == "TTPlayer.Osu.Identity") {
            identity += key; identity += '='; identity += value; identity += '\0';
        }
    HCRYPTPROV provider{};
    HCRYPTHASH hash{};
    BYTE bytes[20]{};DWORD size = sizeof(bytes);
    const bool good = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
        CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(identity.data()),
            static_cast<DWORD>(identity.size()), 0) &&
        CryptGetHashParam(hash, HP_HASHVAL, bytes, &size, 0) && size == sizeof(bytes);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    if (!good) return {};
    std::wstring key;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const BYTE value : bytes) { key += hex[value >> 4]; key += hex[value & 15]; }
    const auto base = settings.source_path.empty() ? runtime : settings.source_path.parent_path();
    const auto directory = base / L"Lyrics" / L"osu" / key;
    // A player installed inside the configured game store must fail closed.
    return IsOsuLibrarySourcePath(track, settings, directory) ? std::filesystem::path{} : directory;
}

inline std::filesystem::path OsuLibraryDefaultLyricPath(
    const playlist::Track& track, const settings::Settings& settings,
    const std::filesystem::path& runtime) {
    const auto directory = OsuLibraryLyricDirectory(track, settings, runtime);
    if (directory.empty()) return {};
    return directory / lyrics::LyricFileName(track.path.stem().wstring());
}

inline std::filesystem::path LyricDownloadPathForTrack(
    const playlist::Track& track, const settings::Settings& settings,
    const std::filesystem::path& runtime, std::wstring filename) {
    const auto directory = IsOsuLibraryTrack(track)
        ? OsuLibraryLyricDirectory(track, settings, runtime)
        : lyrics::DownloadDirectory(settings.lyric, track.path, runtime);
    return directory.empty() ? std::filesystem::path{}
        : directory / lyrics::LyricFileName(std::move(filename));
}

} // namespace ttplayer::ui::detail
