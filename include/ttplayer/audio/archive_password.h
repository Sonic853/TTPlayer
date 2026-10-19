#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>
#include <vector>
#include <ttpcomm/archive_api.h>

namespace ttplayer::audio {
// Background catalogue scans may use verified/saved passwords and volume
// mappings, but must not open modal dialogs on behalf of a directory watcher.
class ScopedArchiveBackgroundRead {
public:
    ScopedArchiveBackgroundRead() noexcept;
    ~ScopedArchiveBackgroundRead();
    ScopedArchiveBackgroundRead(const ScopedArchiveBackgroundRead&) = delete;
    ScopedArchiveBackgroundRead& operator=(const ScopedArchiveBackgroundRead&) = delete;
};
struct ArchivePasswordAnswer {std::wstring password;bool remember{};};
using ArchivePasswordPrompt=std::function<std::optional<ArchivePasswordAnswer>(
    const std::filesystem::path&,bool,const std::function<bool()>&)>;
void ConfigureArchivePasswords(const std::filesystem::path& store,ArchivePasswordPrompt prompt={},
    std::function<void(HRESULT)> storage_error={});
HRESULT WithArchivePassword(const std::filesystem::path& archive,
    const std::function<HRESULT(const wchar_t*)>& operation,const std::function<bool()>& canceled={});
// DPAPI-protected per-request IPC; plaintext never enters argv or packet files.
std::wstring ExportArchivePasswordPacket(const std::filesystem::path& logical,
    const std::function<bool()>& canceled={});
void ImportArchivePasswordPacket(const std::filesystem::path& logical,std::wstring_view packet);
void RenameArchivePasswordSession(const std::filesystem::path& before,const std::filesystem::path& after);
HRESULT ArchivePasswordStorageResult();
using ArchiveVolumePrompt=std::function<std::optional<std::filesystem::path>(
    const std::filesystem::path&,const std::filesystem::path&,const std::function<bool()>&)>;
void ConfigureArchiveVolumes(ArchiveVolumePrompt prompt={});
void DiscardArchiveVolumes(const std::filesystem::path& archive);
void AliasArchiveSession(const std::filesystem::path& before,const std::filesystem::path& after);
// Cache provenance: no password bytes escape through this API. Configuration,
// accepted credentials, volume remapping and integrity failures advance the epoch.
struct ArchiveVolumeMappingIdentity { std::filesystem::path expected, actual; };
uint64_t ArchiveSessionGeneration() noexcept;
std::vector<ArchiveVolumeMappingIdentity> ArchiveSessionVolumeMappings(
    const std::filesystem::path& archive);
// Synchronous ABI 3 calls only: canceled must outlive the function call.
TtpCommArchiveVolumeOptions ArchiveVolumeOptions(const std::function<bool()>* canceled);
}
