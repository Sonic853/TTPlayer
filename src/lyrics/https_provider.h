#pragma once
#include "ttplayer/lyrics/lyric_http.h"
#include <optional>

namespace ttplayer::lyrics {
struct HttpsProviderResponse {
    std::string body, title_header, url_header;
};
// Only the optional C ABI adapter lives in the player. Missing/invalid DLLs
// and unsupported native proxy authentication preserve the original WinHTTP.
std::optional<HttpsProviderResponse> FetchHttpsProvider(const std::wstring&,
    const settings::NetworkSettings&,const std::function<bool()>&);
}
