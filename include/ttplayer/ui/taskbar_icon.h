#pragma once

#include <filesystem>
#include <string>
#include <windows.h>

namespace ttplayer::ui {
// Stable across updates/icon selections, distinct for portable installations.
std::wstring PlayerAppUserModelId(const std::filesystem::path& executable);
// Call before showing UI. Unowned DSP windows inherit this process identity,
// while the main window and pinned shortcuts use the same explicit ID.
// S_FALSE on systems without Windows 7 taskbar identities.
HRESULT InitializePlayerTaskbarIdentity();
bool HasTaskbarIconProperties();

// Updates only links targeting this executable; preserves launch parameters,
// names and placement. Exposed separately for local shell integration tests.
enum class ShortcutIconUpdate { unrelated, unchanged, updated, failed };
ShortcutIconUpdate UpdatePlayerShortcutIcon(
    const std::filesystem::path& shortcut, const std::filesystem::path& executable,
    const std::filesystem::path& icon, const std::wstring& app_id);

std::filesystem::path PublishTaskbarIcon(HWND window, HICON small_icon, HICON large_icon,
                                       const std::wstring& title);
void ClearTaskbarIconProperties(HWND window);
} // namespace ttplayer::ui
