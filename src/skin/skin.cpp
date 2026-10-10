#include <ttpcomm/base64.h>
#include "ttplayer/skin/skin.h"
#include "ttplayer/settings/settings.h"

#include <algorithm>
#include <array>
#include <comdef.h>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <msxml6.h>
#include <regex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ttplayer::skin {
namespace {
struct ComScope {
    HRESULT result{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::wstring Attribute(IXMLDOMNode* node, const wchar_t* name) {
    IXMLDOMNamedNodeMap* attributes{};
    IXMLDOMNode* attribute{};
    if (!node || FAILED(node->get_attributes(&attributes)) || !attributes) return {};
    attributes->getNamedItem(_bstr_t(name), &attribute);
    attributes->Release();
    if (!attribute) return {};
    VARIANT value{};
    VariantInit(&value);
    attribute->get_nodeValue(&value);
    attribute->Release();
    const _variant_t managed(value, false);
    if (managed.vt == VT_EMPTY || managed.vt == VT_NULL) return {};
    return static_cast<const wchar_t*>(_bstr_t(managed));
}

std::vector<IXMLDOMNode*> Nodes(IXMLDOMDocument* document, const wchar_t* path) {
    std::vector<IXMLDOMNode*> result;
    IXMLDOMNodeList* list{};
    document->selectNodes(_bstr_t(path), &list);
    long count{};
    if (list) list->get_length(&count);
    for (long i = 0; i < count; ++i) {
        IXMLDOMNode* node{};
        list->get_item(i, &node);
        if (node) result.push_back(node); // caller releases each item
    }
    if (list) list->Release();
    return result;
}

std::filesystem::path SkinAssetPath(const std::filesystem::path& directory,
                                    const std::wstring& name) {
    const std::filesystem::path relative(name);
    if (name.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory() || name.find(L':') != std::wstring::npos) return {};
    for (const auto& part : relative) if (part == L"..") return {};
    return directory / relative;
}

RECT ParseRect(const std::wstring& value, RECT result = {}) {
    // 004A9D02 clears empty input, but sscanf writes any successfully parsed
    // prefix into the existing slot. Preserve that deterministic behavior.
    if (value.empty()) return {};
    swscanf_s(value.c_str(), L"%ld,%ld,%ld,%ld",
        &result.left, &result.top, &result.right, &result.bottom);
    return result;
}

COLORREF ParseColor(const std::wstring& value, COLORREF fallback) {
    unsigned int red{}, green{}, blue{};
    if (swscanf_s(value.c_str(), L"#%2x%2x%2x",
                  &red, &green, &blue) != 3) return fallback;
    return RGB(red, green, blue);
}

int ParseInt(const std::wstring& value, int fallback) {
    if (value.empty()) return fallback;
    wchar_t* end{};
    const long parsed = wcstol(value.c_str(), &end, 10);
    // 004A9CE9 uses atoi: accept a numeric prefix (including whitespace).
    return end != value.c_str() ? static_cast<int>(parsed) : 0;
}

unsigned int ParseAlignment(const std::wstring& value) {
    unsigned int result{};
    size_t first{};
    while (first <= value.size()) {
        const size_t separator = value.find(L'+', first);
        const std::wstring_view token(value.data() + first,
            (separator == std::wstring::npos ? value.size() : separator) - first);
        if (token == L"left") result = (result & 0xf0U) | 1U;
        else if (token == L"center") result = (result & 0xf0U) | 2U;
        else if (token == L"right") result = (result & 0xf0U) | 3U;
        else if (token == L"top") result = (result & 0x0fU) | 0x10U;
        else if (token == L"vcenter") result = (result & 0x0fU) | 0x20U;
        else if (token == L"bottom") result = (result & 0x0fU) | 0x30U;
        if (separator == std::wstring::npos) break;
        first = separator + 1;
    }
    return result;
}

std::wstring DecodeLegacyXml(std::span<const unsigned char> source) {
    std::string bytes(reinterpret_cast<const char*>(source.data()), source.size());
    if (bytes.starts_with("\xEF\xBB\xBF")) bytes.erase(0, 3);
    UINT code_page = CP_UTF8;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                    static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) {
        code_page = CP_ACP;
        count = MultiByteToWideChar(code_page, 0, bytes.data(),
                                    static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (count <= 0) throw std::runtime_error("unsupported Skin.xml encoding");
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(code_page, code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                        bytes.data(), static_cast<int>(bytes.size()), result.data(), count);
    // TTPlayer's legacy XML reader accepts two defects present in the shipped
    // skin collection: unquoted scalar attributes and adjacent quoted
    // attributes without intervening whitespace.  Normalize both before
    // handing the document to MSXML.  Excluding '/' keeps `value=true/>` as a
    // self-closing tag rather than consuming its terminator into the value.
    result = std::regex_replace(result,
        std::wregex(L"([\"'])([A-Za-z_][A-Za-z0-9_-]*\\s*=)"),
        L"$1 $2");
    // Quote unquoted attribute values only while walking a tag and only when
    // the '=' is outside an existing quoted value.  A regular expression over
    // the whole document also sees query-string pairs inside url="..." and
    // corrupts strings such as `?automodule=blog&blogid=62`.
    std::wstring normalized;
    normalized.reserve(result.size());
    bool in_tag{};
    wchar_t quote{};
    for (size_t index{}; index < result.size();) {
        const wchar_t value = result[index];
        if (!in_tag) {
            normalized.push_back(value);
            if (value == L'<') in_tag = true;
            ++index;
            continue;
        }
        if (quote) {
            normalized.push_back(value);
            if (value == quote) quote = 0;
            ++index;
            continue;
        }
        if (value == L'\"' || value == L'\'') {
            quote = value;
            normalized.push_back(value);
            ++index;
            continue;
        }
        if (value == L'>') {
            in_tag = false;
            normalized.push_back(value);
            ++index;
            continue;
        }
        normalized.push_back(value);
        ++index;
        if (value != L'=') continue;
        while (index < result.size() && iswspace(result[index]))
            normalized.push_back(result[index++]);
        if (index >= result.size() || result[index] == L'\"' ||
            result[index] == L'\'' || result[index] == L'>' ||
            result[index] == L'/') continue;
        size_t end = index;
        while (end < result.size() && !iswspace(result[end]) &&
               result[end] != L'>' && result[end] != L'\"' &&
               result[end] != L'\'') ++end;
        size_t value_end = end;
        if (value_end > index && result[value_end - 1] == L'/') --value_end;
        normalized.push_back(L'\"');
        normalized.append(result, index, value_end - index);
        normalized.push_back(L'\"');
        normalized.append(result, value_end, end - value_end);
        // Some published skins terminate an otherwise unquoted value with a
        // stray quote (`position=-207,12,0,187," resize_rect=...`).  The
        // native reader treats it as the end delimiter; discard it here.
        index = end < result.size() && (result[end] == L'\"' ||
            result[end] == L'\'') ? end + 1 : end;
    }
    result = std::move(normalized);
    // The same permissive parser also keeps the first occurrence of an
    // immediately repeated attribute.  ouptix.skn contains
    // `align="" align=""`; XML DOM rejects the entire document for it.
    const std::wregex adjacent_duplicate(
        L"(\\s+([A-Za-z_][A-Za-z0-9_-]*)\\s*=\\s*(\\\"[^\\\"]*\\\"|'[^']*'))"
        L"\\s+\\2\\s*=\\s*(\\\"[^\\\"]*\\\"|'[^']*')");
    for (;;) {
        const std::wstring deduplicated = std::regex_replace(
            result, adjacent_duplicate, L"$1");
        if (deduplicated == result) break;
        result = deduplicated;
    }
    // Old skin metadata commonly embeds a query string with a literal '&'
    // in url=. The legacy reader accepts it; XML 1.0 requires escaping it.
    for (size_t offset{}; (offset = result.find(L'&', offset)) !=
                          std::wstring::npos;) {
        const bool entity = result.compare(offset, 5, L"&amp;") == 0 ||
            result.compare(offset, 4, L"&lt;") == 0 ||
            result.compare(offset, 4, L"&gt;") == 0 ||
            result.compare(offset, 6, L"&quot;") == 0 ||
            result.compare(offset, 6, L"&apos;") == 0 ||
            (offset + 2 < result.size() && result[offset + 1] == L'#' &&
             result.find(L';', offset + 2) != std::wstring::npos);
        if (entity) {
            ++offset;
        } else {
            result.replace(offset, 1, L"&amp;");
            offset += 5;
        }
    }
    return result;
}

std::wstring DecodeLegacyXml(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open Skin.xml");
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(input)), {});
    return DecodeLegacyXml(bytes);
}

bool IsFourStateButton(std::wstring_view name) {
    // Exact node set dispatched to FUN_004A95B0 by
    // CSkinParser_ParsePlayerWindow (004A8536).  Ghidra rendered the literal
    // at VA 0051C438 as an empty string, but the PE bytes at file offset
    // 0011F3C8 are "browser\0".  It is therefore one of the native buttons;
    // 6.1.2 (00520101) additionally accepts set and the five mode_* buttons.
    constexpr std::wstring_view names[] = {
        L"play", L"pause", L"stop", L"prev", L"next", L"mute", L"open",
        L"lyric", L"equalizer", L"playlist", L"browser", L"exit",
        L"minimize", L"minimode", L"set", L"login", L"mode_single", L"mode_loop",
        L"mode_slider", L"mode_circle", L"mode_random"
    };
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}
} // namespace

std::optional<SkinMetadata> ParseLegacySkinMetadata(
    std::span<const unsigned char> xml_bytes) {
    try {
        // DecodeLegacyXml deliberately accepts and strips an UTF-8 BOM.  The
        // native token reader did not, but accepting it is required for
        // otherwise valid packages such as 004941_088.skn.
        ComScope com;
        if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
            return std::nullopt;

        IXMLDOMDocument* document{};
        if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&document)))) {
            return std::nullopt;
        }
        document->put_async(VARIANT_FALSE);
        auto xml = DecodeLegacyXml(xml_bytes);
        // FUN_004A7929 consumes one root element and ignores bytes after its
        // closing tag.  Preserve that behavior for published skins carrying
        // an obsolete coordinate line after </skin>.
        if (const size_t end = xml.find(L"</skin>"); end != std::wstring::npos)
            xml.erase(end + std::wstring_view(L"</skin>").size());

        VARIANT_BOOL loaded{};
        document->loadXML(_bstr_t(xml.c_str()), &loaded);
        if (loaded != VARIANT_TRUE) {
            document->Release();
            return std::nullopt;
        }

        IXMLDOMNode* root{};
        document->selectSingleNode(_bstr_t(L"/skin"), &root);
        if (!root) {
            document->Release();
            return std::nullopt;
        }

        SkinMetadata result;
        result.version = ParseInt(Attribute(root, L"version"), 0);
        if (result.version == 2) {
            result.name = Attribute(root, L"name");
            result.author = Attribute(root, L"author");
            result.url = Attribute(root, L"url");
            result.email = Attribute(root, L"email");
            if (result.email.size() >= 7 &&
                _wcsnicmp(result.email.c_str(), L"mailto:", 7) == 0) {
                result.email.erase(0, 7);
            }
            result.transparent_color = ParseColor(
                Attribute(root, L"transparent_color"), CLR_INVALID);
        }
        root->Release();
        document->Release();
        if (result.version != 2) return std::nullopt;
        return result;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

