#include "ttplayer/ui/shell_send_to.h"

#include <algorithm>
#include <iterator>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <utility>
#include <vector>

namespace ttplayer::ui {
namespace {

template <class Interface>
class ComOwner final {
public:
    ComOwner() = default;
    ~ComOwner() { Reset(); }
    ComOwner(const ComOwner&) = delete;
    ComOwner& operator=(const ComOwner&) = delete;

    Interface** Put() noexcept {
        Reset();
        return &value_;
    }
    Interface* Get() const noexcept { return value_; }
    Interface* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    void Reset() noexcept {
        if (value_) value_->Release();
        value_ = nullptr;
    }
    Interface* value_{};
};

HICON DefaultFolderIcon() {
    SHFILEINFOW information{};
    if (SHGetFileInfoW(L"", FILE_ATTRIBUTE_DIRECTORY, &information,
            sizeof(information), SHGFI_ICON | SHGFI_SMALLICON |
                                     SHGFI_USEFILEATTRIBUTES) == 0) {
        return nullptr;
    }
    return information.hIcon;
}

HICON ShellItemIcon(PCIDLIST_ABSOLUTE item) {
    if (!item) return nullptr;
    SHFILEINFOW information{};
    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(item), 0, &information,
            sizeof(information), SHGFI_PIDL | SHGFI_ICON |
                                     SHGFI_SMALLICON) == 0) {
        return nullptr;
    }
    return information.hIcon;
}

} // namespace

ShellDropResult DropPlaylistDataOnShellTarget(
    IDropTarget* target, IDataObject* data) noexcept {
    ShellDropResult outcome;
    if (!target || !data) {
        outcome.result = E_INVALIDARG;
        return outcome;
    }

    POINTL point{};
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_LINK;
    HRESULT result = target->DragEnter(data, MK_LBUTTON, point, &effect);
    if (SUCCEEDED(result) && effect != DROPEFFECT_NONE) {
        outcome.drop_called = true;
        result = target->Drop(data, MK_LBUTTON, point, &effect);
        // 0048713E calls DragLeave after Drop as well.  Some modern targets
        // treat it as redundant, but retaining it preserves the legacy ABI.
        static_cast<void>(target->DragLeave());
    }
    outcome.result = result;
    outcome.effect = effect;
    outcome.remove_source = SUCCEEDED(result) && effect == DROPEFFECT_MOVE;
    return outcome;
}

HRESULT BindShellDropTargetForDirectory(
    HWND owner, PCWSTR path, IDropTarget** target) noexcept {
    if (!target) return E_POINTER;
    *target = nullptr;
    if (!path || path[0] == L'\0') return E_INVALIDARG;

    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error == ERROR_SUCCESS
                ? ERROR_PATH_NOT_FOUND : error);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return HRESULT_FROM_WIN32(ERROR_DIRECTORY);

    PIDLIST_ABSOLUTE absolute{};
    HRESULT result = SHParseDisplayName(path, nullptr, &absolute, 0, nullptr);
    if (FAILED(result) || !absolute) return FAILED(result) ? result : E_FAIL;

    ComOwner<IShellFolder> parent;
    PCUITEMID_CHILD child{};
    result = SHBindToParent(absolute, IID_IShellFolder,
        reinterpret_cast<void**>(parent.Put()), &child);
    if (SUCCEEDED(result) && parent && child) {
        PCUITEMID_CHILD children[]{child};
        result = parent->GetUIObjectOf(owner, 1, children, IID_IDropTarget,
            nullptr, reinterpret_cast<void**>(target));
        if (SUCCEEDED(result) && !*target) result = E_NOINTERFACE;
    } else if (SUCCEEDED(result)) {
        result = E_FAIL;
    }
    CoTaskMemFree(absolute);

    if (FAILED(result) && *target) {
        (*target)->Release();
        *target = nullptr;
    }
    return result;
}

struct ShellSendToCatalog::Impl {
    struct Target {
        UINT command{};
        IDropTarget* drop_target{};
    };

    ~Impl() { Clear(); }

    void Clear() noexcept {
        for (auto& target : targets) {
            if (target.drop_target) target.drop_target->Release();
        }
        targets.clear();
    }

