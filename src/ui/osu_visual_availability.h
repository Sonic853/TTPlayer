#pragma once

#include "ttplayer/audio/osu_installation.h"
#include "ttplayer/audio/osu_download_page.h"
#include "ttplayer/settings/settings.h"
#include "ttplayer/audio/osz_archive.h"
#include <atomic>

namespace ttplayer::ui::detail {
// Refresh on startup/settings edits, never from the render loop. Library and
// OSZ capabilities remain independent of the installed game executables.
inline std::atomic<unsigned> osu_executable_kinds{};
inline void ApplyOsuInstallationDefaults(settings::OsuLibrarySettings& paths) {
    // Settings loading also runs in the updater/workers. Only the player UI
    // asks the optional DLL to discover defaults, so an updater cannot lock it.
    for(unsigned source=1;source<=2;++source) {
        auto& executable=source==1?paths.stable_executable:paths.lazer_executable;
        auto& directory=source==1?paths.stable_songs:paths.lazer_files;
        const unsigned exe_bit=1u<<(source-1),directory_bit=exe_bit<<2;
        const bool find_executable=executable.empty() && !(paths.explicit_paths&exe_bit);
        const bool find_directory=directory.empty() && !(paths.explicit_paths&directory_bit);
        if(!find_executable && !find_directory)continue;
        audio::OsuInstallation found;
        if(FAILED(audio::ReadOsuInstallation(source,find_executable?nullptr:executable.c_str(),found)))continue;
        if(find_executable)executable=std::move(found.executable);
        if(find_directory)directory=std::move(found.library_directory);
    }
}
inline bool RefreshOsuExecutableIdentity(const settings::OsuLibrarySettings& paths) noexcept {
    using audio::IdentifyOsuExecutable;
    const unsigned kinds=static_cast<unsigned>(IdentifyOsuExecutable(paths.stable_executable)) |
        (static_cast<unsigned>(IdentifyOsuExecutable(paths.lazer_executable))<<2);
    return osu_executable_kinds.exchange(kinds)!=kinds;
}
inline audio::OsuExecutableKind OsuExecutableIdentity(bool lazer) noexcept {
    return static_cast<audio::OsuExecutableKind>(
        (osu_executable_kinds.load()>>(lazer?2:0))&3);
}
inline HRESULT AvailableOsuDownloadPage(BSTR* name,BSTR* url) noexcept {
    if(name)*name=nullptr;
    if(url)*url=nullptr;
    if(!name||!url)return E_POINTER;
    return osu_executable_kinds.load()?audio::GetOsuDownloadPage(name,url):S_FALSE;
}
inline HRESULT RecognizeAvailableOsuDownload(const wchar_t* page,const wchar_t* url,BSTR* filename) noexcept {
    if(!filename)return E_POINTER;
    *filename=nullptr;
    return osu_executable_kinds.load()?audio::RecognizeOsuDownload(page,url,filename):S_FALSE;
}
inline bool OsuVisualAvailable() noexcept {
    return osu_executable_kinds.load()!=0 && audio::HasOsuVisualSupport();
}
inline bool OsuLazerAvailable() noexcept {
    return osu_executable_kinds.load()!=0 && audio::HasOsuLazerSupport();
}
}
