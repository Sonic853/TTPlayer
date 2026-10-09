#pragma once
#include "ttplayer/settings/settings.h"
#include <functional>
#include <memory>

namespace ttplayer::lyrics {
struct LegacyLyricResponse {
    std::string body;
    std::wstring title_header, url_header, redirect;
};
// Original ttp_lrcsh uses WinINet, including its HTTP/0.9 response support.
// Each search owns an in-memory cookie jar; downloads retain that same jar.
class LegacyLyricSession {
public:
    LegacyLyricSession();
    ~LegacyLyricSession();
    LegacyLyricResponse Fetch(const std::wstring& url,
        const settings::NetworkSettings& network, const std::function<bool()>& canceled);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
