#pragma once
#include <windows.h>
namespace ttplayer::ui {
// Lifetime is owned by the player's main window. Returns the hidden native
// notification receiver or the original-style HTML popup.
HWND ShowUpdateNotice(HWND owner);
}
