#pragma once
#include "ttplayer/settings/settings.h"
#include <functional>
#include <string>
namespace ttplayer::net {
struct HttpResponse { unsigned status{}; std::string body, retry_after; };
HttpResponse GetHttps(const std::wstring& url, const std::string& agent,
    const settings::NetworkSettings&, const std::function<bool()>& canceled);
}
