#pragma once
#include <windows.h>

namespace ttplayer::ui::detail {
// Original 00491FEF color bitmap and 0046E0C9 button rendering, shared by
// ordinary options and the visual page. Native buttons retain input handling.
void DrawOptionsColorButton(const DRAWITEMSTRUCT& item, COLORREF color);
void MakeOptionsColorButton(HWND dialog, int control);
bool InstallOptionsBitmapButton(HWND dialog, int control, HMODULE resources,
                                UINT identifier);
}
