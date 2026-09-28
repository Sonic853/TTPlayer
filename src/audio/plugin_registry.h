#pragma once

#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ttplayer::audio::detail {

// A typed, file-backed registry tree. No Windows registry keys are created.
class FileRegistry {
public:
    explicit FileRegistry(const std::filesystem::path& file);
    ~FileRegistry();
    FileRegistry(const FileRegistry&) = delete;
    FileRegistry& operator=(const FileRegistry&) = delete;
    LSTATUS Open(std::wstring path, REGSAM access, bool create,
                 HKEY* result, DWORD* disposition);
    bool Owns(HKEY key) const;
    std::wstring Path(HKEY key) const;
    LSTATUS Status(HKEY key) const;
    LSTATUS Query(HKEY key, const wchar_t* name, DWORD* type, BYTE* data, DWORD* size);
    LSTATUS ReadValue(HKEY key, const wchar_t* name, DWORD& type, std::vector<BYTE>& data);
    LSTATUS Set(HKEY key, const wchar_t* name, DWORD type, const BYTE* data, DWORD size);
    LSTATUS Close(HKEY key);
    LSTATUS EnumKey(HKEY key, DWORD index, std::wstring& name);
    LSTATUS EnumValue(HKEY key, DWORD index, std::wstring& name, DWORD& type, std::vector<BYTE>& data);
    LSTATUS DeleteValue(HKEY key, const wchar_t* name);
    LSTATUS DeleteKey(HKEY key, const wchar_t* subkey);
    LSTATUS QueryInfo(HKEY key, DWORD& subkeys, DWORD& max_subkey, DWORD& values,
                      DWORD& max_value_name, DWORD& max_value_data, bool ansi = false);
    void ConfigureDfxPaths(const std::filesystem::path& plugin, const std::filesystem::path& host);
    LSTATUS Flush();
    LSTATUS SaveStatus() const;
    bool Empty() const;
    // Shared full-hive namespace. Missing data falls back to read-only native
    // registry access; mutations and imports only change this file.
    void Import(const std::filesystem::path& file);
    // Copy the previous per-Ozone store if this shared store has no Ozone state.
    // Keep the old file intact and preserve any already imported DFX state.
    void MigrateLegacyOzone(const std::filesystem::path& file);
    void MigrateEnhancer();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Shared adapter for DSP modules and their private dependencies. Must outlive
// FreeLibrary so Quit and DLL_PROCESS_DETACH see the same file-backed settings.
class PluginRegistry {
public:
    struct Impl;
    static bool Supports(const std::filesystem::path& plugin);
    static std::unique_ptr<PluginRegistry> Attach(
        HMODULE module, const std::filesystem::path& plugin,
        const std::filesystem::path& directory = {});
    ~PluginRegistry();
    bool Flush() noexcept;
    std::wstring StorageDescription() const;
    bool ConfigureDfx(HWND owner);
    std::vector<std::wstring> Diagnostics();
    std::vector<std::wstring> HelperTrace();
    void SetTraceSink(std::function<void(std::wstring_view)> sink);
private:
    explicit PluginRegistry(std::shared_ptr<Impl> state);
    std::shared_ptr<Impl> impl_;
};
} // namespace ttplayer::audio::detail
