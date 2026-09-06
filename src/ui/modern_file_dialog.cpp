#include "modern_file_dialog.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

#include <objbase.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace ttplayer::ui::detail {
namespace {

using Microsoft::WRL::ComPtr;

class ScopedStaApartment {
public:
    ScopedStaApartment() noexcept
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedStaApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }
    ScopedStaApartment(const ScopedStaApartment&) = delete;
    ScopedStaApartment& operator=(const ScopedStaApartment&) = delete;

    [[nodiscard]] bool available() const noexcept {
        // RPC_E_CHANGED_MODE means this thread is already an MTA.  The shell
        // file-dialog implementation is an STA UI component, so do not try to
        // create or show it from the incompatible apartment.
        return SUCCEEDED(result_);
    }

private:
    HRESULT result_;
};

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return path;
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    return error ? path : absolute;
}

void ConfigureInitialLocation(IFileDialog* dialog,
                              const std::filesystem::path& supplied,
                              bool use_file_name) {
    if (!dialog || supplied.empty()) return;

    const auto initial = AbsolutePath(supplied);
    const DWORD attributes = GetFileAttributesW(initial.c_str());
    const bool is_directory = attributes != INVALID_FILE_ATTRIBUTES &&
                              (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const auto folder = is_directory ? initial : initial.parent_path();
    if (!folder.empty()) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                folder.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()))) &&
            item) {
            // SetFolder, rather than SetDefaultFolder, makes the application
            // history authoritative even if Explorer persisted another folder
            // for this dialog class.
            static_cast<void>(dialog->SetFolder(item.Get()));
        }
    }
    if (use_file_name && !is_directory && !initial.filename().empty())
        static_cast<void>(dialog->SetFileName(initial.filename().c_str()));
}

std::vector<COMDLG_FILTERSPEC> MakeFilterSpecifications(
    const std::vector<ModernDialogFilter>& filters) {
    std::vector<COMDLG_FILTERSPEC> specifications;
    specifications.reserve(filters.size());
    for (const auto& filter : filters) {
        if (!filter.label.empty() && !filter.pattern.empty()) {
            specifications.push_back(
                COMDLG_FILTERSPEC{filter.label.c_str(), filter.pattern.c_str()});
        }
    }
    return specifications;
}

bool ConfigureFilters(IFileDialog* dialog,
                      const std::vector<COMDLG_FILTERSPEC>& specifications) {
    if (!dialog) return false;
    if (specifications.empty()) return true;
    if (specifications.size() > std::numeric_limits<UINT>::max()) return false;
    if (FAILED(dialog->SetFileTypes(
            static_cast<UINT>(specifications.size()), specifications.data())))
        return false;
    return SUCCEEDED(dialog->SetFileTypeIndex(1));
}

bool ConfigureCommon(IFileDialog* dialog, const std::wstring& title,
                     const std::wstring& default_extension) {
    if (!dialog) return false;
    if (!title.empty() && FAILED(dialog->SetTitle(title.c_str()))) return false;
    if (!default_extension.empty()) {
        const wchar_t* extension = default_extension.c_str();
        while (*extension == L'.') ++extension;
        if (*extension && FAILED(dialog->SetDefaultExtension(extension)))
            return false;
    }
    return true;
}

std::optional<std::filesystem::path> ShellItemPath(IShellItem* item) {
    if (!item) return std::nullopt;
    PWSTR value{};
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) || !value)
        return std::nullopt;
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(
        value, &CoTaskMemFree);
    std::filesystem::path path(owned.get());
    return path.empty() ? std::nullopt
                        : std::optional<std::filesystem::path>{std::move(path)};
}

} // namespace

std::vector<ModernDialogFilter> ParseLegacyDialogFilter(
    std::span<const wchar_t> filter) {
    std::vector<ModernDialogFilter> result;
    size_t cursor{};
    while (cursor < filter.size() && filter[cursor] != L'\0') {
        const size_t label_begin = cursor;
        while (cursor < filter.size() && filter[cursor] != L'\0') ++cursor;
        if (cursor == filter.size()) break;
        std::wstring label(filter.data() + label_begin, cursor - label_begin);
        ++cursor;
        const size_t pattern_begin = cursor;
        while (cursor < filter.size() && filter[cursor] != L'\0') ++cursor;
        if (cursor == filter.size()) break;
        std::wstring pattern(filter.data() + pattern_begin,
                             cursor - pattern_begin);
        ++cursor;
        while (!pattern.empty() && pattern.back() == L';') pattern.pop_back();
        if (!label.empty() && !pattern.empty())
            result.push_back({std::move(label), std::move(pattern)});
    }
    return result;
}

