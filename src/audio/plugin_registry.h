#pragma once

#include <windows.h>
#include <filesystem>
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
    LSTATUS Query(HKEY key, const wchar_t* name, DWORD* type, BYTE* data, DWORD* size);
    LSTATUS ReadValue(HKEY key, const wchar_t* name, DWORD& type, std::vector<BYTE>& data);
    LSTATUS Set(HKEY key, const wchar_t* name, DWORD type, const BYTE* data, DWORD size);
    LSTATUS Close(HKEY key);
    LSTATUS Flush();
    bool Empty() const;
    // Explicit import sidecar, never execute regedit or import another branch.
    void Import(const std::filesystem::path& file);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Enabled only for the audited Ozone binary. Must outlive FreeLibrary so Quit
// and DLL_PROCESS_DETACH see the same file-backed settings.
class PluginRegistry {
public:
    struct Impl;
    static bool Supports(const std::filesystem::path& plugin);
    static std::unique_ptr<PluginRegistry> Attach(
        HMODULE module, const std::filesystem::path& plugin,
        const std::filesystem::path& directory = {});
    ~PluginRegistry();
    bool Flush() noexcept;
private:
    explicit PluginRegistry(std::shared_ptr<Impl> state);
    std::shared_ptr<Impl> impl_;
};
} // namespace ttplayer::audio::detail
