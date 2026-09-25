#pragma once

#include "ttplayer/ui/wtl_runtime.h"
#include <atlctrlx.h>
#include <memory>

namespace ttplayer::ui {

// Match the Options footer: normal dialog font, pure blue, no underline or
// visited-color change. WTL supplies mouse, focus and keyboard navigation.
class PlainBlueLink final : public WTL::CHyperLinkImpl<PlainBlueLink> {
public:
    PlainBlueLink() : CHyperLinkImpl(HLINK_NOTUNDERLINED | HLINK_NOTOOLTIP | HLINK_SINGLELINE) {}

    void Init() {
        CHyperLinkImpl::Init();
        m_clrLink = m_clrVisited = RGB(0, 0, 255);
    }

    void OnFinalMessage(HWND) override { delete this; }

    static bool Attach(HWND control, const wchar_t* url) {
        if (!control || FAILED(EnsureWtlRuntime())) return false;
        auto link = std::make_unique<PlainBlueLink>();
        if (!link->SetHyperLink(url) || !link->SubclassWindow(control)) return false;
        link.release();
        return true;
    }
};

} // namespace ttplayer::ui
