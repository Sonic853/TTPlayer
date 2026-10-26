#include <ttpcomm/base64.h>
#include "ttplayer/skin/skin.h"
#include "ttplayer/skin/skin_package.h"
#include "ttplayer/settings/settings.h"

#include <algorithm>
#include <array>
#include "legacy_skin_xml.h"
#include <cstring>
#include <cwctype>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ttplayer::skin {
namespace {
std::wstring Attribute(const LegacyXmlNode* node, const wchar_t* name) {
    return node ? node->Attribute(name) : std::wstring{};
}

std::vector<LegacyXmlNode*> Nodes(const LegacyXmlDocument& document, const wchar_t* path) {
    return document.Nodes(path);
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

unsigned int ParseAlignment(std::wstring value, unsigned int initial = 0) {
    if (value.empty()) return 0;
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    unsigned int result = initial;
    const auto apply = [&](std::wstring_view token) {
        if (token == L"left") result = (result & 0xf0U) | 1U;
        else if (token == L"center") result = (result & 0xf0U) | 2U;
        else if (token == L"right") result = (result & 0xf0U) | 3U;
        else if (token == L"top") result = (result & 0x0fU) | 0x10U;
        else if (token == L"vcenter") result = (result & 0x0fU) | 0x20U;
        else if (token == L"bottom") result = (result & 0x0fU) | 0x30U;
    };
    // 004AA150 splits once; text passes initial=1, images initial=0.
    const auto split = value.find(L'+');
    apply(std::wstring_view(value).substr(0, split));
    if (split != std::wstring::npos) apply(std::wstring_view(value).substr(split + 1));
    return result;
}

std::wstring DecodeLegacyXml(std::span<const unsigned char> source, bool main_document = true) {
    if (source.empty() || source.size() > LegacyXmlDocument::MaxBytes)
        throw std::runtime_error("invalid Skin.xml length");
    std::string bytes(reinterpret_cast<const char*>(source.data()), source.size());
    const bool utf8_bom = bytes.starts_with("\xEF\xBB\xBF");
    if (utf8_bom) bytes.erase(0, 3);
    std::string declaration;
    const auto start = bytes.find_first_not_of(" \t\r\n\v\f");
    if (start != bytes.npos && bytes.compare(start, 5, "<?xml") == 0)
        declaration = bytes.substr(start, bytes.find("?>", start) - start);
    std::transform(declaration.begin(), declaration.end(), declaration.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    // 004A794C supplies CP936 for Skin.xml, irrespective of the system ACP.
    // Sidecars use 004A73B5's automatic/declaration-selected encoding.
    // Explicit UTF-8 and its BOM remain supported extensions for new skins.
    UINT code_page = main_document ? 936 : CP_ACP;
    if (utf8_bom || declaration.find("utf-8") != declaration.npos) code_page = CP_UTF8;
    else if (declaration.find("gb2312") != declaration.npos ||
             declaration.find("gbk") != declaration.npos) code_page = 936;
    const int count = MultiByteToWideChar(code_page, code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
        bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("unsupported Skin.xml encoding");
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(code_page, code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                        bytes.data(), static_cast<int>(bytes.size()), result.data(), count);

    return result;
}

std::wstring DecodeLegacyXml(const std::filesystem::path& path, bool main_document = true) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open Skin.xml");
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(LegacyXmlDocument::MaxBytes))
        throw std::runtime_error("invalid Skin.xml length");
    input.seekg(0);
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), length))
        throw std::runtime_error("truncated Skin.xml");
    return DecodeLegacyXml(bytes, main_document);
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
        LegacyXmlDocument document(DecodeLegacyXml(xml_bytes));
        const auto* root = document.First(L"/*[1]");
        if (!root) return std::nullopt;

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
        if (result.version != 2) return std::nullopt;
        return result;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

LegacySkin::~LegacySkin() = default;

LegacySkin::LegacySkin(LegacySkin&& other) noexcept {
    *this = std::move(other);
}

