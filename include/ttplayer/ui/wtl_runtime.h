#pragma once

#include <atlbase.h>
#include <atlapp.h>
static_assert(_WTL_VER == 0x1001, "TTPlayer requires the reviewed WTL 10.01 headers");
extern WTL::CAppModule _Module;
#include <atlwin.h>
#include <atlcrack.h>
#include <atlctrls.h>
#include <atlgdi.h>
#include <atlframe.h>

namespace ttplayer::ui {
// Shared by the player and local UI test hosts. Resources remain explicit:
// EXE dialogs and ttpres.dll pages must not change each other's resource module.
HRESULT EnsureWtlRuntime();

class PlayerMessageLoop final : public WTL::CMessageLoop {
public:
    BOOL IsIdleMessage(MSG* message) const override {
        // 004B54F2 additionally excludes the playback/UI timer.
        return message->message != WM_TIMER &&
               WTL::CMessageLoop::IsIdleMessage(message);
    }
};
}