LegacySkin::~LegacySkin() {
    if (icon_) DestroyIcon(icon_);
}

LegacySkin::LegacySkin(LegacySkin&& other) noexcept {
    *this = std::move(other);
}

LegacySkin& LegacySkin::operator=(LegacySkin&& other) noexcept {
    if (this == &other) return *this;
    if (icon_) DestroyIcon(icon_);
    bitmaps_ = std::move(other.bitmaps_);
    // Layouts/cache share RAII images. Clear moved-from references as well.
    other.bitmaps_.clear();
    background_ = std::exchange(other.background_, nullptr);
    icon_ = std::exchange(other.icon_, nullptr);
    window_size_ = other.window_size_;
    transparent_color_ = other.transparent_color_;
    elements_ = std::move(other.elements_);
    other.elements_.clear();
    mini_ = std::move(other.mini_);
    other.mini_ = {};
    lyric_ = std::move(other.lyric_);
    other.lyric_ = {};
    desktop_lyric_bar_ = std::move(other.desktop_lyric_bar_);
    other.desktop_lyric_bar_ = {};
    playlist_ = std::move(other.playlist_);
    other.playlist_ = {};
    equalizer_ = std::move(other.equalizer_);
    other.equalizer_ = {};
    visual_ = std::move(other.visual_);
    other.visual_ = {};
    return *this;
}

SkinImage LegacySkin::LoadBitmap(const std::filesystem::path& path) {
    auto key = path.lexically_normal().wstring();
    std::transform(key.begin(), key.end(), key.begin(), towlower);
    if (const auto found = bitmaps_.find(key); found != bitmaps_.end()) return found->second;
    const auto bitmap = SkinImage::Load(path);
    if (!bitmap) return nullptr;
    bitmaps_.emplace(key, bitmap);
    return bitmap;
}

