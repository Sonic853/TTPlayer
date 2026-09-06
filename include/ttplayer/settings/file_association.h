#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "ttplayer/plugins/plugin_manager.h"

namespace ttplayer::settings {

// FileAssociationBackend intentionally does not mutate the registry from its
// constructor or from any Query* member.  The options page can therefore be
// populated without changing the user's shell configuration; writes happen
// only after an explicit Set*/Create*/Remove* call.
enum class FileAssociationError {
    none,
    invalid_argument,
    registry,
    shell,
    com,
    filesystem,
};

struct FileAssociationResult {
    FileAssociationError error{FileAssociationError::none};
    DWORD native_error{ERROR_SUCCESS};
    HRESULT hresult{S_OK};
    bool changed{};
    std::wstring operation;
    std::wstring message;

    [[nodiscard]] bool Succeeded() const noexcept {
        return error == FileAssociationError::none;
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

struct AssociationQuery {
    FileAssociationResult result;
    bool associated{};
    // For the real HKCU\Software\Classes store, this reports whether the
    // shell currently resolves the extension to this executable.  Windows 8+
    // may leave it false when a protected UserChoice selects another app even
    // though the TTPlayer ProgID is registered correctly.
    bool effective{};
    std::wstring extension;       // normalized, without the leading dot
    std::wstring current_prog_id;
    std::wstring backup_prog_id;
    std::wstring managed_prog_id;
    std::wstring icon;
};

struct AssociableExtension {
    // Reader-advertised spelling, without the leading dot.  The original
    // association tree preserves case and duplicate extensions; the registry
    // backend normalizes only when it performs a query/write.
    std::wstring extension;
    std::wstring description;
    std::filesystem::path module_path;
};

// Reproduces FUN_0049D2FA's per-reader pattern scan. Formats which were not
// registered by a loaded reader never reach this function. Video, archive and
// RealMedia container extensions deliberately omitted by the original page
// are filtered. Reader order, spelling and duplicates are retained.
[[nodiscard]] std::vector<AssociableExtension> BuildAssociableExtensions(
    const std::vector<plugins::ReaderFormat>& reader_formats);

enum class ShellIntegrationTarget {
    audio_cd,
    directory,
};

struct ShellIntegrationQuery {
    FileAssociationResult result;
    bool associated{};
    std::wstring command;
};

struct ShellVerbLabels {
    // Callers should normally supply resource strings 0x81A8 and 0x81A9 from
    // ttpres.dll.  These fallbacks keep the backend independently usable.
    std::wstring playback{L"Play with TTPlayer"};
    std::wstring add_to_playlist{L"Add to TTPlayer playlist"};
};

enum class ShortcutLocation {
    desktop,
    programs,
    quick_launch,
};

struct ShortcutOptions {
    std::wstring display_name;
    std::wstring arguments;
    std::wstring description;
    std::filesystem::path working_directory;
    std::filesystem::path icon_path;
    int icon_index{};
    // Used only for ShortcutLocation::programs.  It must be a relative path
    // and may be empty, matching the original direct Programs-folder link.
    std::filesystem::path programs_subdirectory;
};

struct ShortcutQuery {
    FileAssociationResult result;
    bool exists{};
    std::filesystem::path path;
};

struct FileAssociationBackendOptions {
    // Exposed primarily so isolated Windows-Sandbox tests can redirect all
    // registry traffic away from the live shell classes.  Production callers
    // should retain the default per-user Classes store.
    std::wstring current_user_classes_subkey{L"Software\\Classes"};
    std::wstring prog_id_prefix{L"Audio"};
    bool notify_shell{true};
};

class FileAssociationBackend {
public:
    explicit FileAssociationBackend(
        std::filesystem::path executable,
        std::wstring application_name = L"TTPlayer",
        FileAssociationBackendOptions options = {});

    [[nodiscard]] AssociationQuery QueryExtension(
        std::wstring_view extension) const;

    // description is the reader-supplied type text shown in Explorer.
    // icon may be a complete DefaultIcon value.  An empty icon uses
    // "<executable>,0".  Disabling restores the prior default ProgID and the
    // pre-existing contents of any class keys touched while enabling.
    [[nodiscard]] FileAssociationResult SetExtensionAssociation(
        std::wstring_view extension, bool enabled,
        std::wstring_view description = {}, std::wstring_view icon = {},
        const ShellVerbLabels& labels = {});

    // Changes only this backend's managed ProgID.  It never overwrites the
    // icon of an unrelated current association.
    [[nodiscard]] FileAssociationResult SetExtensionIcon(
        std::wstring_view extension, std::wstring_view icon);

    [[nodiscard]] ShellIntegrationQuery QueryShellIntegration(
        ShellIntegrationTarget target) const;
    [[nodiscard]] FileAssociationResult SetShellIntegration(
        ShellIntegrationTarget target, bool enabled,
        const ShellVerbLabels& labels = {});

    [[nodiscard]] ShortcutQuery QueryShortcut(
        ShortcutLocation location,
        const ShortcutOptions& options = {}) const;
    [[nodiscard]] FileAssociationResult CreateShortcut(
        ShortcutLocation location,
        const ShortcutOptions& options = {}) const;
    [[nodiscard]] FileAssociationResult RemoveShortcut(
        ShortcutLocation location,
        const ShortcutOptions& options = {}) const;

    // FUN_0049cd41/FUN_0049d8c0 use this exact shell notification after a
    // batch association change.  Set* calls invoke it automatically when
    // options.notify_shell is true and a registry value actually changed.
    static void NotifyShellAssociationsChanged() noexcept;

    [[nodiscard]] const std::filesystem::path& Executable() const noexcept {
        return executable_;
    }

private:
    std::filesystem::path executable_;
    std::wstring application_name_;
    FileAssociationBackendOptions options_;
};

} // namespace ttplayer::settings