LegacySkin& LegacySkin::operator=(LegacySkin&& other) noexcept {
    if (this == &other) return *this;
    bitmaps_ = std::move(other.bitmaps_);
    // Layouts/cache share RAII images. Clear moved-from references as well.
    other.bitmaps_.clear();
    background_ = std::exchange(other.background_, nullptr);
    icons_ = std::move(other.icons_);
    assets_ = std::move(other.assets_);
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
    browser_ = std::move(other.browser_);
    other.browser_ = {};
    metadata_ = std::move(other.metadata_);
    visual_ = std::move(other.visual_);
    other.visual_ = {};
    return *this;
}

bool LegacySkin::PrepareAsset(const std::filesystem::path& path) const {
    if (path.empty()) return false;
    try { return assets_ ? assets_(path) : std::filesystem::is_regular_file(path); }
    catch (const std::exception&) { return false; }
}

SkinImage LegacySkin::LoadBitmap(const std::filesystem::path& path, bool* replacement_started) {
    if (replacement_started) *replacement_started = false;
    auto key = path.lexically_normal().wstring();
    std::transform(key.begin(), key.end(), key.begin(), towlower);
    if (const auto found = bitmaps_.find(key); found != bitmaps_.end()) {
        if (replacement_started) *replacement_started = true;
        return found->second;
    }
    if (!PrepareAsset(path)) return nullptr;
    const auto bitmap = SkinImage::Load(path, replacement_started);
    if (bitmap) bitmaps_.emplace(key, bitmap);
    return bitmap;
}

LegacySkin LegacySkin::LoadPackage(const SkinPackage& package,
    const std::filesystem::path& directory, HMODULE ttpcomm, const settings::Settings* current) {
    return Load(directory, current, [&](const std::filesystem::path& path) {
        return package.ExtractEntryTo(directory, path.lexically_relative(directory).generic_wstring(), ttpcomm);
    });
}

