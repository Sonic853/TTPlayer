#pragma once

#include "ttplayer/plugins/plugin_manager.h"
#include <mutex>
#include <optional>

namespace ttplayer::lyrics {
struct LyricService {
    std::wstring name, url, key;
    std::filesystem::path module, storage;
    bool read_only{};
    size_t legacy_provider{};
    std::optional<int> order;
    bool operator==(const LyricService&) const = default;
};
struct ServiceFile {
    std::filesystem::path module, ini;
    bool exists{}, valid{true};
    std::string original;
};
struct ServiceCatalog {
    // DLL entries first, then INI entries; stable ordering within each group.
    std::vector<LyricService> entries;
    std::vector<ServiceFile> files;
    std::wstring error;
};
struct CatalogJob {
    std::mutex mutex;
    std::optional<ServiceCatalog> result;
};
std::filesystem::path ServiceIniPath(std::filesystem::path module);
std::wstring ServiceKey(const LyricService& service);
bool ValidServiceUrl(std::wstring_view url);
bool CanMoveService(const std::vector<LyricService>& entries, int index, int direction) noexcept;
bool MoveService(std::vector<LyricService>& entries, int index, int direction);
ServiceCatalog ReadServiceCatalog(const std::vector<plugins::LyricSearchProviderInfo>& providers);
// Only INI files represented by baseline can be changed. Reject edits to DLL
// rows and detect external file changes before committing a complete XML file.
ServiceCatalog SaveServiceCatalog(const ServiceCatalog& baseline,
                                 const std::vector<LyricService>& edited);
std::shared_ptr<CatalogJob> ReadServiceCatalogAsync(std::vector<plugins::LyricSearchProviderInfo> providers);
std::shared_ptr<CatalogJob> SaveServiceCatalogAsync(ServiceCatalog baseline,
                                                 std::vector<LyricService> edited);
} // namespace ttplayer::lyrics
