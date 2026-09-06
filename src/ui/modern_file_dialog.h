#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <windows.h>

namespace ttplayer::ui::detail {

// IFileDialog consumes COMDLG_FILTERSPEC, whose strings must remain alive for
// the duration of Show().  Keep ownership in this small value type instead of
// leaking pointers into the legacy double-NUL filter buffers.
struct ModernDialogFilter {
    std::wstring label;
    std::wstring pattern;
};

std::vector<ModernDialogFilter> ParseLegacyDialogFilter(
    std::span<const wchar_t> filter);

template <size_t Size>
std::vector<ModernDialogFilter> ParseLegacyDialogFilter(
    const wchar_t (&filter)[Size]) {
    return ParseLegacyDialogFilter(std::span<const wchar_t>(filter, Size));
}

struct ModernOpenFileOptions {
    HWND owner{};
    std::vector<ModernDialogFilter> filters;
    std::filesystem::path initial_path;
    std::wstring title;
    std::wstring default_extension;
    bool allow_multiple{};
};

// Cancellation and dialog-creation failure both return nullopt.  Callers keep
// their existing no-op-on-cancel behavior and perform all post-selection
// import/state transitions themselves.
std::optional<std::vector<std::filesystem::path>> ModernOpenFiles(
    const ModernOpenFileOptions& options);
std::optional<std::filesystem::path> ModernOpenFile(
    const ModernOpenFileOptions& options);

struct ModernSaveFileOptions {
    HWND owner{};
    std::vector<ModernDialogFilter> filters;
    std::filesystem::path initial_path;
    std::wstring title;
    std::wstring default_extension;
    bool overwrite_prompt{true};
};

std::optional<std::filesystem::path> ModernSaveFile(
    const ModernSaveFileOptions& options);

struct ModernFolderOptions {
    HWND owner{};
    std::filesystem::path initial_path;
    std::wstring title;
    // A non-empty label adds a native IFileDialogCustomize checkbox.  This is
    // used by the options page's "include subfolders" folder picker.
    DWORD checkbox_id{0x5454};
    std::wstring checkbox_label;
    bool checkbox_checked{};
};

struct ModernFolderResult {
    std::filesystem::path path;
    bool checkbox_checked{};
};

std::optional<ModernFolderResult> ModernPickFolder(
    const ModernFolderOptions& options);

} // namespace ttplayer::ui::detail
