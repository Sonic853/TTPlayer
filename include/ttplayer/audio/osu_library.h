#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>
namespace ttplayer::audio {
struct OsuLibraryMedia {
    std::filesystem::path audio,beatmap,background;
    std::wstring identity,title,artist,album,creator;
    uint64_t duration_ms{};
};
bool HasOsuLibrarySupport() noexcept;
HRESULT ReadOsuLibrary(unsigned source,const std::filesystem::path& root,
    std::vector<OsuLibraryMedia>& result,const std::function<bool()>& canceled) noexcept;
}
