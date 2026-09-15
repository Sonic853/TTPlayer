#include "ttplayer/lyrics/lyric_http.h"
#include "ttplayer/lyrics/service_catalog.h"
#include "service_xml.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <winhttp.h>

namespace ttplayer::lyrics {
namespace {
struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    operator HINTERNET() const { return value; }
};
void Check(BOOL ok) {
    if (ok) return;
    const DWORD error = GetLastError();
    wchar_t buffer[1024]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, buffer, 1024, nullptr);
    throw std::runtime_error("HTTP/TLS " + std::to_string(error) + ": " + core::WideToUtf8(buffer));
}
std::wstring Header(HINTERNET request, const wchar_t* name) {
    DWORD size{};
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, nullptr, &size, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size > 65536) return {};
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, text.data(), &size, nullptr)) return {};
    text.resize(size / sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    return text;
}
std::wstring DecodeHeader(const std::wstring& hex) {
    if (hex.size() % 4) return {};
    auto nibble = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c-L'0' :
        c >= L'a' && c <= L'f' ? c-L'a'+10 : c >= L'A' && c <= L'F' ? c-L'A'+10 : -1; };
    std::wstring result;
    for (size_t i = 0; i < hex.size(); i += 4) {
        const int a=nibble(hex[i]), b=nibble(hex[i+1]), c=nibble(hex[i+2]), d=nibble(hex[i+3]);
        if (std::min({a,b,c,d}) < 0) return {};
        const auto value = static_cast<wchar_t>(((c*16+d)<<8) | (a*16+b));
        if (value == L'\0') break;
        result += value;
    }
    return result;
}
struct Response { std::string body; std::wstring title, url; };
Response Fetch(const std::wstring& address, const settings::NetworkSettings& network,
               const std::function<bool()>& canceled) {
    if (!ValidServiceUrl(address)) throw std::runtime_error("Invalid HTTP/HTTPS URL");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(65);
    const auto guard = [&] {
        if (canceled && canceled()) throw std::runtime_error("Canceled");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("HTTP timeout");
    };
    guard();
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    Check(WinHttpCrackUrl(address.c_str(), static_cast<DWORD>(address.size()), 0, &parts));
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    DWORD access = network.proxy_type == 0 ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    std::wstring proxy;
    if (network.proxy_type > 1 && !network.proxy_server.empty()) {
        access = WINHTTP_ACCESS_TYPE_NAMED_PROXY; proxy = network.proxy_server;
        if (network.proxy_port > 0) proxy += L":" + std::to_wstring(network.proxy_port);
    }
    InternetHandle session{WinHttpOpen(L"TTPlayerRebuild/Lyrics", access,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(),
        proxy.empty() ? WINHTTP_NO_PROXY_BYPASS : L"<local>", 0)};
    if (!session.value && access == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY &&
        GetLastError() == ERROR_INVALID_PARAMETER) {
        session.value = WinHttpOpen(L"TTPlayerRebuild/Lyrics", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    Check(session.value != nullptr);
    Check(WinHttpSetTimeouts(session, 5000, 10000, 10000, 10000));
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (parts.nScheme == INTERNET_SCHEME_HTTPS &&
        !WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        Check(WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)));
    }
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    InternetHandle connection{WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
    Check(connection.value != nullptr);
    InternetHandle request{WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    Check(request.value != nullptr);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) {
        // XP lacks this policy. Disable automatic redirects instead of silently
        // allowing an HTTPS request to be redirected to an insecure connection.
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
        Check(WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)));
    }
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));
    DWORD status{};
    for (int attempt = 0; attempt < 2; ++attempt) {
        guard();
        Check(WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0));
        Check(WinHttpReceiveResponse(request, nullptr));
        DWORD size = sizeof(status);
        Check(WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX));
        if (status != 407 || attempt || proxy.empty() || network.proxy_username.empty()) break;
        DWORD supported{}, first{}, target{};
        Check(WinHttpQueryAuthSchemes(request, &supported, &first, &target));
        DWORD scheme = supported & WINHTTP_AUTH_SCHEME_NEGOTIATE ? WINHTTP_AUTH_SCHEME_NEGOTIATE :
            supported & WINHTTP_AUTH_SCHEME_NTLM ? WINHTTP_AUTH_SCHEME_NTLM :
            supported & WINHTTP_AUTH_SCHEME_DIGEST ? WINHTTP_AUTH_SCHEME_DIGEST : WINHTTP_AUTH_SCHEME_BASIC;
        Check(WinHttpSetCredentials(request, WINHTTP_AUTH_TARGET_PROXY, scheme,
            network.proxy_username.c_str(), network.proxy_password.c_str(), nullptr));
    }
    if (status != 200) throw std::runtime_error("HTTP status " + std::to_string(status));
    Response response;
    response.title = DecodeHeader(Header(request, L"tt-title"));
    response.url = DecodeHeader(Header(request, L"tt-url"));
    char bytes[8192];
    for (;;) {
        guard(); DWORD count{}; Check(WinHttpReadData(request, bytes, sizeof(bytes), &count));
        if (!count) break;
        if (response.body.size() + count > 2 * 1024 * 1024) throw std::runtime_error("Lyric response exceeds 2 MiB");
        response.body.append(bytes, count);
    }
    guard(); return response;
}
std::wstring SearchHex(std::wstring_view text) {
    // 60352A6F -> 603529F6(0x02000100) -> 60352BC0.
    constexpr std::wstring_view pairs = L"()[]{}<>（）［］｛｝《》【】“”";
    auto separator = [](wchar_t c) { WORD type{}; GetStringTypeW(CT_CTYPE1, &c, 1, &type); return !(type & (C1_ALPHA | C1_DIGIT)); };
    size_t start = 0;
    while (start < std::min<size_t>(text.size(), 4) && text[start] >= L'0' && text[start] <= L'9') ++start;
    if (!start || start >= 4 || start >= text.size()-1 || !separator(text[start])) start = 0;
    std::wstring clean;
    for (size_t i = start; i < text.size(); ++i) {
        if (!separator(text[i])) { clean += text[i]; continue; }
        auto at = pairs.find(text[i]);
        if (at != pairs.npos && at % 2 == 0) {
            auto end = text.find(pairs[at+1], i+1); if (end != text.npos) i = end;
        }
    }
    if (!clean.empty()) {
        const int count = LCMapStringW(GetThreadLocale(), LCMAP_SIMPLIFIED_CHINESE | LCMAP_LOWERCASE,
            clean.data(), static_cast<int>(clean.size()), nullptr, 0);
        if (count > 0) {
            std::wstring mapped(count, L'\0');
            LCMapStringW(GetThreadLocale(), LCMAP_SIMPLIFIED_CHINESE | LCMAP_LOWERCASE,
                clean.data(), static_cast<int>(clean.size()), mapped.data(), count);
            clean = std::move(mapped);
        }
    }
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring encoded;
    for (auto c : clean) for (unsigned shift : {0U,8U}) {
        auto byte = (static_cast<unsigned>(c) >> shift) & 255;
        encoded += hex[byte >> 4]; encoded += hex[byte & 15];
    }
    return encoded;
}
}
std::wstring LyricSearchUrl(std::wstring_view base, std::wstring_view artist, std::wstring_view title) {
    return std::wstring(base) + L"?sh?Artist=" + SearchHex(artist) + L"&Title=" + SearchHex(title) + L"&Flags=0&";
}
std::int32_t LyricDownloadCode(std::uint32_t id, std::string_view bytes) {
    // 603531E7: explicit 32-bit wrapping; plain char is SIGNED on x86.
    auto high = id >> 24;
    auto middle = (id >> 16) & 255;
    if (!middle) middle = (~(id >> 8)) & 255;
    if (!high) high = (~id) & 255;
    const std::uint32_t mixed = ((id & 255) << 24) | (middle << 16) | (((id >> 8)&255) << 8) | high;
    std::uint32_t reverse = 0, forward = 0;
    for (size_t i = bytes.size(); i-- > 0;)
        reverse = static_cast<std::int8_t>(bytes[i]) + reverse + (reverse << (i % 2 + 4));
    for (size_t i = 0; i < bytes.size(); ++i)
        forward = static_cast<std::int8_t>(bytes[i]) + forward + (forward << (i % 2 + 3));
    return std::bit_cast<std::int32_t>(((reverse ^ mixed) + (forward | id)) * (forward | mixed) * (reverse ^ id));
}
std::wstring LyricDownloadUrl(std::wstring_view base, const HttpLyricResult& result) {
    const auto bytes = core::WideToUtf8(result.artist + result.title);
    return std::wstring(base) + L"?dl?Id=" + std::to_wstring(result.id) + L"&Code=" +
        std::to_wstring(LyricDownloadCode(static_cast<std::uint32_t>(result.id), bytes)) + L"&";
}
std::vector<HttpLyricResult> SearchHttpLyrics(const std::wstring& base, std::wstring_view artist,
    std::wstring_view title, const settings::NetworkSettings& network, const std::function<bool()>& canceled) {
    xml::Apartment apartment;
    auto response = Fetch(LyricSearchUrl(base, artist, title), network, canceled);
    auto doc = xml::Parse(response.body);
    xml::ComPtr<IXMLDOMNode> result_node; doc->selectSingleNode(_bstr_t(L"/result"), &result_node);
    if (!result_node) throw std::runtime_error("Not a lyric search result");
    xml::ComPtr<IXMLDOMElement> root; doc->get_documentElement(&root);
    xml::ComPtr<IXMLDOMNodeList> nodes; root->selectNodes(_bstr_t(L"lrc"), &nodes);
    long count{}; nodes->get_length(&count);
    if (count > 10000) throw std::runtime_error("Too many lyric results");
    std::vector<HttpLyricResult> results;
    for (long i = 0; i < count; ++i) {
        xml::ComPtr<IXMLDOMNode> node; nodes->get_item(i, &node);
        const auto id = core::WideToUtf8(xml::Attribute(node.Get(), L"id"));
        std::int32_t value{}; const auto parse = std::from_chars(id.data(), id.data()+id.size(), value);
        if (parse.ec != std::errc{} || parse.ptr != id.data()+id.size()) continue;
        results.push_back({value, xml::Attribute(node.Get(), L"artist"), xml::Attribute(node.Get(), L"title")});
    }
    if (results.empty()) {
        const auto error = xml::Attribute(root.Get(), L"errmsg");
        if (!error.empty()) throw std::runtime_error(core::WideToUtf8(error));
    }
    return results;
}
HttpLyricDownload DownloadHttpLyric(const std::wstring& base, const HttpLyricResult& result,
    const settings::NetworkSettings& network, const std::function<bool()>& canceled) {
    auto response = Fetch(LyricDownloadUrl(base, result), network, canceled);
    auto body = std::string_view(response.body);
    if (body.starts_with("\xef\xbb\xbf")) body.remove_prefix(3);
    const auto first = body.find_first_not_of(" \t\r\n");
    if (first != body.npos && body[first] == '<') {
        xml::Apartment apartment; auto doc = xml::Parse(response.body);
        xml::ComPtr<IXMLDOMElement> root; doc->get_documentElement(&root);
        auto error = core::WideToUtf8(xml::Attribute(root.Get(), L"errmsg"));
        throw std::runtime_error(error.empty() ? "Server returned XML/HTML instead of lyrics" : error);
    }
    if (response.body.empty()) throw std::runtime_error("Empty lyric download");
    auto text = core::Utf8ToWide(response.body.starts_with("\xef\xbb\xbf") ? response.body.substr(3) : response.body);
    return {std::move(text), std::move(response.title), std::move(response.url)};
}
std::string FetchLyricHttp(const std::wstring& url, const settings::NetworkSettings& network, const std::function<bool()>& canceled) {
    return Fetch(url, network, canceled).body;
}
}
