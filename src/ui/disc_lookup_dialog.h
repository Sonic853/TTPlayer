#pragma once
#include "ttplayer/audio/disc_lookup.h"
namespace ttplayer::ui {
// Returns only after the worker has stopped. A result means local save succeeded.
std::optional<audio::DiscRelease> ShowDiscLookupDialog(HWND owner,
    const std::filesystem::path&, const plugins::PluginManager*, HMODULE,
    const settings::NetworkSettings&, bool automatic = false);
}
