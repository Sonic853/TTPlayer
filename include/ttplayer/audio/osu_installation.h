#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace ttplayer::audio {
enum class OsuExecutableKind { none, stable, lazer };
struct OsuInstallation {
    std::wstring executable,library_directory;
    OsuExecutableKind kind{};
};
bool HasOsuInstallationSupport() noexcept;
OsuExecutableKind IdentifyOsuExecutable(std::wstring_view path) noexcept;
// NULL executable discovers a default; empty preserves an explicit empty path.
HRESULT ReadOsuInstallation(unsigned source,const wchar_t* executable,
    OsuInstallation& result,const wchar_t* roaming_root=nullptr) noexcept;
}