namespace {
SkinAnimation LoadAnimation(IXMLDOMNode* node, int default_mode) {
    SkinAnimation result;
    result.mode = ParseInt(Attribute(node, L"flash_mode"), default_mode);
    result.frame_count = ParseInt(Attribute(node, L"frame_count"), 10);
    result.frame_interval = ParseInt(Attribute(node, L"frame_interval"), 100);
    // 00419637 disables invalid animation parameters, not the image/control.
    if (result.mode < 0 || result.mode > 3 || result.frame_count <= 0 ||
        result.frame_interval <= 0) result.mode = 0;
    return result;
}

void LoadElementAnimation(LegacySkin& skin, const std::filesystem::path& directory,
                          IXMLDOMNode* node, SkinElement& element) {
    element.animation = LoadAnimation(node, element.four_state ? 1 : 0);
    if (element.four_state && element.animation.mode > 2) element.animation.mode = 0;
    const auto flash = Attribute(node, L"flash_image");
    if (!flash.empty()) {
        element.flash_image = skin.LoadBitmap(directory / flash);
        element.flash_size = element.flash_image.Size();
    }
}

SkinBitmap LoadSkinBitmap(LegacySkin& skin, const std::filesystem::path& directory,
                          IXMLDOMNode* node, const wchar_t* attribute, const SkinBitmap* previous = nullptr) {
    SkinBitmap result = previous ? *previous : SkinBitmap{};
    const auto name = Attribute(node, attribute);
    if (name.empty()) return result;
    // Keep bitmap ownership centralized in LegacySkin::bitmaps_.
    auto candidate = skin.LoadBitmap(SkinAssetPath(directory, name));
    if (candidate) {
        result.image = std::move(candidate);
        BITMAP info{};
        GetObjectW(result.image, sizeof(info), &info);
        result.size = {info.bmWidth, info.bmHeight};
    }
    return result;
}

SkinElement LoadEqualizerElement(LegacySkin& skin,
                                 const std::filesystem::path& directory,
                                 IXMLDOMNode* node, std::wstring name,
                                 bool button, bool force_vertical = false,
                                 const SkinElement* previous = nullptr) {
    SkinElement element = previous ? *previous : SkinElement{};
    element.name = std::move(name);
    element.bounds = ParseRect(Attribute(node, L"position"), element.bounds);
    element.alignment = ParseAlignment(Attribute(node, L"align"));
    const auto vertical = Attribute(node, L"vertical");
    element.vertical = force_vertical || _wcsicmp(vertical.c_str(), L"true") == 0;
    element.four_state = button;
    const auto load = [&](const wchar_t* attribute, SkinImage& bitmap, SIZE& size) {
        const auto file = Attribute(node, attribute);
        if (file.empty()) return;
        auto candidate = skin.LoadBitmap(SkinAssetPath(directory, file));
        if (!candidate) return;
        bitmap = std::move(candidate);
        BITMAP info{};
        GetObjectW(bitmap, sizeof(info), &info);
        size = {info.bmWidth, info.bmHeight};
    };
    load(L"image", element.image, element.image_size);
    load(L"bar_image", element.bar_image, element.bar_size);
    load(L"fill_image", element.fill_image, element.fill_size);
    load(L"fill_image2", element.fill_image2, element.fill_size2);
    load(L"thumb_image", element.thumb_image, element.thumb_size);
    LoadElementAnimation(skin, directory, node, element);
    if (button && element.image) {
        element.frames = 4;
        // FUN_0042955B applies the XML position after FUN_00452FFF installs
        // the four-frame bitmap. The child therefore retains the complete
        // XML rectangle even when a frame is a few pixels smaller (for
        // example Let's Vista profile: 96x21 client, 94x19 frame).
    }
    return element;
}

SkinElement LoadPlaylistImageElement(LegacySkin& skin,
                                     const std::filesystem::path& directory,
                                     IXMLDOMNode* node, std::wstring name,
                                     const SkinElement* previous = nullptr) {
    SkinElement element = previous ? *previous : SkinElement{};
    element.name = std::move(name);
    element.bounds = ParseRect(Attribute(node, L"position"), element.bounds);
    element.alignment = ParseAlignment(Attribute(node, L"align"));
    const auto image = Attribute(node, L"image");
    if (!image.empty()) {
        if (auto candidate = skin.LoadBitmap(SkinAssetPath(directory, image))) {
            element.image = std::move(candidate);
            element.image_size = element.image.Size();
        }
    }
    if (element.name == L"close" && element.image && element.image_size.cx >= 4) {
        element.four_state = true;
        element.frames = 4;
        element.bounds.left = element.bounds.right - element.image_size.cx / 4;
        element.bounds.bottom = element.bounds.top + element.image_size.cy;
    }
    LoadElementAnimation(skin, directory, node, element);
    return element;
}

bool ParseBool(const std::wstring& value, bool fallback = false) {
    if (value.empty()) return fallback;
    return _wcsicmp(value.c_str(), L"true") == 0 || value == L"1";
}

void InitializeDefaultLogFont(LOGFONTW& font) {
    font = {};
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 0, &metrics, 0))
        font = metrics.lfMessageFont;
    else {
        font.lfHeight = -12;
        font.lfWeight = FW_NORMAL;
        wcscpy_s(font.lfFaceName, L"SimSun");
    }
}

std::vector<unsigned char> DecodeBase64(std::wstring_view source) {
    return ttpcomm::text::DecodeLegacyBase64(source);
}

