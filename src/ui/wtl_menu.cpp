#include "ttplayer/ui/wtl_runtime.h"
#include "ttplayer/ui/wtl_menu.h"
#include <atlctrlw.h>

namespace ttplayer::ui {
namespace {
// WTL owns tracking, its CBT/message hooks and keyboard focus. The player's
// renderer alone owns itemData, including lazily populated skin/track menus.
class PlayerCommandBar final : public WTL::CCommandBarCtrlImpl<PlayerCommandBar> {
public:
    using Base = WTL::CCommandBarCtrlImpl<PlayerCommandBar>;
    HWND owner{};
    DECLARE_WND_SUPERCLASS(L"TTPlayer.WTL.CommandBar", GetWndClassName())

    BOOL ProcessWindowMessage(HWND window, UINT message, WPARAM wp, LPARAM lp,
                              LRESULT& result, DWORD map = 0) override {
        // The owner keeps its original dispatch. Do not let the stock parent
        // map convert the player's owner-draw records into WTL menu records.
        if (map == 1) return FALSE;
        if (map == 0) {
            switch (message) {
            case WM_INITMENU: case WM_INITMENUPOPUP: case WM_UNINITMENUPOPUP:
            case WM_MENUSELECT: case WM_MENUCHAR: case WM_DRAWITEM:
            case WM_MEASUREITEM: case WM_ENTERMENULOOP: case WM_EXITMENULOOP:
            case WM_ENTERIDLE: case WM_COMMAND:
                result = ::SendMessageW(owner, message, wp, lp);
                return TRUE;
            }
        }
        return Base::ProcessWindowMessage(window, message, wp, lp, result, map);
    }
};

class MenuState final : public WTL::CDynamicUpdateUI<MenuState> {
public:
    BEGIN_UPDATE_UI_MAP(MenuState)
    END_UPDATE_UI_MAP()
    MenuState(HMENU menu, UINT command) {
        UIAddUpdateElement(static_cast<WORD>(command), UPDUI_MENUPOPUP);
        const UINT current = ::GetMenuState(menu, command, MF_BYCOMMAND);
        DWORD state{};
        if (current & (MF_DISABLED | MF_GRAYED)) state |= UPDUI_DISABLED;
        if (current & MF_CHECKED) state |= UPDUI_CHECKED;
        if (current & MF_DEFAULT) state |= UPDUI_DEFAULT;
        UISetState(command, state);
    }
    void Apply(HMENU menu) {
        BOOL handled{};
        OnInitMenuPopup(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0, handled);
    }
};
}

BOOL TrackPlayerPopupMenuEx(HMENU menu, UINT flags, int x, int y,
                           HWND owner, LPTPMPARAMS parameters) {
    // WTL has one active CBT tracker per thread; recursive popups continue
    // through User32 and the existing owner, without overwriting that tracker.
    if (FAILED(EnsureWtlRuntime()) || PlayerCommandBar::s_hCreateHook)
        return ::TrackPopupMenuEx(menu, flags, x, y, owner, parameters);
    PlayerCommandBar bar;
    bar.owner = owner;
    RECT bounds{};
    if (!bar.Create(owner, bounds, nullptr, WS_CHILD | TBSTYLE_FLAT))
        return ::TrackPopupMenuEx(menu, flags, x, y, owner, parameters);
    bar.m_bImagesVisible = false;
    bar.m_hWndFocus = ::GetFocus();
    // Return the command while the temporary bar is alive, then route it to
    // the original HWND. A posted WM_COMMAND to a destroyed bar would be lost.
    const BOOL selected = bar.TrackPopupMenu(menu, flags | TPM_RETURNCMD, x, y, parameters);
    bar.GiveFocusBack();
    bar.DestroyWindow();
    if (flags & TPM_RETURNCMD) return selected;
    if (selected && !(flags & TPM_NONOTIFY))
        ::PostMessageW(owner, WM_COMMAND, static_cast<WPARAM>(selected), 0);
    return selected != 0;
}

BOOL TrackPlayerPopupMenu(HMENU menu, UINT flags, int x, int y,
                         int, HWND owner, const RECT* exclude) {
    TPMPARAMS parameters{sizeof(parameters)};
    if (exclude) parameters.rcExclude = *exclude;
    return TrackPlayerPopupMenuEx(menu, flags, x, y, owner, exclude ? &parameters : nullptr);
}

void SetMenuCommandEnabled(HMENU menu, UINT command, bool enabled) {
    if (command > MAXWORD) {
        ::EnableMenuItem(menu, command, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
        return;
    }
    MenuState state(menu, command);
    state.UIEnable(command, enabled);
    state.Apply(menu);
}
void SetMenuCommandChecked(HMENU menu, UINT command, bool checked) {
    if (command > MAXWORD) {
        ::CheckMenuItem(menu, command, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
        return;
    }
    MenuState state(menu, command);
    state.UISetCheck(command, checked);
    state.Apply(menu);
}
}