namespace {
bool HasIconBytes(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size != 0;
}

SkinAnimation LoadAnimation(LegacyXmlNode* node, int default_mode) {
    SkinAnimation result;
    result.mode = ParseInt(Attribute(node, L"flash_mode"), default_mode);
    result.frame_count = ParseInt(Attribute(node, L"frame_count"), 10);
    result.frame_interval = ParseInt(Attribute(node, L"frame_interval"), 100);
    // 00419637 disables invalid animation parameters, not the image/control.
    if (result.mode < 0 || result.mode > 3 || result.frame_count <= 0 ||
        result.frame_interval <= 0) result.mode = 0;
    return result;
}

void LoadElementBehavior(LegacySkin& skin, const std::filesystem::path& directory,
                          LegacyXmlNode* node, SkinElement& element) {
    const auto hit_test = Attribute(node, L"hit_test");
    if (_wcsicmp(hit_test.c_str(), L"rect") == 0) element.rectangular_hit_test = true;
    else if (_wcsicmp(hit_test.c_str(), L"alpha") == 0) element.rectangular_hit_test = false;
    element.animation = LoadAnimation(node, element.four_state ? 1 : 0);
    if (element.four_state && element.animation.mode > 2) element.animation.mode = 0;
    const auto flash = Attribute(node, L"flash_image");
    if (!flash.empty()) {
        bool replace{};
        auto image = skin.LoadBitmap(SkinAssetPath(directory, flash), &replace);
        if (replace) {
            element.flash_image = std::move(image);
            element.flash_size = element.flash_image.Size();
        }
    }
}

SkinBitmap LoadSkinBitmap(LegacySkin& skin, const std::filesystem::path& directory,
                          LegacyXmlNode* node, const wchar_t* attribute, const SkinBitmap* previous = nullptr) {
    SkinBitmap result = previous ? *previous : SkinBitmap{};
    const auto name = Attribute(node, attribute);
    if (name.empty()) return result;
    // Keep bitmap ownership centralized in LegacySkin::bitmaps_.
    bool replace{};
    auto candidate = skin.LoadBitmap(SkinAssetPath(directory, name), &replace);
    if (replace) {
        result.image = std::move(candidate);
        result.size = result.image.Size();
    }
    return result;
}

SkinElement LoadEqualizerElement(LegacySkin& skin,
                                 const std::filesystem::path& directory,
                                 LegacyXmlNode* node, std::wstring name,
                                 bool button, bool force_vertical = false,
                                 const SkinElement* previous = nullptr,
                                 bool image_alignment = false) {
    SkinElement element = previous ? *previous : SkinElement{};
    element.name = std::move(name);
    element.bounds = ParseRect(Attribute(node, L"position"), element.bounds);
    element.alignment = image_alignment ? ParseAlignment(Attribute(node, L"align")) : 0;
    const auto vertical = Attribute(node, L"vertical");
    element.vertical = force_vertical || _wcsicmp(vertical.c_str(), L"true") == 0;
    element.four_state = button;
    const auto load = [&](const wchar_t* attribute, SkinImage& bitmap, SIZE& size) {
        const auto file = Attribute(node, attribute);
        if (file.empty()) return;
        bool replace{};
        auto candidate = skin.LoadBitmap(SkinAssetPath(directory, file), &replace);
        if (!replace) return;
        bitmap = std::move(candidate);
        size = bitmap.Size();
    };
    load(L"image", element.image, element.image_size);
    load(L"bar_image", element.bar_image, element.bar_size);
    load(L"fill_image", element.fill_image, element.fill_size);
    load(L"fill_image2", element.fill_image2, element.fill_size2);
    load(L"thumb_image", element.thumb_image, element.thumb_size);
    LoadElementBehavior(skin, directory, node, element);
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
                                     LegacyXmlNode* node, std::wstring name,
                                     const SkinElement* previous = nullptr) {
    SkinElement element = previous ? *previous : SkinElement{};
    element.name = std::move(name);
    element.bounds = ParseRect(Attribute(node, L"position"), element.bounds);
    element.alignment = ParseAlignment(Attribute(node, L"align"));
    const auto image = Attribute(node, L"image");
    if (!image.empty()) {
        bool replace{};
        auto candidate = skin.LoadBitmap(SkinAssetPath(directory, image), &replace);
        if (replace) {
            element.image = std::move(candidate);
            element.image_size = element.image.Size();
        }
    }
    if (element.name == L"close" && element.image && !IsRectEmpty(&element.bounds)) {
        element.four_state = true;
        element.frames = 4;
        element.bounds.left = element.bounds.right - element.image_size.cx / 4;
        element.bounds.bottom = element.bounds.top + element.image_size.cy;
    }
    LoadElementBehavior(skin, directory, node, element);
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
    size_t comma = 0;
    bool face_present = true;
    for (size_t index = 0; index < value.size(); ++index) {
        comma = descriptor.find(L',', comma);
        if (comma == std::wstring::npos) { face_present = false; break; }
        ++comma;
    }
    if (face_present) wcsncpy_s(parsed.lfFaceName, descriptor.c_str() + comma, _TRUNCATE);
    font = parsed;
    return true;
}

void LoadLyricColors(const std::filesystem::path& directory, LyricSkin& lyric) {
    const auto path = directory / L"Lyric.xml";
    if (!std::filesystem::exists(path)) return;
    try {
        LegacyXmlDocument document(DecodeLegacyXml(path, false));
        if (const auto* node = document.First(L"/ttplayer_lyric/Lyric")) {
            static_cast<void>(ParseLogFont(Attribute(node, L"Font"), lyric.font));
            lyric.text_color = ParseColor(Attribute(node, L"TextColor"),
                                          lyric.text_color);
            lyric.highlight_color = ParseColor(Attribute(node, L"HilightColor"),
                                               lyric.highlight_color);
            lyric.background_color = ParseColor(Attribute(node, L"BkgndColor"),
                                                lyric.background_color);
        }
    } catch (const std::exception&) {
        // A malformed optional sidecar leaves the seeded settings unchanged.
    }
}

void LoadPlaylistColors(const std::filesystem::path& directory, PlaylistSkin& playlist) {
    const auto path = directory / L"Playlist.xml";
    if (!std::filesystem::exists(path)) return;
    try {
        LegacyXmlDocument document(DecodeLegacyXml(path, false));
        if (const auto* node = document.First(L"/ttplayer_playlist/PlayList")) {
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
        }
    } catch (const std::exception&) {
        // A malformed optional sidecar leaves the seeded settings unchanged.
    }
}

void LoadVisualSettings(const std::filesystem::path& directory,
                        VisualSkin& visual, const settings::Settings* current) {
    const auto path = directory / L"Visual.xml";
    if (!std::filesystem::exists(path)) return;
    try {
        LegacyXmlDocument document(DecodeLegacyXml(path, false));
        if (const auto* node = document.First(L"/ttplayer_visual/Visual")) {
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
            if (!blur.empty()) visual.blur = ParseInt(blur, 0) != 0;
            color(L"BlurScopeColor", visual.blur_scope_color);
            color(L"TextColor", visual.text_color);
            const auto descriptor = Attribute(node, L"Font");
            if (!descriptor.empty()) {
                LOGFONTW font{};
                InitializeDefaultLogFont(font);
                if (current && current->visual.font_valid) font = current->visual.font;
                if (ParseLogFont(descriptor, font)) visual.font = font;
            }
            visual.valid = true;
        }
    } catch (const std::exception&) {
        // A malformed optional sidecar leaves the seeded settings unchanged.
    }
}
} // namespace

