#pragma once

#include "ttplayer/app/single_instance.h"
#include "ttplayer/plugins/plugin_manager.h"

#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace ttplayer::app {
struct ParsedCommandLine {
    std::filesystem::path file;
    std::vector<std::wstring> switches;
    bool smoke_test{};
    [[nodiscard]] bool HasSwitch(std::wstring_view name) const;
    // 004C038B/004C0DB2: /a -> 1, /e -> 2, ordinary open -> 0.
    [[nodiscard]] ULONG_PTR FileMode() const;
};

ParsedCommandLine ParseCommandLine();
int TTPlayer_RunApplicationSession(HINSTANCE instance, int show_command,
                                   SingleInstanceIpc& single_instance,
                                   HMODULE resource_module,
                                   HMODULE ttpcomm_module,
                                   const plugins::PluginManager& sound_library);
} // namespace ttplayer::app
