#pragma once
#include <windows.h>
#include <prsht.h>

namespace ttplayer::ui {
// Retains each page's resource module, template, callback and application data.
INT_PTR ShowWtlPropertySheet(const PROPSHEETHEADERW& header);
HWND CreateWtlDialog(HINSTANCE resources, LPCWSTR name, HWND parent,
                     DLGPROC handler, LPARAM data);
}
