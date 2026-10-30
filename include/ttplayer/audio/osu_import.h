#pragma once
#include <filesystem>
#include <functional>
#include <windows.h>
namespace ttplayer::audio {
struct OsuImportResult {
    unsigned state{},retention{};
    bool already_present{};
    HRESULT detail{E_PENDING};
    std::filesystem::path task_directory;
};
bool HasOsuImportSupport() noexcept;
HRESULT ImportOsuPackage(unsigned source,const std::filesystem::path& executable,
    const std::filesystem::path& archive,bool keep,bool referenced,HMODULE ttpcomm,
    const std::function<bool()>& canceled,
    const std::function<void(unsigned,uint64_t,uint64_t,const wchar_t*)>& progress,
    OsuImportResult& result) noexcept;
}