    size_t Enumerate(HMENU menu, HWND owner, int folder_id,
                     SHCONTF flags, UINT& next_command, UINT last_command,
                     const IconSink& icon_sink) {
        if (!menu || next_command > last_command) return 0;

        ComOwner<IShellFolder> desktop;
        if (FAILED(SHGetDesktopFolder(desktop.Put())) || !desktop) return 0;

        PIDLIST_ABSOLUTE folder_item{};
        if (FAILED(SHGetSpecialFolderLocation(owner, folder_id,
                                              &folder_item)) ||
            !folder_item) {
            return 0;
        }

        ComOwner<IShellFolder> folder;
        const HRESULT bind = desktop->BindToObject(
            folder_item, nullptr, IID_IShellFolder,
            reinterpret_cast<void**>(folder.Put()));
        if (FAILED(bind) || !folder) {
            CoTaskMemFree(folder_item);
            return 0;
        }

        ComOwner<IEnumIDList> enumeration;
        const HRESULT enumerated = folder->EnumObjects(
            owner, flags, enumeration.Put());
        if (FAILED(enumerated) || !enumeration) {
            CoTaskMemFree(folder_item);
            return 0;
        }

        size_t added{};
        PITEMID_CHILD child{};
        ULONG fetched{};
        while (next_command <= last_command &&
               enumeration->Next(1, &child, &fetched) == S_OK) {
            SFGAOF attributes = static_cast<SFGAOF>(0x20000177U);
            PCUITEMID_CHILD children[]{child};
            const HRESULT attributes_result = folder->GetAttributesOf(
                1, children, &attributes);
            if (FAILED(attributes_result) ||
                (attributes & SFGAO_DROPTARGET) == 0) {
                CoTaskMemFree(child);
                child = nullptr;
                continue;
            }

            wchar_t label[MAX_PATH]{};
            STRRET display{};
            if (FAILED(folder->GetDisplayNameOf(child, SHGDN_NORMAL,
                                                &display)) ||
                FAILED(StrRetToBufW(&display, child, label,
                                    static_cast<UINT>(std::size(label)))) ||
                label[0] == L'\0') {
                CoTaskMemFree(child);
                child = nullptr;
                continue;
            }

            IDropTarget* drop_target{};
            const HRESULT target_result = folder->GetUIObjectOf(
                owner, 1, children, IID_IDropTarget, nullptr,
                reinterpret_cast<void**>(&drop_target));
            if (FAILED(target_result) || !drop_target) {
                CoTaskMemFree(child);
                child = nullptr;
                continue;
            }

            const UINT command = next_command;
            if (!AppendMenuW(menu, MF_STRING, command, label)) {
                drop_target->Release();
                CoTaskMemFree(child);
                child = nullptr;
                continue;
            }

            targets.push_back({command, drop_target});
            ++next_command;
            ++added;

            if (icon_sink) {
                PIDLIST_ABSOLUTE absolute = ILCombine(folder_item, child);
                if (absolute) {
                    if (HICON icon = ShellItemIcon(absolute)) {
                        icon_sink(command, icon);
                        DestroyIcon(icon);
                    }
                    CoTaskMemFree(absolute);
                }
            }
            CoTaskMemFree(child);
            child = nullptr;
        }
        if (child) CoTaskMemFree(child);
        CoTaskMemFree(folder_item);
        return added;
    }

    std::vector<Target> targets;
};

ShellSendToCatalog::ShellSendToCatalog() : impl_(std::make_unique<Impl>()) {}
ShellSendToCatalog::~ShellSendToCatalog() = default;

size_t ShellSendToCatalog::PopulateMenu(
    HMENU menu, HWND owner, UINT first_command, UINT last_command,
    const IconSink& icon_sink) {
    if (!menu || first_command > last_command) return 0;
    Clear();

    // FUN_004671DB walks backwards and keeps resource positions zero and one
    // ("choose folder" plus its separator).  This also makes repeated
    // WM_INITMENUPOPUP notifications idempotent.
    for (int position = GetMenuItemCount(menu) - 1; position >= 2; --position)
        DeleteMenu(menu, position, MF_BYPOSITION);

    if (icon_sink) {
        if (HICON icon = DefaultFolderIcon()) {
            icon_sink(first_command - 1, icon);
            DestroyIcon(icon);
        }
    }

    UINT next_command = first_command;
    size_t added = impl_->Enumerate(
        menu, owner, CSIDL_SENDTO,
        static_cast<SHCONTF>(SHCONTF_FOLDERS | SHCONTF_NONFOLDERS),
        next_command, last_command, icon_sink);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    added += impl_->Enumerate(
        menu, owner, CSIDL_DRIVES, SHCONTF_FOLDERS,
        next_command, last_command, icon_sink);
    return added;
}

void ShellSendToCatalog::Clear() noexcept {
    if (impl_) impl_->Clear();
}

bool ShellSendToCatalog::Contains(UINT command) const noexcept {
    if (!impl_) return false;
    return std::ranges::any_of(impl_->targets,
        [command](const Impl::Target& target) {
            return target.command == command;
        });
}

std::optional<ShellDropResult> ShellSendToCatalog::Drop(
    UINT command, IDataObject* data) const noexcept {
    if (!impl_) return std::nullopt;
    const auto found = std::ranges::find_if(impl_->targets,
        [command](const Impl::Target& target) {
            return target.command == command;
        });
    if (found == impl_->targets.end()) return std::nullopt;
    return DropPlaylistDataOnShellTarget(found->drop_target, data);
}

} // namespace ttplayer::ui
