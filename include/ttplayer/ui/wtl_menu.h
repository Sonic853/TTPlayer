#pragma once
#include <windows.h>

namespace ttplayer::ui {
BOOL TrackPlayerPopupMenuEx(HMENU menu, UINT flags, int x, int y,
                           HWND owner, LPTPMPARAMS parameters);
BOOL TrackPlayerPopupMenu(HMENU menu, UINT flags, int x, int y,
                         int reserved, HWND owner, const RECT* exclude);
void SetMenuCommandEnabled(HMENU menu, UINT command, bool enabled);
void SetMenuCommandChecked(HMENU menu, UINT command, bool checked);
}