std::optional<std::vector<std::filesystem::path>> ModernOpenFiles(
    const ModernOpenFileOptions& options) {
    ScopedStaApartment apartment;
    if (!apartment.available()) return std::nullopt;

    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.GetAddressOf()))) ||
        !dialog)
        return std::nullopt;

    FILEOPENDIALOGOPTIONS flags{};
    if (FAILED(dialog->GetOptions(&flags))) return std::nullopt;
    flags |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
             FOS_NOCHANGEDIR;
    if (options.allow_multiple) flags |= FOS_ALLOWMULTISELECT;
    else flags &= ~FOS_ALLOWMULTISELECT;
    // Both the COMDLG_FILTERSPEC array and the strings to which it points stay
    // alive until Show() returns.  Some shell implementations copy the array
    // in SetFileTypes, but the interface does not require callers to depend on
    // that implementation detail.
    const auto filter_specs = MakeFilterSpecifications(options.filters);
    if (FAILED(dialog->SetOptions(flags)) ||
        !ConfigureFilters(dialog.Get(), filter_specs) ||
        !ConfigureCommon(dialog.Get(), options.title,
                         options.default_extension))
        return std::nullopt;
    ConfigureInitialLocation(dialog.Get(), options.initial_path, true);
    if (FAILED(dialog->Show(options.owner))) return std::nullopt;

    std::vector<std::filesystem::path> paths;
    if (!options.allow_multiple) {
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(item.GetAddressOf()))) return std::nullopt;
        if (auto path = ShellItemPath(item.Get())) paths.push_back(std::move(*path));
    } else {
        ComPtr<IShellItemArray> items;
        if (FAILED(dialog->GetResults(items.GetAddressOf())) || !items)
            return std::nullopt;
        DWORD count{};
        if (FAILED(items->GetCount(&count))) return std::nullopt;
        paths.reserve(count);
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(index, item.GetAddressOf())))
                return std::nullopt;
            auto path = ShellItemPath(item.Get());
            if (!path) return std::nullopt;
            paths.push_back(std::move(*path));
        }
    }
    return paths.empty()
        ? std::nullopt
        : std::optional<std::vector<std::filesystem::path>>{std::move(paths)};
}

std::optional<std::filesystem::path> ModernOpenFile(
    const ModernOpenFileOptions& options) {
    auto single = options;
    single.allow_multiple = false;
    auto paths = ModernOpenFiles(single);
    if (!paths || paths->empty()) return std::nullopt;
    return std::move(paths->front());
}

std::optional<std::filesystem::path> ModernSaveFile(
    const ModernSaveFileOptions& options) {
    ScopedStaApartment apartment;
    if (!apartment.available()) return std::nullopt;

    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.GetAddressOf()))) ||
        !dialog)
        return std::nullopt;

    FILEOPENDIALOGOPTIONS flags{};
    if (FAILED(dialog->GetOptions(&flags))) return std::nullopt;
    flags |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    if (options.overwrite_prompt) flags |= FOS_OVERWRITEPROMPT;
    else flags &= ~FOS_OVERWRITEPROMPT;
    const auto filter_specs = MakeFilterSpecifications(options.filters);
    if (FAILED(dialog->SetOptions(flags)) ||
        !ConfigureFilters(dialog.Get(), filter_specs) ||
        !ConfigureCommon(dialog.Get(), options.title,
                         options.default_extension))
        return std::nullopt;
    ConfigureInitialLocation(dialog.Get(), options.initial_path, true);
    if (FAILED(dialog->Show(options.owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return std::nullopt;
    return ShellItemPath(item.Get());
}

std::optional<ModernFolderResult> ModernPickFolder(
    const ModernFolderOptions& options) {
    ScopedStaApartment apartment;
    if (!apartment.available()) return std::nullopt;

    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.GetAddressOf()))) ||
        !dialog)
        return std::nullopt;

    FILEOPENDIALOGOPTIONS flags{};
    if (FAILED(dialog->GetOptions(&flags))) return std::nullopt;
    flags |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
             FOS_NOCHANGEDIR;
    flags &= ~FOS_ALLOWMULTISELECT;
    if (FAILED(dialog->SetOptions(flags)) ||
        !ConfigureCommon(dialog.Get(), options.title, {}))
        return std::nullopt;
    ConfigureInitialLocation(dialog.Get(), options.initial_path, false);

    ComPtr<IFileDialogCustomize> customize;
    if (!options.checkbox_label.empty()) {
        if (FAILED(dialog.As(&customize)) || !customize ||
            FAILED(customize->AddCheckButton(options.checkbox_id,
                                              options.checkbox_label.c_str(),
                                              options.checkbox_checked)))
            return std::nullopt;
    }
    if (FAILED(dialog->Show(options.owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return std::nullopt;
    auto path = ShellItemPath(item.Get());
    if (!path) return std::nullopt;

    BOOL checked = options.checkbox_checked;
    if (customize) {
        if (FAILED(customize->GetCheckButtonState(options.checkbox_id, &checked)))
            return std::nullopt;
    }
    return ModernFolderResult{std::move(*path), checked != FALSE};
}

} // namespace ttplayer::ui::detail