bool ParseLogFont(const std::wstring& descriptor, LOGFONTW& font) {
    if (descriptor.empty()) return false;
    LOGFONTW parsed = font;
    if (_wcsnicmp(descriptor.c_str(), L"base64:", 7) == 0) {
        const auto bytes = DecodeBase64(
            std::wstring_view(descriptor).substr(7));
        if (bytes.size() == sizeof(LOGFONTW)) {
            std::memcpy(&parsed, bytes.data(), sizeof(parsed));
        } else if (bytes.size() == sizeof(LOGFONTA)) {
            LOGFONTA ansi{};
            std::memcpy(&ansi, bytes.data(), sizeof(ansi));
            parsed = {};
            parsed.lfHeight = ansi.lfHeight;
            parsed.lfWidth = ansi.lfWidth;
            parsed.lfEscapement = ansi.lfEscapement;
            parsed.lfOrientation = ansi.lfOrientation;
            parsed.lfWeight = ansi.lfWeight;
            parsed.lfItalic = ansi.lfItalic;
            parsed.lfUnderline = ansi.lfUnderline;
            parsed.lfStrikeOut = ansi.lfStrikeOut;
            parsed.lfCharSet = ansi.lfCharSet;
            parsed.lfOutPrecision = ansi.lfOutPrecision;
            parsed.lfClipPrecision = ansi.lfClipPrecision;
            parsed.lfQuality = ansi.lfQuality;
            parsed.lfPitchAndFamily = ansi.lfPitchAndFamily;
            if (MultiByteToWideChar(CP_ACP, 0, ansi.lfFaceName, -1,
                    parsed.lfFaceName, LF_FACESIZE) == 0) return false;
        } else {
            return false;
        }
        font = parsed;
        return true;
    }

    std::array<int, 13> value{};
    if (swscanf_s(descriptor.c_str(),
            L"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,",
            &value[0], &value[1], &value[2], &value[3], &value[4],
            &value[5], &value[6], &value[7], &value[8], &value[9],
            &value[10], &value[11], &value[12]) != 13) return false;
    size_t comma = 0;
    for (size_t index = 0; index < value.size(); ++index) {
        comma = descriptor.find(L',', comma);
        if (comma == std::wstring::npos) return false;
        ++comma;
    }
    parsed.lfHeight = value[0];
    parsed.lfWidth = value[1];
    parsed.lfEscapement = value[2];
    parsed.lfOrientation = value[3];
    parsed.lfWeight = value[4];
    parsed.lfItalic = static_cast<BYTE>(value[5]);
    parsed.lfUnderline = static_cast<BYTE>(value[6]);
    parsed.lfStrikeOut = static_cast<BYTE>(value[7]);
    parsed.lfCharSet = static_cast<BYTE>(value[8]);
    parsed.lfOutPrecision = static_cast<BYTE>(value[9]);
    parsed.lfClipPrecision = static_cast<BYTE>(value[10]);
    parsed.lfQuality = static_cast<BYTE>(value[11]);
    parsed.lfPitchAndFamily = static_cast<BYTE>(value[12]);
    // FUN_0048DE50 always copies the final field, including an empty face
    // name.  This intentionally clears the seeded face while preserving all
    // thirteen numeric LOGFONT members.
    wcsncpy_s(parsed.lfFaceName, descriptor.c_str() + comma, _TRUNCATE);
    font = parsed;
    return true;
}

void LoadLyricColors(const std::filesystem::path& directory, LyricSkin& lyric) {
    const auto path = directory / L"Lyric.xml";
    if (!std::filesystem::exists(path)) return;
    ComScope com;
    IXMLDOMDocument* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&document)))) return;
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    try {
        const auto xml = DecodeLegacyXml(path);
        document->loadXML(_bstr_t(xml.c_str()), &loaded);
    } catch (const std::exception&) {
        document->Release();
        return;
    }
    if (loaded == VARIANT_TRUE) {
        IXMLDOMNode* node{};
        document->selectSingleNode(_bstr_t(L"/ttplayer_lyric/Lyric"), &node);
        if (node) {
            static_cast<void>(ParseLogFont(Attribute(node, L"Font"), lyric.font));
            lyric.text_color = ParseColor(Attribute(node, L"TextColor"),
                                          lyric.text_color);
            lyric.highlight_color = ParseColor(Attribute(node, L"HilightColor"),
                                               lyric.highlight_color);
            lyric.background_color = ParseColor(Attribute(node, L"BkgndColor"),
                                                lyric.background_color);
            node->Release();
        }
    }
    document->Release();
}

void LoadPlaylistColors(const std::filesystem::path& directory, PlaylistSkin& playlist) {
    const auto path = directory / L"Playlist.xml";
    if (!std::filesystem::exists(path)) return;
    ComScope com;
    IXMLDOMDocument* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&document)))) return;
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    try {
        const auto xml = DecodeLegacyXml(path);
        document->loadXML(_bstr_t(xml.c_str()), &loaded);
    } catch (const std::exception&) {
        document->Release();
        return;
    }
    if (loaded == VARIANT_TRUE) {
        IXMLDOMNode* node{};
        document->selectSingleNode(_bstr_t(L"/ttplayer_playlist/PlayList"), &node);
        if (node) {
            static_cast<void>(ParseLogFont(Attribute(node, L"Font"), playlist.font_descriptor));
            playlist.font = playlist.font_descriptor.lfFaceName;
            playlist.font_height = playlist.font_descriptor.lfHeight;
            playlist.text_color = ParseColor(Attribute(node, L"Color_Text"), playlist.text_color);
            playlist.highlight_color = ParseColor(Attribute(node, L"Color_Hilight"), playlist.highlight_color);
            playlist.background_color = ParseColor(Attribute(node, L"Color_Bkgnd"), playlist.background_color);
            playlist.number_color = ParseColor(Attribute(node, L"Color_Number"), playlist.number_color);
            playlist.duration_color = ParseColor(Attribute(node, L"Color_Duration"), playlist.duration_color);
            // 0048E696 has a special fallback for an existing PlayList node:
            // absent/invalid Color_Select uses the system selection color.
            // An absent node/file, like other missing fields, retains the seed.
            playlist.selected_color = ParseColor(Attribute(node, L"Color_Select"),
                                                  GetSysColor(COLOR_HIGHLIGHT));
            const auto selected_text = Attribute(node, L"Color_SelText");
            if (!selected_text.empty()) {
                const auto color = ParseColor(selected_text, CLR_INVALID);
                if (color != CLR_INVALID) playlist.selected_text_color = color;
            }
            playlist.alternate_background_color = ParseColor(
                Attribute(node, L"Color_Bkgnd2"), playlist.alternate_background_color);
            node->Release();
        }
    }
    document->Release();
}

void LoadVisualSettings(const std::filesystem::path& directory,
                        VisualSkin& visual) {
    const auto path = directory / L"Visual.xml";
    if (!std::filesystem::exists(path)) return;
    ComScope com;
    IXMLDOMDocument* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&document))) || !document)
        return;
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    try {
        const auto xml = DecodeLegacyXml(path);
        document->loadXML(_bstr_t(xml.c_str()), &loaded);
    } catch (const std::exception&) {
        document->Release();
        return;
    }
    if (loaded == VARIANT_TRUE) {
        IXMLDOMNode* node{};
        document->selectSingleNode(_bstr_t(L"/ttplayer_visual/Visual"), &node);
        if (node) {
            const auto color = [&](const wchar_t* name,
                                   std::optional<COLORREF>& destination) {
                const auto source = Attribute(node, name);
                if (source.empty()) return;
                const COLORREF parsed = ParseColor(source, CLR_INVALID);
                if (parsed != CLR_INVALID) destination = parsed;
            };
            const auto integer = [&](const wchar_t* name,
                                     std::optional<int>& destination) {
                const auto source = Attribute(node, name);
                if (!source.empty()) destination = ParseInt(source, 0);
            };
            color(L"SpectrumTopColor", visual.spectrum_top_color);
            color(L"SpectrumBtmColor", visual.spectrum_bottom_color);
            color(L"SpectrumMidColor", visual.spectrum_middle_color);
            color(L"SpectrumPeakColor", visual.spectrum_peak_color);
            integer(L"SpectrumWide", visual.spectrum_wide);
            integer(L"BlurSpeed", visual.blur_speed);
            integer(L"Type", visual.type);
            const auto blur = Attribute(node, L"Blur");
            if (!blur.empty()) visual.blur = ParseBool(blur);
            color(L"BlurScopeColor", visual.blur_scope_color);
            color(L"TextColor", visual.text_color);
            const auto descriptor = Attribute(node, L"Font");
            if (!descriptor.empty()) {
                LOGFONTW font{};
                if (ParseLogFont(descriptor, font)) visual.font = font;
            }
            visual.valid = true;
            node->Release();
        }
    }
    document->Release();
}
} // namespace

