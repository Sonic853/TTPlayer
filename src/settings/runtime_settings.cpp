#include "ttplayer/settings/settings.h"

#include <exception>

namespace ttplayer::settings {

Settings LoadRuntimeSettings(const std::filesystem::path& runtime_directory) {
    Settings defaults;
    if (runtime_directory.empty()) return defaults;
    const auto destination = runtime_directory / kSettingsFileName;
    defaults.source_path = destination;
    auto source = destination;
    std::error_code error;
    if (!std::filesystem::exists(destination, error)) {
        if (error) return defaults;
        const auto previous = runtime_directory / L"TTPlayer.xml";
        if (!std::filesystem::is_regular_file(previous, error) || error) return defaults;
        // Preserve unknown XML fields byte-for-byte. Never replace an existing
        // new file, including one created concurrently by another invocation.
        std::filesystem::copy_file(previous, destination,
                                   std::filesystem::copy_options::none, error);
        if (error) {
            std::error_code destination_error;
            if (!std::filesystem::exists(destination, destination_error)) {
                // Read-only installations may import in memory. Any later
                // save still targets the NEW file, never the old one.
                source = previous;
            }
        }
    }
    try {
        auto settings = LoadLegacyXml(source);
        settings.source_path = destination;
        return settings;
    } catch (const std::exception&) {
        // A malformed NEW file must not resurrect stale settings from the old one.
        return defaults;
    }
}

} // namespace ttplayer::settings
