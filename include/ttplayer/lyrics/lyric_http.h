#pragma once
#include "ttplayer/settings/settings.h"
#include <functional>
#include <cstdint>
#include <string_view>

namespace ttplayer::lyrics {
struct HttpLyricResult { std::int32_t id{}; std::wstring artist, title; };
struct HttpLyricDownload { std::wstring text, extra_title, extra_url; };
std::wstring LyricSearchUrl(std::wstring_view base, std::wstring_view artist, std::wstring_view title);
std::int32_t LyricDownloadCode(std::uint32_t id, std::string_view utf8_artist_title);
std::wstring LyricDownloadUrl(std::wstring_view base, const HttpLyricResult& result);
// Synchronous calls are confined to OnlineSearch's worker. TLS verification
// is never disabled, including for user-defined endpoints.
std::vector<HttpLyricResult> SearchHttpLyrics(const std::wstring& base,
    std::wstring_view artist, std::wstring_view title, const settings::NetworkSettings& network,
    const std::function<bool()>& canceled);
HttpLyricDownload DownloadHttpLyric(const std::wstring& base, const HttpLyricResult& result,
    const settings::NetworkSettings& network, const std::function<bool()>& canceled);
std::string FetchLyricHttp(const std::wstring& url, const settings::NetworkSettings& network,
    const std::function<bool()>& canceled);
}
