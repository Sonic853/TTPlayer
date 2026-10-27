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
bool HasOsuExportSupport() noexcept;
HRESULT ExportOsuLibrary(unsigned source,const std::filesystem::path& root,
    const std::vector<std::wstring>& identities,const std::filesystem::path& destination,
    bool single_file,bool replace,const std::function<bool()>& canceled,
    const std::function<void(uint32_t,uint64_t,uint64_t,const wchar_t*)>& progress,
    const std::function<void(const std::filesystem::path&)>& completed) noexcept;
HRESULT ReadOsuLibrary(unsigned source,const std::filesystem::path& root,
    std::vector<OsuLibraryMedia>& result,const std::function<bool()>& canceled) noexcept;
}