LegacySkin LegacySkin::Load(const std::filesystem::path& directory,
                            const settings::Settings* current) {
    ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) throw std::runtime_error("COM init failed");
    IXMLDOMDocument* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&document)))) {
        throw std::runtime_error("MSXML6 unavailable");
    }
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    auto xml = DecodeLegacyXml(directory / L"Skin.xml");
    // The original streaming XML reader stops at </skin>. Several published
    // packages append an old coordinate line after the root element (for
    // example ironger-BlueArmorPlayer); MSXML rejects that otherwise valid
    // package as top-level trailing content.
    if (const size_t end = xml.find(L"</skin>"); end != std::wstring::npos)
        xml.erase(end + std::wstring_view(L"</skin>").size());
    document->loadXML(_bstr_t(xml.c_str()), &loaded);
    if (loaded != VARIANT_TRUE) {
        IXMLDOMParseError* parse_error{};
        std::string detail;
        if (SUCCEEDED(document->get_parseError(&parse_error)) && parse_error) {
            BSTR reason{};
            parse_error->get_reason(&reason);
            if (reason) {
                const int length = WideCharToMultiByte(CP_UTF8, 0, reason, SysStringLen(reason),
                    nullptr, 0, nullptr, nullptr);
                detail.resize(static_cast<size_t>(length));
                WideCharToMultiByte(CP_UTF8, 0, reason, SysStringLen(reason), detail.data(), length,
                    nullptr, nullptr);
                SysFreeString(reason);
            }
            long line{};
            long position{};
            parse_error->get_line(&line);
            parse_error->get_linepos(&position);
            if (line > 0) detail += " (line " + std::to_string(line) +
                ", column " + std::to_string(position) + ")";
            if (line == 1 && position > 0) {
                const size_t begin = static_cast<size_t>(std::max(0L, position - 16));
                const size_t length = std::min<size_t>(48, xml.size() -
                    std::min(begin, xml.size()));
                std::string context;
                context.reserve(length);
                for (const wchar_t value : xml.substr(begin, length))
                    context.push_back(value < 0x80 ? static_cast<char>(value) : '?');
                detail += " near `" + context + "`";
            }
            parse_error->Release();
        }
        document->Release();
        throw std::runtime_error("invalid Skin.xml: " + detail);
    }

    LegacySkin skin;
    InitializeDefaultLogFont(skin.lyric_.font);
    InitializeDefaultLogFont(skin.playlist_.font_descriptor);
    // 0048E635 / 0048EA53 seed package descriptors from the current settings
    // before parsing sparse XML. Standalone loads start with DLL defaults.
    if (current) {
        auto& list = skin.playlist_;
        if (current->playlist.font_descriptor_valid)
            list.font_descriptor = current->playlist.font_descriptor;
        else {
            list.font_descriptor.lfHeight = current->playlist.font_height;
            wcsncpy_s(list.font_descriptor.lfFaceName, current->playlist.font.c_str(), _TRUNCATE);
        }
        list.text_color = current->playlist.text_color;
        list.highlight_color = current->playlist.highlight_color;
        list.number_color = current->playlist.number_color;
        list.duration_color = current->playlist.duration_color;
        list.selected_color = current->playlist.selected_color;
        list.background_color = current->playlist.background_color;
        list.alternate_background_color = current->playlist.alternate_background_color;
        auto& lyric = skin.lyric_;
        if (current->lyric.font_valid) lyric.font = current->lyric.font;
        if (current->lyric.text_color != CLR_INVALID)
            lyric.text_color = current->lyric.text_color;
        if (current->lyric.highlight_color != CLR_INVALID)
            lyric.highlight_color = current->lyric.highlight_color;
        if (current->lyric.background_color != CLR_INVALID)
            lyric.background_color = current->lyric.background_color;
    }
    IXMLDOMNode* root{};
    document->selectSingleNode(_bstr_t(L"/skin"), &root);
    if (!root || ParseInt(Attribute(root, L"version"), 0) != 2) {
        if (root) root->Release();
        document->Release();
        throw std::runtime_error("unsupported Skin.xml version (expected 2)");
    }
    skin.transparent_color_ = ParseColor(Attribute(root, L"transparent_color"), CLR_INVALID);
    if (root) root->Release();

    // 004A8536 writes fixed slots. Repeated definitions update the same slot;
    // missing/failed bitmap loads preserve its previous image (004A9D8C).
    const auto load_player = [&](const wchar_t* path, SkinImage& background,
                                 SIZE& size, std::vector<SkinElement>& elements) {
        IXMLDOMNodeList* windows{};
        document->selectNodes(_bstr_t(path), &windows);
        long window_count{};
        if (windows) windows->get_length(&window_count);
        for (long w = 0; w < window_count; ++w) {
            IXMLDOMNode* window{};
            windows->get_item(w, &window);
            if (!window) continue;
            const auto file = Attribute(window, L"image");
            if (!file.empty()) {
                if (auto image = skin.LoadBitmap(SkinAssetPath(directory, file))) {
                    background = image;
                    size = image.Size();
                }
            }
            IXMLDOMNodeList* children{};
            window->get_childNodes(&children);
            long count{};
            if (children) children->get_length(&count);
            for (long i = 0; i < count; ++i) {
                IXMLDOMNode* node{};
                children->get_item(i, &node);
                if (!node) continue;
                BSTR raw{};
                node->get_nodeName(&raw);
                const std::wstring name = raw ? raw : L"";
                SysFreeString(raw);
                if (PlayerSkinOrder(name) < 0) { node->Release(); continue; }
                auto it = std::find_if(elements.begin(), elements.end(),
                    [&](const auto& e) { return e.name == name; });
                if (it == elements.end()) {
                    elements.emplace_back();
                    it = std::prev(elements.end());
                    it->name = name;
                }
                auto& element = *it;
                element.four_state = IsFourStateButton(name);
                element.bounds = ParseRect(Attribute(node, L"position"), element.bounds);
                element.color = ParseColor(Attribute(node, L"color"), RGB(255,255,255));
                element.background = ParseColor(Attribute(node, L"bkgnd"), 0xff000000);
                element.alignment = ParseAlignment(Attribute(node, L"align"));
                const auto font = Attribute(node, L"font");
                if (!font.empty()) {
                    element.font = font;
                    element.font_size = ParseInt(Attribute(node, L"font_size"), 12);
                }
                element.vertical = _wcsicmp(Attribute(node, L"vertical").c_str(), L"true") == 0;
                const auto load = [&](const wchar_t* attr, SkinImage& image, SIZE& image_size) {
                    const auto file_name = Attribute(node, attr);
                    if (!file_name.empty()) {
                        if (auto candidate = skin.LoadBitmap(SkinAssetPath(directory, file_name))) {
                            image = candidate;
                            image_size = candidate.Size();
                        }
                    }
                };
                load(L"image", element.image, element.image_size);
                load(L"bar_image", element.bar_image, element.bar_size);
                load(L"fill_image", element.fill_image, element.fill_size);
                load(L"fill_image2", element.fill_image2, element.fill_size2);
                load(L"thumb_image", element.thumb_image, element.thumb_size);
                if (element.four_state && element.image) {
                    element.frames = 4;
                    element.bounds.right = element.bounds.left + element.image_size.cx / 4;
                    element.bounds.bottom = element.bounds.top + element.image_size.cy;
                }
                if (name == L"icon") {
                    const auto icon_file = Attribute(node, L"icon");
                    if (!icon_file.empty()) {
                        const auto icon = static_cast<HICON>(LoadImageW(nullptr,
                            (SkinAssetPath(directory, icon_file)).c_str(), IMAGE_ICON,
                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
                        if (icon) {
                            if (skin.icon_) DestroyIcon(skin.icon_);
                            skin.icon_ = icon;
                        }
                    }
                }
                LoadElementAnimation(skin, directory, node, element);
                node->Release();
            }
            if (children) children->Release();
            window->Release();
        }
        if (windows) windows->Release();
        std::stable_sort(elements.begin(), elements.end(), [](const auto& a, const auto& b) {
            return PlayerSkinOrder(a.name) < PlayerSkinOrder(b.name);
        });
    };
    load_player(L"/skin/player_window", skin.background_, skin.window_size_, skin.elements_);
    load_player(L"/skin/mini_window", skin.mini_.background, skin.mini_.window_size, skin.mini_.elements);
    if (!skin.background_) {
        document->Release();
        throw std::runtime_error("player skin bitmap unavailable");
    }

    // 004A88D0 stores lyric_window at skin-object offset +0x91C.  Its three
    // commands are real four-frame SkinButton children and its lyric rect is
    // enlarged by the same delta as the resizable popup.
    for (auto* lyric_window : Nodes(document, L"/skin/lyric_window")) {
        auto& lyric = skin.lyric_;
        lyric.position = ParseRect(Attribute(lyric_window, L"position"));
        lyric.resize_rect = ParseRect(Attribute(lyric_window, L"resize_rect"));
        lyric.resize_tile = ParseInt(Attribute(lyric_window, L"resize_tile"), 0) != 0;
        lyric.background = LoadSkinBitmap(skin, directory, lyric_window, L"image", &lyric.background);
        lyric.valid = lyric.background.image != nullptr;

        IXMLDOMNodeList* lyric_children{};
        lyric_window->get_childNodes(&lyric_children);
        long lyric_count{};
        if (lyric_children) lyric_children->get_length(&lyric_count);
        for (long index = 0; index < lyric_count; ++index) {
            IXMLDOMNode* node{};
            lyric_children->get_item(index, &node);
            if (!node) continue;
            DOMNodeType type{};
            node->get_nodeType(&type);
            if (type != NODE_ELEMENT) { node->Release(); continue; }
            BSTR raw_name{};
            node->get_nodeName(&raw_name);
            const std::wstring name = raw_name
                ? std::wstring(raw_name, SysStringLen(raw_name)) : std::wstring{};
            if (raw_name) SysFreeString(raw_name);
            if (name == L"title") {
                lyric.title = LoadEqualizerElement(skin, directory, node, name, false, false, &lyric.title);
            } else if (name == L"close") {
                lyric.close = LoadEqualizerElement(skin, directory, node, name, true, false, &lyric.close);
            } else if (name == L"ontop") {
                lyric.ontop = LoadEqualizerElement(skin, directory, node, name, true, false, &lyric.ontop);
            } else if (name == L"desklrc") {
                lyric.desklrc = LoadEqualizerElement(skin, directory, node, name, true, false, &lyric.desklrc);
            } else if (name == L"lyric") {
                lyric.lyric_bounds = ParseRect(Attribute(node, L"position"));
            } else if (name == L"mini_border") {
                lyric.mini_border_left_top = ParseColor(
                    Attribute(node, L"left_top_color"), 0xff000000);
                lyric.mini_border_right_bottom = ParseColor(
                    Attribute(node, L"right_bottom_color"), 0xff000000);
            } else if (name == L"mini_lyric") {
                // Compatibility extension for converted dialog-based skins;
                // not a node understood by the original 5.7.9 parser.
                InitializeDefaultLogFont(lyric.mini_font);
                lyric.mini_font_valid = ParseLogFont(
                    Attribute(node, L"Font"), lyric.mini_font);
                lyric.mini_text_color = ParseColor(Attribute(node, L"TextColor"), CLR_INVALID);
                lyric.mini_highlight_color = ParseColor(Attribute(node, L"HilightColor"), CLR_INVALID);
                lyric.mini_background_color = ParseColor(Attribute(node, L"BkgndColor"), CLR_INVALID);
                RECT padding{};
                const auto value = Attribute(node, L"padding");
                if (swscanf_s(value.c_str(), L"%ld , %ld , %ld , %ld",
                        &padding.left, &padding.top, &padding.right, &padding.bottom) == 4 &&
                    padding.left >= 0 && padding.top >= 0 &&
                    padding.right >= 0 && padding.bottom >= 0 &&
                    padding.left <= 1024 && padding.top <= 1024 &&
                    padding.right <= 1024 && padding.bottom <= 1024)
                    lyric.mini_padding = padding;
            }
            node->Release();
        }
        if (lyric_children) lyric_children->Release();
        lyric_window->Release();
        LoadLyricColors(directory, lyric);
    }

    // 004A92A2 parses /skin/desklrc_bar after the ordinary lyric window.
    // Unlike the main skin controls its XML rectangles are retained verbatim:
    // FUN_004186BC installs the four-frame images and then applies positions.
    for (auto* desktop_bar_node : Nodes(document, L"/skin/desklrc_bar")) {
        auto& bar = skin.desktop_lyric_bar_;
        bar.background = LoadSkinBitmap(skin, directory, desktop_bar_node, L"image", &bar.background);
        bar.transparent_color = ParseColor(
            Attribute(desktop_bar_node, L"transparent_color"),
            RGB(255, 0, 255));
        bar.valid = bar.background.image != nullptr;

        IXMLDOMNodeList* bar_children{};
        desktop_bar_node->get_childNodes(&bar_children);
        long bar_count{};
        if (bar_children) bar_children->get_length(&bar_count);
        for (long index = 0; index < bar_count; ++index) {
            IXMLDOMNode* node{};
            bar_children->get_item(index, &node);
            if (!node) continue;
            DOMNodeType type{};
            node->get_nodeType(&type);
            if (type != NODE_ELEMENT) {
                node->Release();
                continue;
            }
            BSTR raw_name{};
            node->get_nodeName(&raw_name);
            const std::wstring name = raw_name
                ? std::wstring(raw_name, SysStringLen(raw_name))
                : std::wstring{};
            if (raw_name) SysFreeString(raw_name);

            if (name == L"icon") {
                bar.icon = ParseRect(Attribute(node, L"position"));
            } else {
                SkinElement* target{};
                const struct { const wchar_t* name; SkinElement* field; } fields[] = {
                    {L"play", &bar.play}, {L"pause", &bar.pause}, {L"prev", &bar.previous},
                    {L"next", &bar.next}, {L"list", &bar.list}, {L"settings", &bar.settings},
                    {L"kalaok", &bar.karaoke}, {L"lines", &bar.lines}, {L"lock", &bar.lock},
                    {L"ontop", &bar.ontop}, {L"zoomin", &bar.zoom_in}, {L"zoomout", &bar.zoom_out},
                    {L"return", &bar.return_to_window}, {L"close", &bar.close}
                };
                for (const auto& field : fields) if (name == field.name) target = field.field;
                if (target) *target = LoadEqualizerElement(skin, directory, node, name, true, false, target);
            }
            node->Release();
        }
        if (bar_children) bar_children->Release();
        desktop_bar_node->Release();
    }

    // CSkinParser_ParseEqualizerWindow (004A8B73) dispatches these exact
    // node names.  In particular eqfactor is not a single control in the
    // native object: FUN_0042955B materializes ten copies at width+interval.
    for (auto* equalizer_window : Nodes(document, L"/skin/equalizer_window")) {
        auto& equalizer = skin.equalizer_;
        equalizer.position = ParseRect(Attribute(equalizer_window, L"position"));
        equalizer.eq_interval = ParseInt(Attribute(equalizer_window, L"eq_interval"), 0);
        equalizer.background = LoadSkinBitmap(skin, directory, equalizer_window, L"image", &equalizer.background);
        equalizer.valid = equalizer.background.image != nullptr;

        IXMLDOMNodeList* equalizer_children{};
        equalizer_window->get_childNodes(&equalizer_children);
        long equalizer_count{};
        if (equalizer_children) equalizer_children->get_length(&equalizer_count);
        for (long index = 0; index < equalizer_count; ++index) {
            IXMLDOMNode* node{};
            equalizer_children->get_item(index, &node);
            if (!node) continue;
            DOMNodeType type{};
            node->get_nodeType(&type);
            if (type != NODE_ELEMENT) { node->Release(); continue; }
            BSTR raw_name{};
            node->get_nodeName(&raw_name);
            const std::wstring name = raw_name
                ? std::wstring(raw_name, SysStringLen(raw_name)) : std::wstring{};
            if (raw_name) SysFreeString(raw_name);
            if (name == L"title") {
                equalizer.title = LoadEqualizerElement(skin, directory, node, name, false, false, &equalizer.title);
            } else if (name == L"close") {
                equalizer.close = LoadEqualizerElement(skin, directory, node, name, true, false, &equalizer.close);
            } else if (name == L"enabled") {
                equalizer.enabled = LoadEqualizerElement(skin, directory, node, name, true, false, &equalizer.enabled);
            } else if (name == L"profile") {
                equalizer.profile = LoadEqualizerElement(skin, directory, node, name, true, false, &equalizer.profile);
            } else if (name == L"reset") {
                equalizer.reset = LoadEqualizerElement(skin, directory, node, name, true, false, &equalizer.reset);
            } else if (name == L"balance") {
                equalizer.balance = LoadEqualizerElement(skin, directory, node, name, false, false, &equalizer.balance);
            } else if (name == L"surround") {
                equalizer.surround = LoadEqualizerElement(skin, directory, node, name, false, false, &equalizer.surround);
            } else if (name == L"preamp") {
                equalizer.preamp = LoadEqualizerElement(skin, directory, node, name, false, true, &equalizer.preamp);
            } else if (name == L"eqfactor") {
                auto band = LoadEqualizerElement(skin, directory, node, name, false, true, &equalizer.bands[0]);
                const LONG stride = (band.bounds.right - band.bounds.left) +
                                    equalizer.eq_interval;
                for (size_t band_index = 0; band_index < equalizer.bands.size(); ++band_index) {
                    equalizer.bands[band_index] = band;
                    equalizer.bands[band_index].name =
                        L"eqfactor" + std::to_wstring(band_index);
                    OffsetRect(&equalizer.bands[band_index].bounds,
                               stride * static_cast<LONG>(band_index), 0);
                }
            }
            node->Release();
        }
        if (equalizer_children) equalizer_children->Release();
        equalizer_window->Release();
    }

    // 004A8DC6 parses playlist_window independently from player_window.  It
    // is deliberately loaded before the XML document is released so runtime
    // skin switching can update both HWNDs transactionally.
    for (auto* playlist_window : Nodes(document, L"/skin/playlist_window")) {
        auto& playlist = skin.playlist_;
        playlist.position = ParseRect(Attribute(playlist_window, L"position"));
        playlist.resize_rect = ParseRect(Attribute(playlist_window, L"resize_rect"));
        playlist.resize_tile = ParseInt(Attribute(playlist_window, L"resize_tile"), 0) != 0;
        playlist.background = LoadSkinBitmap(skin, directory, playlist_window, L"image", &playlist.background);
        playlist.valid = playlist.background.image != nullptr;

        IXMLDOMNodeList* playlist_children{};
        playlist_window->get_childNodes(&playlist_children);
        long playlist_count{};
        if (playlist_children) playlist_children->get_length(&playlist_count);
        for (long index = 0; index < playlist_count; ++index) {
            IXMLDOMNode* node{};
            playlist_children->get_item(index, &node);
            if (!node) continue;
            DOMNodeType type{};
            node->get_nodeType(&type);
            if (type != NODE_ELEMENT) { node->Release(); continue; }
            BSTR raw_name{};
            node->get_nodeName(&raw_name);
            const std::wstring name = raw_name
                ? std::wstring(raw_name, SysStringLen(raw_name)) : std::wstring{};
            if (raw_name) SysFreeString(raw_name);
            if (name == L"title") {
                playlist.title = LoadPlaylistImageElement(skin, directory, node, L"title", &playlist.title);
            } else if (name == L"close") {
                playlist.close = LoadPlaylistImageElement(skin, directory, node, L"close", &playlist.close);
            } else if (name == L"toolbar") {
                playlist.toolbar_bounds = ParseRect(Attribute(node, L"position"));
                playlist.toolbar_alignment = ParseAlignment(Attribute(node, L"align"));
                playlist.toolbar = LoadSkinBitmap(skin, directory, node, L"image", &playlist.toolbar);
                playlist.toolbar_hot = LoadSkinBitmap(skin, directory, node, L"hot_image", &playlist.toolbar_hot);
                playlist.toolbar_animation = LoadAnimation(node, 1);
                IXMLDOMNodeList* items{};
                node->selectNodes(_bstr_t(L"item"), &items);
                long toolbar_item_count{};
                if (items) items->get_length(&toolbar_item_count);
                for (long item_index = 0; item_index < toolbar_item_count; ++item_index) {
                    IXMLDOMNode* item{};
                    items->get_item(item_index, &item);
                    if (!item) continue;
                    const int slot = ParseInt(Attribute(item, L"index"), -1);
                    const RECT rect = ParseRect(Attribute(item, L"position"));
                    if (slot >= 0 && slot < 7 && rect.left >= 0 && rect.top >= 0 &&
                        rect.right > rect.left && rect.bottom > rect.top &&
                        rect.right <= playlist.toolbar_bounds.right - playlist.toolbar_bounds.left &&
                        rect.bottom <= playlist.toolbar_bounds.bottom - playlist.toolbar_bounds.top) {
                        if (!playlist.toolbar_items)
                            playlist.toolbar_items.emplace(std::array<RECT, 7>{});
                        (*playlist.toolbar_items)[slot] = rect;
                    }
                    item->Release();
                }
                if (items) items->Release();
            } else if (name == L"scrollbar") {
                playlist.scrollbar_buttons = LoadSkinBitmap(skin, directory, node, L"buttons_image", &playlist.scrollbar_buttons);
                playlist.scrollbar_thumb = LoadSkinBitmap(skin, directory, node, L"thumb_image", &playlist.scrollbar_thumb);
                playlist.scrollbar_bar = LoadSkinBitmap(skin, directory, node, L"bar_image", &playlist.scrollbar_bar);
                playlist.scrollbar_thumb_resize_center = std::max(0, ParseInt(
                    Attribute(node, L"thumb_resize_center"), 0));
                playlist.scrollbar_thumb_resize_tile = ParseInt(
                    Attribute(node, L"thumb_resize_tile"), 0) != 0;
            } else if (name == L"playlist") {
                playlist.list_bounds = ParseRect(Attribute(node, L"position"));
                playlist.splitter_bar = LoadSkinBitmap(skin, directory, node, L"splitter_bar_image", &playlist.splitter_bar);
                playlist.splitter_arrow = LoadSkinBitmap(skin, directory, node, L"splitter_arrow_image", &playlist.splitter_arrow);
                playlist.selected = LoadSkinBitmap(skin, directory, node, L"selected_image", &playlist.selected);
            }
            node->Release();
        }
        if (playlist_children) playlist_children->Release();
        playlist_window->Release();
        LoadPlaylistColors(directory, playlist);
    }
    for (auto* elements : {&skin.elements_, &skin.mini_.elements}) {
        for (auto& element : *elements) {
            if (element.name == L"progress" && !element.fill_image2 && element.fill_image) {
                element.fill_image2 = element.fill_image.DarkenedBufferFill(skin.transparent_color_);
                element.fill_size2 = element.fill_image2.Size();
            }
        }
    }
    // CSkinManager_LoadPackageXml opens Visual.xml after the window layouts
    // and stores its sparse descriptor at skin-object offset +0x498.
    LoadVisualSettings(directory, skin.visual_);
    document->Release();
    return skin;
}

const SkinElement* LegacySkin::Find(std::wstring_view name) const {
    const auto found = std::find_if(elements_.begin(), elements_.end(), [&](const SkinElement& item) {
        return _wcsicmp(item.name.c_str(), std::wstring(name).c_str()) == 0;
    });
    return found == elements_.end() ? nullptr : &*found;
}

const SkinElement* LegacySkin::FindMini(std::wstring_view name) const {
    const auto found = std::find_if(mini_.elements.begin(), mini_.elements.end(),
        [&](const SkinElement& item) {
            return _wcsicmp(item.name.c_str(), std::wstring(name).c_str()) == 0;
        });
    return found == mini_.elements.end() ? nullptr : &*found;
}

HRGN LegacySkin::CreateWindowRegion(bool mini) const {
    const auto& image = mini ? mini_.background : background_;
    const auto mask = image.IsGdiPlus() ? image.CoverageMask() : image;
    const HBITMAP background = mask;
    const COLORREF transparent = image.IsGdiPlus() ? RGB(0, 0, 0) : transparent_color_;
    if (!background) return nullptr;
    BITMAP info{};
    GetObjectW(background, sizeof(info), &info);
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = info.bmWidth;
    bitmap_info.bmiHeader.biHeight = -info.bmHeight;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(info.bmWidth) * info.bmHeight * 4);
    const HDC dc = GetDC(nullptr);
    GetDIBits(dc, background, 0, static_cast<UINT>(info.bmHeight), pixels.data(), &bitmap_info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);

    HRGN result = CreateRectRgn(0, 0, 0, 0);
    for (int y = 0; y < info.bmHeight; ++y) {
        int run = -1;
        for (int x = 0; x <= info.bmWidth; ++x) {
            bool opaque = false;
            if (x < info.bmWidth) {
                const size_t offset = (static_cast<size_t>(y) * info.bmWidth + x) * 4;
                const COLORREF color = RGB(pixels[offset + 2], pixels[offset + 1], pixels[offset]);
                opaque = color != transparent;
            }
            if (opaque && run < 0) run = x;
            if (!opaque && run >= 0) {
                const HRGN row = CreateRectRgn(run, y, x, y + 1);
                CombineRgn(result, result, row, RGN_OR);
                DeleteObject(row);
                run = -1;
            }
        }
    }
    return result;
}
} // namespace ttplayer::skin