LegacySkin LegacySkin::Load(const std::filesystem::path& directory,
                            const settings::Settings* current, AssetLoader assets) {
    LegacySkin skin;
    skin.assets_ = std::move(assets);
    if (!skin.PrepareAsset(directory / L"Skin.xml")) throw std::runtime_error("Skin.xml unavailable");
    LegacyXmlDocument document(DecodeLegacyXml(directory / L"Skin.xml"));

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
    const auto* root = document.First(L"/*[1]");
    if (!root || ParseInt(Attribute(root, L"version"), 0) != 2) {
        throw std::runtime_error("unsupported Skin.xml version (expected 2)");
    }
    skin.transparent_color_ = ParseColor(Attribute(root, L"transparent_color"), CLR_INVALID);
    skin.metadata_ = {2, Attribute(root, L"name"), Attribute(root, L"author"),
        Attribute(root, L"url"), Attribute(root, L"email"), skin.transparent_color_};

    // 004A8536 writes fixed slots. Repeated definitions update the same slot;
    // missing/failed bitmap loads preserve its previous image (004A9D8C).
    const auto load_player = [&](const wchar_t* path, SkinImage& background,
                                 SIZE& size, std::vector<SkinElement>& elements, SkinIcons& icons) {
        for (auto* window : document.Nodes(path)) {
            const auto file = Attribute(window, L"image");
            if (!file.empty()) {
                bool replace{};
                auto image = skin.LoadBitmap(SkinAssetPath(directory, file), &replace);
                if (replace) {
                    background = image;
                    size = image.Size();
                }
            }
            for (auto* node : window->Children()) {
                const std::wstring& name = node->name;
                if (PlayerSkinOrder(name) < 0) { continue; }
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
                const bool text = name == L"icon" || name == L"info" || name == L"stereo" ||
                    name == L"status" || name == L"login_name";
                element.alignment = text ? ParseAlignment(Attribute(node, L"align"), 1) :
                    (name == L"led" || name == L"visual") ? ParseAlignment(Attribute(node, L"align")) : 0;
                const auto font = Attribute(node, L"font");
                if (!font.empty()) {
                    element.font = font;
                    element.font_size = ParseInt(Attribute(node, L"font_size"), 12);
                }
                element.vertical = _wcsicmp(Attribute(node, L"vertical").c_str(), L"true") == 0;
                const auto load = [&](const wchar_t* attr, SkinImage& image, SIZE& image_size) {
                    const auto file_name = Attribute(node, attr);
                    if (!file_name.empty()) {
                        bool replace{};
                        auto candidate = skin.LoadBitmap(SkinAssetPath(directory, file_name), &replace);
                        if (replace) {
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
                    // The host only binds a bitmap to a nonempty descriptor.
                    // An absent/unparseable position remains a zero-size child;
                    // it must not acquire a hit target merely from its image.
                    if (!IsRectEmpty(&element.bounds)) {
                        element.bounds.right = element.bounds.left + element.image_size.cx / 4;
                        element.bounds.bottom = element.bounds.top + element.image_size.cy;
                    }
                }
                if (name == L"icon") {
                    const auto icon_path = SkinAssetPath(directory, Attribute(node, L"icon"));
                    if (skin.PrepareAsset(icon_path) && HasIconBytes(icon_path)) {
                        icons.small_icon.Reset(static_cast<HICON>(LoadImageW(nullptr, icon_path.c_str(), IMAGE_ICON,
                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE)));
                        icons.large_icon.Reset(static_cast<HICON>(LoadImageW(nullptr, icon_path.c_str(), IMAGE_ICON,
                            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE)));
                    }
                }
                LoadElementBehavior(skin, directory, node, element);
            }
        }
        // The original always owns a SkinIcon child. 0045C937 installs the
        // application fallback even when the descriptor has no <icon> node.
        if (background && std::none_of(elements.begin(), elements.end(),
            [](const auto& element) { return element.name == L"icon"; })) {
            SkinElement icon;
            icon.name = L"icon";
            elements.push_back(std::move(icon));
        }
        std::stable_sort(elements.begin(), elements.end(), [](const auto& a, const auto& b) {
            return PlayerSkinOrder(a.name) < PlayerSkinOrder(b.name);
        });
    };
    load_player(L"/*[1]/player_window", skin.background_, skin.window_size_, skin.elements_, skin.icons_);
    load_player(L"/*[1]/mini_window", skin.mini_.background, skin.mini_.window_size, skin.mini_.elements, skin.mini_.icons);
    if (!skin.background_) {
        throw std::runtime_error("player skin bitmap unavailable");
    }

    // 004A9051: the optional browser layout remains a host skin descriptor.
    // The DLL receives owned bitmap copies through its C ABI when available.
    for (auto* window : Nodes(document, L"/*[1]/browser_window")) {
        auto& browser = skin.browser_;
        browser.position = ParseRect(Attribute(window, L"position"), browser.position);
        browser.resize_rect = ParseRect(Attribute(window, L"resize_rect"), browser.resize_rect);
        browser.resize_tile = ParseInt(Attribute(window, L"resize_tile"), 0) != 0;
        browser.transparent_color = ParseColor(Attribute(window, L"transparent_color"), skin.transparent_color_);
        browser.background = LoadSkinBitmap(skin, directory, window, L"image", &browser.background);
        browser.valid = browser.background.image != nullptr;
        for (auto* node : window->Children()) {
            const auto& name = node->name;
            if (name == L"browser") {
                browser.content = ParseRect(Attribute(node, L"position"), browser.content);
                continue;
            }
            SkinElement* target{};
            const struct { const wchar_t* name; SkinElement* field; } fields[] = {
                {L"close", &browser.close}, {L"minibtn", &browser.music},
                {L"downloadbtn", &browser.downloads}, {L"backward", &browser.back},
                {L"forward", &browser.forward}, {L"refresh", &browser.refresh},
                {L"startup", &browser.startup}, {L"linktxt", &browser.link}
            };
            for (const auto& field : fields) if (name == field.name) target = field.field;
            if (!target) continue;
            const bool button = name != L"startup" && name != L"linktxt";
            *target = LoadEqualizerElement(skin, directory, node, name, button, false, target, true);
            target->color = ParseColor(Attribute(node, L"color"), target->color);
            target->font = Attribute(node, L"font");
            target->font_size = ParseInt(Attribute(node, L"font_size"), 12);
            if (button && target->image && !IsRectEmpty(&target->bounds)) {
                target->bounds.right = target->bounds.left + target->image_size.cx / 4;
                target->bounds.bottom = target->bounds.top + target->image_size.cy;
            }
            if (name == L"startup") {
                browser.checkbox_interval = ParseInt(Attribute(node, L"interval"), 4);
                const auto image = LoadSkinBitmap(skin, directory, node, L"ckbox_image");
                target->image = image.image; target->image_size = image.size;
                target->frames = 4;
            }
        }
    }

    // 004A88D0 stores lyric_window at skin-object offset +0x91C.  Its three
    // commands are real four-frame SkinButton children and its lyric rect is
    // enlarged by the same delta as the resizable popup.
    for (auto* lyric_window : Nodes(document, L"/*[1]/lyric_window")) {
        auto& lyric = skin.lyric_;
        lyric.mini_border_left_top = lyric.mini_border_right_bottom = 0xff000000;
        lyric.position = ParseRect(Attribute(lyric_window, L"position"), lyric.position);
        lyric.resize_rect = ParseRect(Attribute(lyric_window, L"resize_rect"), lyric.resize_rect);
        lyric.resize_tile = ParseInt(Attribute(lyric_window, L"resize_tile"), 0) != 0;
        lyric.background = LoadSkinBitmap(skin, directory, lyric_window, L"image", &lyric.background);
        lyric.valid = lyric.background.image != nullptr;

        for (auto* node : lyric_window->Children()) {
            const std::wstring& name = node->name;
            if (name == L"title") {
                lyric.title = LoadEqualizerElement(skin, directory, node, name, false, false, &lyric.title, true);
            } else if (name == L"close") {
                lyric.close = LoadEqualizerElement(skin, directory, node, name, true, false, &lyric.close, true);
            } else if (name == L"ontop") {
                lyric.ontop = LoadEqualizerElement(skin, directory, node, name, true, false, &lyric.ontop, true);
            } else if (name == L"desklrc") {
                lyric.desklrc = LoadEqualizerElement(skin, directory, node, name, true, false, &lyric.desklrc, true);
            } else if (name == L"lyric") {
                lyric.lyric_bounds = ParseRect(Attribute(node, L"position"), lyric.lyric_bounds);
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
        }
    }

    // 004A92A2 parses /skin/desklrc_bar after the ordinary lyric window.
    // Unlike the main skin controls its XML rectangles are retained verbatim:
    // FUN_004186BC installs the four-frame images and then applies positions.
    for (auto* desktop_bar_node : Nodes(document, L"/*[1]/desklrc_bar")) {
        auto& bar = skin.desktop_lyric_bar_;
        bar.background = LoadSkinBitmap(skin, directory, desktop_bar_node, L"image", &bar.background);
        bar.transparent_color = ParseColor(
            Attribute(desktop_bar_node, L"transparent_color"),
            CLR_INVALID);
        bar.valid = bar.background.image != nullptr;

        for (auto* node : desktop_bar_node->Children()) {
            const std::wstring& name = node->name;

            if (name == L"icon") {
                bar.icon = ParseRect(Attribute(node, L"position"), bar.icon);
                const auto path = SkinAssetPath(directory, Attribute(node, L"icon"));
                if (skin.PrepareAsset(path) && HasIconBytes(path))
                    bar.icon_image.Reset(static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON,
                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE)));
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
        }
    }

    // CSkinParser_ParseEqualizerWindow (004A8B73) dispatches these exact
    // node names.  In particular eqfactor is not a single control in the
    // native object: FUN_0042955B materializes ten copies at width+interval.
    SkinElement band_template;
    for (auto* equalizer_window : Nodes(document, L"/*[1]/equalizer_window")) {
        auto& equalizer = skin.equalizer_;
        equalizer.position = ParseRect(Attribute(equalizer_window, L"position"), equalizer.position);
        equalizer.eq_interval = ParseInt(Attribute(equalizer_window, L"eq_interval"), 0);
        equalizer.background = LoadSkinBitmap(skin, directory, equalizer_window, L"image", &equalizer.background);
        equalizer.valid = equalizer.background.image != nullptr;

        for (auto* node : equalizer_window->Children()) {
            const std::wstring& name = node->name;
            if (name == L"title") {
                equalizer.title = LoadEqualizerElement(skin, directory, node, name, false, false, &equalizer.title, true);
            } else if (name == L"close") {
                equalizer.close = LoadEqualizerElement(skin, directory, node, name, true, false, &equalizer.close, true);
            } else if (name == L"enabled") {
                equalizer.enabled = LoadEqualizerElement(skin, directory, node, name, true, false, &equalizer.enabled);
            } else if (name == L"profile") {
                equalizer.profile = LoadEqualizerElement(skin, directory, node, name, true, false, &equalizer.profile);
            } else if (name == L"reset") {
                equalizer.reset = LoadEqualizerElement(skin, directory, node, name, true, false, &equalizer.reset);
            } else if (name == L"balance") {
                equalizer.balance = LoadEqualizerElement(skin, directory, node, name, false, false, &equalizer.balance);
                equalizer.balance.vertical = false; // 0042955B binds balance horizontally.
            } else if (name == L"surround") {
                equalizer.surround = LoadEqualizerElement(skin, directory, node, name, false, false, &equalizer.surround);
            } else if (name == L"preamp") {
                equalizer.preamp = LoadEqualizerElement(skin, directory, node, name, false, true, &equalizer.preamp);
            } else if (name == L"eqfactor") {
                band_template = LoadEqualizerElement(skin, directory, node, name, false, true, &band_template);
            }
        }
    }

    if (!band_template.name.empty()) {
        const LONG stride = (band_template.bounds.right - band_template.bounds.left) + skin.equalizer_.eq_interval;
        for (size_t i = 0; i < skin.equalizer_.bands.size(); ++i) {
            auto& band = skin.equalizer_.bands[i];
            band = band_template;
            band.name = L"eqfactor" + std::to_wstring(i);
            OffsetRect(&band.bounds, stride * static_cast<LONG>(i), 0);
        }
    }

    // 004A8DC6 parses playlist_window independently from player_window.  It
    // is deliberately loaded before the XML document is released so runtime
    // skin switching can update both HWNDs transactionally.
    for (auto* playlist_window : Nodes(document, L"/*[1]/playlist_window")) {
        auto& playlist = skin.playlist_;
        playlist.position = ParseRect(Attribute(playlist_window, L"position"), playlist.position);
        playlist.resize_rect = ParseRect(Attribute(playlist_window, L"resize_rect"), playlist.resize_rect);
        playlist.resize_tile = ParseInt(Attribute(playlist_window, L"resize_tile"), 0) != 0;
        playlist.background = LoadSkinBitmap(skin, directory, playlist_window, L"image", &playlist.background);
        playlist.valid = playlist.background.image != nullptr;

        for (auto* node : playlist_window->Children()) {
            const std::wstring& name = node->name;
            if (name == L"title") {
                playlist.title = LoadPlaylistImageElement(skin, directory, node, L"title", &playlist.title);
            } else if (name == L"close") {
                playlist.close = LoadPlaylistImageElement(skin, directory, node, L"close", &playlist.close);
            } else if (name == L"toolbar") {
                playlist.toolbar_bounds = ParseRect(Attribute(node, L"position"), playlist.toolbar_bounds);
                playlist.toolbar_alignment = ParseAlignment(Attribute(node, L"align"));
                playlist.toolbar = LoadSkinBitmap(skin, directory, node, L"image", &playlist.toolbar);
                playlist.toolbar_hot = LoadSkinBitmap(skin, directory, node, L"hot_image", &playlist.toolbar_hot);
                playlist.toolbar_animation = LoadAnimation(node, 1);
                for (auto* item : node->Children(L"item")) {
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
                }
            } else if (name == L"scrollbar") {
                playlist.scrollbar_buttons = LoadSkinBitmap(skin, directory, node, L"buttons_image", &playlist.scrollbar_buttons);
                playlist.scrollbar_thumb = LoadSkinBitmap(skin, directory, node, L"thumb_image", &playlist.scrollbar_thumb);
                playlist.scrollbar_bar = LoadSkinBitmap(skin, directory, node, L"bar_image", &playlist.scrollbar_bar);
                playlist.scrollbar_thumb_resize_center = ParseInt(
                    Attribute(node, L"thumb_resize_center"), playlist.scrollbar_thumb_resize_center);
                playlist.scrollbar_thumb_resize_tile = ParseInt(
                    Attribute(node, L"thumb_resize_tile"), playlist.scrollbar_thumb_resize_tile ? 1 : 0) != 0;
            } else if (name == L"playlist") {
                playlist.list_bounds = ParseRect(Attribute(node, L"position"), playlist.list_bounds);
                playlist.splitter_bar = LoadSkinBitmap(skin, directory, node, L"splitter_bar_image", &playlist.splitter_bar);
                playlist.splitter_arrow = LoadSkinBitmap(skin, directory, node, L"splitter_arrow_image", &playlist.splitter_arrow);
                playlist.selected = LoadSkinBitmap(skin, directory, node, L"selected_image", &playlist.selected);
            }
        }
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
    if (skin.PrepareAsset(directory / L"Lyric.xml")) LoadLyricColors(directory, skin.lyric_);
    if (skin.PrepareAsset(directory / L"Playlist.xml")) LoadPlaylistColors(directory, skin.playlist_);
    if (skin.PrepareAsset(directory / L"Visual.xml")) LoadVisualSettings(directory, skin.visual_, current);
    skin.assets_ = {};
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
