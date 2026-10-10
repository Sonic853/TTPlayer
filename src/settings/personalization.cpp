#include "ttplayer/settings/personalization.h"
#include "ttplayer/settings/settings.h"
#include <algorithm>
#include <cwctype>
#include <windows.h>
#include <wincrypt.h>

namespace ttplayer::settings {
namespace {
std::wstring Digest(std::string bytes) {
    bytes += "tt#$(@#*$!2";
    HCRYPTPROV provider{};
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return {};
    HCRYPTHASH hash{};
    BYTE digest[16]{};
    DWORD size = sizeof(digest);
    const bool ok = CryptCreateHash(provider, CALG_MD5, 0, 0, &hash) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0) &&
        CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!ok) return {};
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    for (const BYTE byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
int Hex(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}
}

std::wstring NormalizePersonalName(std::wstring_view name) {
    while (!name.empty() && iswspace(name.front())) name.remove_prefix(1);
    while (!name.empty() && iswspace(name.back())) name.remove_suffix(1);
    std::wstring result;
    for (const auto c : name) if (c >= L' ' && c != 0x7f) result += c;
    if (result.size() > kPersonalNameLimit) {
        result.resize(kPersonalNameLimit);
        if (result.back() >= 0xd800 && result.back() <= 0xdbff) result.pop_back();
    }
    return result;
}

std::wstring DecodeLegacyPersonalName(std::wstring_view word, std::wstring_view md5) {
    if (word.empty() || word.size() > 4096 || md5.size() != 32) return {};
    std::string bytes;
    for (size_t i = 0; i < word.size(); ++i) {
        auto c = word[i];
        if (c == L'%') {
            if (i + 2 >= word.size() || Hex(word[i + 1]) < 0 || Hex(word[i + 2]) < 0) return {};
            c = static_cast<wchar_t>((Hex(word[i + 1]) << 4) | Hex(word[i + 2]));
            i += 2;
        } else if (c == L'+') c = L' ';
        if (!c || c > 255) return {};
        bytes += static_cast<char>(c);
    }
    if (Digest(bytes) != md5) return {};
    std::string unescaped;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\\' && i + 1 < bytes.size()) ++i;
        unescaped += bytes[i];
    }
    const int size = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, unescaped.data(),
                                       static_cast<int>(unescaped.size()), nullptr, 0);
    if (!size) return {};
    std::wstring name(size, L'\0');
    MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, unescaped.data(),
                        static_cast<int>(unescaped.size()), name.data(), size);
    return NormalizePersonalName(name);
}

bool SetPersonalName(PlayerSettings& player, std::wstring_view name) {
    const auto normalized = NormalizePersonalName(name);
    if (normalized.empty()) {
        player.personal_name.clear(); player.user_word.clear(); player.user_word_md5.clear();
        return true;
    }
    BOOL substituted{};
    const int size = WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS, normalized.data(),
        static_cast<int>(normalized.size()), nullptr, 0, nullptr, &substituted);
    if (!size || substituted) return false;
    std::string bytes(size, '\0');
    if (!WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS, normalized.data(),
        static_cast<int>(normalized.size()), bytes.data(), size, nullptr, &substituted) || substituted) return false;
    std::string escaped;
    for (const char c : bytes) { escaped += c; if (c == '\\') escaped += c; }
    const auto md5 = Digest(escaped);
    if (md5.empty()) return false;
    std::wstring word;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (const unsigned char c : escaped) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') word += c;
        else { word += L'%'; word += hex[c >> 4]; word += hex[c & 15]; }
    }
    player.personal_name = normalized;
    player.user_word = word;
    player.user_word_md5 = md5;
    return true;
}
}
