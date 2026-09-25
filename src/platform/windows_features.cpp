#include "ttplayer/platform/windows_features.h"
#include <windows.h>

namespace ttplayer::platform {
const WindowsFeatures& CurrentWindowsFeatures() noexcept {
    static const WindowsFeatures features = [] {
        using VersionFunction = LONG (WINAPI*)(OSVERSIONINFOW*);
        const auto query = reinterpret_cast<VersionFunction>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW info{sizeof(info)};
        if (!query || query(&info) != 0) return WindowsFeatures{};
        return WindowsFeatures{{info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber}};
    }();
    return features;
}
} // namespace ttplayer::platform
