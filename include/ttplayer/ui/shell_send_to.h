#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <windows.h>
#include <oleidl.h>

namespace ttplayer::ui {

struct ShellDropResult {
    HRESULT result{E_FAIL};
    DWORD effect{DROPEFFECT_NONE};
    bool drop_called{};
    bool remove_source{};
};

// CPlayerWnd's Send To command calls the target directly instead of routing
// the selected files through SHFileOperation.  Keep that small COM contract
// independently testable: the original supplies MK_LBUTTON, a zero screen
// point and COPY|LINK as the target's initial effect mask.
[[nodiscard]] ShellDropResult DropPlaylistDataOnShellTarget(
    IDropTarget* target, IDataObject* data) noexcept;

// Resolves an ordinary filesystem directory through the Shell namespace and
// asks its parent folder for the same IDropTarget used by the legacy
// CPlayerWnd "choose folder" path.  On every failure *target remains null.
[[nodiscard]] HRESULT BindShellDropTargetForDirectory(
    HWND owner, PCWSTR path, IDropTarget** target) noexcept;

class ShellSendToCatalog final {
public:
    using IconSink = std::function<void(UINT command, HICON icon)>;

    ShellSendToCatalog();
    ~ShellSendToCatalog();
    ShellSendToCatalog(const ShellSendToCatalog&) = delete;
    ShellSendToCatalog& operator=(const ShellSendToCatalog&) = delete;

    // Replaces every row after the two resource skeleton entries.  It then
    // enumerates the user's SendTo namespace, a separator, and My Computer's
    // drive children in the order returned by the shell.
    size_t PopulateMenu(HMENU menu, HWND owner, UINT first_command,
                        UINT last_command, const IconSink& icon_sink);
    void Clear() noexcept;
    [[nodiscard]] bool Contains(UINT command) const noexcept;
    [[nodiscard]] std::optional<ShellDropResult> Drop(
        UINT command, IDataObject* data) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ttplayer::ui
