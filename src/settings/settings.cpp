#include "ttplayer/settings/settings.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <msxml6.h>
#include <comdef.h>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <utility>
#include <wrl/client.h>

namespace ttplayer::settings {
namespace {
using Microsoft::WRL::ComPtr;

template <class Interface>
ComPtr<Interface> AdoptCom(Interface* value) noexcept {
    ComPtr<Interface> result;
    result.Attach(value);
    return result;
}

// CSettings serializes the fixed 54-command table shipped by TTPlayer.  The
// remaining indexed fields back small UI lists; their limits are deliberately
// above any distributed configuration while keeping corrupt counts bounded.
constexpr size_t kMaxHotKeyBindings=54;
constexpr size_t kMaxLyricFolders=256;
constexpr size_t kMaxLibraryDirectories=256;
constexpr size_t kMaxNetworkServers=64;
constexpr size_t kMaxPluginModules=256;
constexpr size_t kMaxHistoryTagNames=256;

struct ComInit { HRESULT hr{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)}; ~ComInit(){ if (SUCCEEDED(hr)) CoUninitialize(); } };
_variant_t Attribute(IXMLDOMNode* node, const wchar_t* name) {
    ComPtr<IXMLDOMNamedNodeMap> map;
    ComPtr<IXMLDOMNode> attr;
    if (!node || FAILED(node->get_attributes(map.GetAddressOf())) || !map)
        return {};
    map->getNamedItem(_bstr_t(name),attr.GetAddressOf());
    if (!attr) return {};
    VARIANT value{};
    VariantInit(&value);
    attr->get_nodeValue(&value);
    return _variant_t(value, false);
}
int IntAttr(IXMLDOMNode* node, const wchar_t* name, int fallback) { auto v=Attribute(node,name); return v.vt==VT_EMPTY ? fallback : static_cast<int>(v); }
RECT RectAttr(IXMLDOMNode* node, const wchar_t* name) {
    RECT result{};
    const auto value=Attribute(node,name);
    if (value.vt==VT_EMPTY) return result;
    const auto text=static_cast<const wchar_t*>(_bstr_t(value));
    if (swscanf_s(text,L"%ld , %ld , %ld , %ld",&result.left,&result.top,
                  &result.right,&result.bottom)!=4) return {};
    return result;
}
std::wstring StringAttr(IXMLDOMNode* node,const wchar_t* name) {
    const auto value=Attribute(node,name);
    return value.vt==VT_EMPTY ? std::wstring{} :
        std::wstring(static_cast<const wchar_t*>(_bstr_t(value)));
}

template <size_t Size>
std::array<int, Size> FixedIntegerListAttr(
    IXMLDOMNode* node, const wchar_t* name, wchar_t separator,
    const std::array<int, Size>& fallback) {
    const auto text=StringAttr(node,name);
    if(text.empty()) return fallback;
    std::wistringstream input(text);
    std::array<int,Size> result{};
    for(size_t index=0; index<Size; ++index) {
        if(!(input>>result[index])) return fallback;
        if(index+1<Size) {
            wchar_t delimiter{};
            if(!(input>>delimiter) || delimiter!=separator) return fallback;
        }
    }
    return result;
}

template <size_t Size>
std::wstring FixedIntegerListText(const std::array<int, Size>& values,
                                  wchar_t separator) {
    std::wostringstream output;
    for(size_t index=0; index<Size; ++index) {
        if(index) output<<separator;
        output<<values[index];
    }
    return output.str();
}

std::wstring ClockText(const std::array<int,3>& value) {
    wchar_t text[64]{};
    swprintf_s(text,L"%02d:%02d:%02d",value[0],value[1],value[2]);
    return text;
}
COLORREF ColorAttr(IXMLDOMNode* node,const wchar_t* name,COLORREF fallback) {
    const auto value=StringAttr(node,name);
    unsigned int red{},green{},blue{};
    // FUN_0048DD6C/FUN_0048DDB2 use "#%2x%2x%2x".  They accept exactly
    // three parsed byte groups but deliberately ignore trailing characters;
    // several shipped Visual.xml files rely on that permissive lexer.
    if(swscanf_s(value.c_str(),L"#%2x%2x%2x",&red,&green,&blue)!=3)
        return fallback;
    return RGB(red,green,blue);
}

std::wstring IndexedAttributeName(const wchar_t* prefix,
                                  const wchar_t* suffix) {
    std::wstring name(prefix);
    name += suffix;
    return name;
}

std::optional<size_t> BoundedIndexedCount(IXMLDOMNode* node,
                                          const wchar_t* prefix,
                                          size_t hard_limit) {
    const auto count_name=IndexedAttributeName(prefix,L"_Count");
    const auto value=Attribute(node,count_name.c_str());
    if(value.vt==VT_EMPTY) return std::nullopt;
    try {
        const int count=static_cast<int>(value);
        if(count<0) return std::nullopt;
        return std::min(static_cast<size_t>(count),hard_limit);
    } catch(const _com_error&) {
        return std::nullopt;
    }
}

std::optional<std::vector<std::wstring>> IndexedStringAttrs(
    IXMLDOMNode* node,const wchar_t* prefix,size_t hard_limit) {
    const auto count=BoundedIndexedCount(node,prefix,hard_limit);
    if(!count) return std::nullopt;
    std::vector<std::wstring> result;
    result.reserve(*count);
    for(size_t index=0;index<*count;++index) {
        const auto name=std::wstring(prefix)+L"_"+std::to_wstring(index);
        const auto value=Attribute(node,name.c_str());
        if(value.vt==VT_EMPTY) break;
        result.emplace_back(static_cast<const wchar_t*>(_bstr_t(value)));
    }
    return result;
}

HotKeyBinding ParseHotKeyBinding(std::wstring text) {
    HotKeyBinding result;
    result.raw_text=std::move(text);
    int command{},application_key{},application_modifiers{};
    int global_key{},global_modifiers{};
    if(swscanf_s(result.raw_text.c_str(),L"%d:a(%d,%d),g(%d,%d)",
                 &command,&application_key,&application_modifiers,
                 &global_key,&global_modifiers)==5) {
        result.command=command;
        result.application={application_key,application_modifiers};
        result.global={global_key,global_modifiers};
        result.raw_text.clear();
    }
    return result;
}

std::wstring HotKeyBindingText(const HotKeyBinding& binding) {
    if(binding.command<0) return binding.raw_text;
    wchar_t text[128]{};
    swprintf_s(text,L"%d:a(%d,%d),g(%d,%d)",binding.command,
               binding.application.virtual_key,binding.application.modifiers,
               binding.global.virtual_key,binding.global.modifiers);
    return text;
}

MediaLibraryDirectory ParseMediaLibraryDirectory(std::wstring text) {
    MediaLibraryDirectory result;
    if(!text.empty() && text.front()==L'*') {
        result.enabled=true;
        text.erase(text.begin());
    }
    result.path=std::move(text);
    return result;
}

std::wstring MediaLibraryDirectoryText(
    const MediaLibraryDirectory& directory) {
    std::wstring result=directory.path.wstring();
    if(directory.enabled) result.insert(result.begin(),L'*');
    return result;
}

void LoadDesktopLyricProfile(IXMLDOMNode* node,const wchar_t* prefix,
                             DesktopLyricColorProfile& profile,
                             bool include_name) {
    const std::wstring base(prefix);
    if(include_name)
        profile.name=StringAttr(node,(base+L"_Name").c_str());
    profile.background_count=IntAttr(
        node,(base+L"_BCount").c_str(),profile.background_count);
    profile.played_count=IntAttr(
        node,(base+L"_PCount").c_str(),profile.played_count);
    for(size_t index=0;index<profile.background_colors.size();++index) {
        const auto number=std::to_wstring(index+1);
        profile.background_colors[index]=ColorAttr(
            node,(base+L"_BColor"+number).c_str(),
            profile.background_colors[index]);
        profile.played_colors[index]=ColorAttr(
            node,(base+L"_PColor"+number).c_str(),
            profile.played_colors[index]);
    }
}

const HotKeyBinding kDefaultHotKeyMap[]{
    {57664, {112, 0}, {0, 0}},
    {32100, {113, 0}, {0, 0}},
    {32101, {114, 0}, {0, 0}},
    {32102, {115, 0}, {0, 0}},
    {32104, {122, 0}, {0, 0}},
    {32000, {116, 0}, {116, 6}},
    {32002, {117, 0}, {117, 6}},
    {32003, {118, 0}, {118, 6}},
    {32004, {119, 0}, {119, 6}},
    {32005, {37, 10}, {37, 14}},
    {32006, {39, 10}, {39, 14}},
    {32010, {38, 10}, {38, 14}},
    {32011, {40, 10}, {40, 14}},
    {32007, {83, 10}, {83, 14}},
    {32008, {68, 10}, {68, 14}},
    {32009, {85, 10}, {85, 14}},
    {32103, {73, 10}, {0, 0}},
    {57601, {79, 10}, {0, 0}},
    {57602, {67, 12}, {0, 0}},
    {32300, {69, 10}, {0, 0}},
    {32521, {70, 10}, {0, 0}},
    {32522, {80, 10}, {0, 0}},
    {32525, {0, 0}, {0, 0}},
    {32580, {0, 0}, {0, 0}},
    {32588, {0, 0}, {0, 0}},
    {32212, {77, 10}, {77, 14}},
    {32213, {87, 12}, {87, 14}},
    {32215, {84, 12}, {84, 14}},
    {1033, {86, 12}, {86, 14}},
    {2151, {66, 12}, {66, 14}},
    {32569, {0, 0}, {0, 0}},
    {32570, {0, 0}, {0, 0}},
    {32571, {74, 10}, {0, 0}},
    {32815, {76, 10}, {76, 14}},
    {32813, {0, 0}, {0, 0}},
    {32812, {0, 0}, {0, 0}},
    {32804, {0, 0}, {0, 0}},
    {32814, {0, 0}, {0, 0}},
    {32805, {0, 0}, {0, 0}},
    {32806, {0, 0}, {0, 0}},
    {32807, {0, 0}, {0, 0}},
    {32808, {0, 0}, {0, 0}},
    {32809, {0, 0}, {0, 0}},
    {32810, {0, 0}, {0, 0}},
    {32811, {0, 0}, {0, 0}},
    {32820, {0, 0}, {0, 0}},
    {32821, {0, 0}, {0, 0}},
    {32836, {84, 10}, {0, 0}},
    {32837, {69, 12}, {69, 6}},
    {32840, {120, 0}, {0, 0}},
    {32841, {121, 0}, {0, 0}},
    {32842, {46, 10}, {0, 0}},
    {32230, {70, 12}, {70, 6}},
    {57665, {88, 12}, {0, 0}},
};
static_assert(std::size(kDefaultHotKeyMap)==kMaxHotKeyBindings);
ComPtr<IXMLDOMNode> SelectOwned(IXMLDOMDocument* document,
                                const wchar_t* xpath) {
    ComPtr<IXMLDOMNode> node;
    if(document)
        document->selectSingleNode(_bstr_t(xpath),node.GetAddressOf());
    return node;
}
std::array<int, 11> EqualizerAttr(IXMLDOMNode* node, const wchar_t* name) {
    std::array<int, 11> result{};
    auto text = StringAttr(node, name);
    std::replace(text.begin(), text.end(), L':', L',');
    std::wistringstream input(text);
    for (auto& value : result) {
        if (!(input >> value)) break;
        if (input.peek() == L',') input.get();
    }
    return result;
}
std::vector<unsigned char> DecodeBase64(std::wstring_view source) {
    std::array<int,256> values{};
    values.fill(-1);
    constexpr char alphabet[]=
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(int index=0;index<64;++index)
        values[static_cast<unsigned char>(alphabet[index])]=index;
    std::vector<unsigned char> output;
    output.reserve(source.size()*3/4);
    unsigned int accumulator{};
    int bits{};
    for(const wchar_t wide:source) {
        if(wide==L'=') break;
        if(iswspace(wide)) continue;
        if(wide<0 || wide>0xff ||
           values[static_cast<unsigned char>(wide)]<0) return {};
        accumulator=(accumulator<<6) |
            static_cast<unsigned int>(values[static_cast<unsigned char>(wide)]);
        bits+=6;
        if(bits>=8) {
            bits-=8;
            output.push_back(static_cast<unsigned char>(accumulator>>bits));
            accumulator&=(1U<<bits)-1U;
        }
    }
    return output;
}

bool LogFontAttr(IXMLDOMNode* node, const wchar_t* name, LOGFONTW& result) {
    const auto text=StringAttr(node,name);
    if(text.empty()) return false;
    if(_wcsnicmp(text.c_str(),L"base64:",7)==0) {
        const auto bytes=DecodeBase64(std::wstring_view(text).substr(7));
        LOGFONTW parsed{};
        if(bytes.size()==sizeof(LOGFONTW)) {
            std::memcpy(&parsed,bytes.data(),sizeof(parsed));
        } else if(bytes.size()==sizeof(LOGFONTA)) {
            LOGFONTA ansi{};
            std::memcpy(&ansi,bytes.data(),sizeof(ansi));
            parsed.lfHeight=ansi.lfHeight;
            parsed.lfWidth=ansi.lfWidth;
            parsed.lfEscapement=ansi.lfEscapement;
            parsed.lfOrientation=ansi.lfOrientation;
            parsed.lfWeight=ansi.lfWeight;
            parsed.lfItalic=ansi.lfItalic;
            parsed.lfUnderline=ansi.lfUnderline;
            parsed.lfStrikeOut=ansi.lfStrikeOut;
            parsed.lfCharSet=ansi.lfCharSet;
            parsed.lfOutPrecision=ansi.lfOutPrecision;
            parsed.lfClipPrecision=ansi.lfClipPrecision;
            parsed.lfQuality=ansi.lfQuality;
            parsed.lfPitchAndFamily=ansi.lfPitchAndFamily;
            if(MultiByteToWideChar(CP_ACP,0,ansi.lfFaceName,-1,
                                   parsed.lfFaceName,LF_FACESIZE)==0)
                return false;
        } else {
            return false;
        }
        result=parsed;
        return true;
    }
    std::wistringstream input(text);
    long values[5]{};
    int bytes[8]{};
    wchar_t comma{};
    for(auto& value:values) {
        if(!(input>>value)) return false;
        if(!(input>>comma) || comma!=L',') return false;
    }
    for(auto& value:bytes) {
        if(!(input>>value)) return false;
        if(!(input>>comma) || comma!=L',') return false;
    }
    std::wstring face;
    std::getline(input,face);
    LOGFONTW parsed{};
    parsed.lfHeight=values[0]; parsed.lfWidth=values[1];
    parsed.lfEscapement=values[2]; parsed.lfOrientation=values[3];
    parsed.lfWeight=values[4]; parsed.lfItalic=static_cast<BYTE>(bytes[0]);
    parsed.lfUnderline=static_cast<BYTE>(bytes[1]);
    parsed.lfStrikeOut=static_cast<BYTE>(bytes[2]);
    parsed.lfCharSet=static_cast<BYTE>(bytes[3]);
    parsed.lfOutPrecision=static_cast<BYTE>(bytes[4]);
    parsed.lfClipPrecision=static_cast<BYTE>(bytes[5]);
    parsed.lfQuality=static_cast<BYTE>(bytes[6]);
    parsed.lfPitchAndFamily=static_cast<BYTE>(bytes[7]);
    // FUN_0048DE50 accepts an empty final face-name field.  Two distributed
    // sidecars use it while still relying on all preceding LOGFONT values.
    wcsncpy_s(parsed.lfFaceName,face.c_str(),_TRUNCATE);
    result=parsed;
    return true;
}

LOGFONTW PlaylistLogFont(const PlaylistSettings& playlist) {
    if (playlist.font_descriptor_valid) return playlist.font_descriptor;
    LOGFONTW result{};
    result.lfHeight=playlist.font_height;
    result.lfWeight=FW_NORMAL;
    result.lfCharSet=DEFAULT_CHARSET;
    result.lfOutPrecision=OUT_DEFAULT_PRECIS;
    result.lfClipPrecision=CLIP_DEFAULT_PRECIS;
    result.lfQuality=DEFAULT_QUALITY;
    result.lfPitchAndFamily=DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy_s(result.lfFaceName,playlist.font.c_str(),_TRUNCATE);
    return result;
}

void SetPlaylistLogFont(PlaylistSettings& playlist,const LOGFONTW& font) {
    playlist.font_descriptor=font;
    playlist.font_descriptor_valid=true;
    playlist.font=font.lfFaceName;
    playlist.font_height=font.lfHeight;
}

void ApplyPlaylistProfile(const PlaylistSettings& source,
                          PlaylistSettings& target) {
    // Per-skin files carry only the list's visual tuple. Behaviour fields
    // belong to TTPlayer.xml and must survive switching packages.
    target.font=source.font;
    target.font_height=source.font_height;
    target.font_descriptor=source.font_descriptor;
    target.font_descriptor_valid=source.font_descriptor_valid;
    target.text_color=source.text_color;
    target.highlight_color=source.highlight_color;
    target.background_color=source.background_color;
    target.number_color=source.number_color;
    target.duration_color=source.duration_color;
    target.selected_color=source.selected_color;
    target.alternate_background_color=source.alternate_background_color;
    target.legacy_playlist_generation=source.legacy_playlist_generation;
}

void ApplyVisualAttributes(IXMLDOMNode* node, VisualSettings& visual,
                           bool include_global_fields) {
    if (!node) return;
    if (include_global_fields) {
        visual.type = std::clamp(IntAttr(node, L"Type", visual.type), 0, 4);
        visual.frames_per_second = std::clamp(
            IntAttr(node, L"FramesPerSec", visual.frames_per_second), 0, 100);
    }
    visual.spectrum_top_color = ColorAttr(
        node, L"SpectrumTopColor", visual.spectrum_top_color);
    visual.spectrum_bottom_color = ColorAttr(
        node, L"SpectrumBtmColor", visual.spectrum_bottom_color);
    visual.spectrum_middle_color = ColorAttr(
        node, L"SpectrumMidColor", visual.spectrum_middle_color);
    visual.spectrum_peak_color = ColorAttr(
        node, L"SpectrumPeakColor", visual.spectrum_peak_color);
    visual.spectrum_wide = std::max(
        0, IntAttr(node, L"SpectrumWide", visual.spectrum_wide));
    visual.blur_speed = std::clamp(
        IntAttr(node, L"BlurSpeed", visual.blur_speed), 0, 255);
    visual.blur = IntAttr(node, L"Blur", visual.blur ? 1 : 0) != 0;
    visual.blur_scope_color = ColorAttr(
        node, L"BlurScopeColor", visual.blur_scope_color);
    visual.text_color = ColorAttr(node, L"TextColor", visual.text_color);
    LOGFONTW font = visual.font;
    if (LogFontAttr(node, L"Font", font)) {
        visual.font = font;
        visual.font_valid = true;
    }
}

std::wstring RectangleText(const RECT& bounds) {
    wchar_t text[96]{};
    swprintf_s(text,L"%ld,%ld,%ld,%ld",bounds.left,bounds.top,
               bounds.right,bounds.bottom);
    return text;
}

std::wstring ColorText(COLORREF color) {
    wchar_t text[16]{};
    swprintf_s(text,L"#%02x%02x%02x",GetRValue(color),GetGValue(color),
               GetBValue(color));
    return text;
}

std::wstring LogFontText(const LOGFONTW& font) {
    std::wostringstream text;
    text << font.lfHeight << L',' << font.lfWidth << L','
         << font.lfEscapement << L',' << font.lfOrientation << L','
         << font.lfWeight << L',' << static_cast<unsigned int>(font.lfItalic)
         << L',' << static_cast<unsigned int>(font.lfUnderline) << L','
         << static_cast<unsigned int>(font.lfStrikeOut) << L','
         << static_cast<unsigned int>(font.lfCharSet) << L','
         << static_cast<unsigned int>(font.lfOutPrecision) << L','
         << static_cast<unsigned int>(font.lfClipPrecision) << L','
         << static_cast<unsigned int>(font.lfQuality) << L','
         << static_cast<unsigned int>(font.lfPitchAndFamily) << L','
         << font.lfFaceName;
    return text.str();
}

void SetAttribute(IXMLDOMElement* element,const wchar_t* name,
                  const std::wstring& value) {
    if(element) element->setAttribute(_bstr_t(name),_variant_t(value.c_str()));
}

void SetAttribute(IXMLDOMElement* element,const wchar_t* name,int value) {
    if(element) element->setAttribute(_bstr_t(name),_variant_t(value));
}

void SaveIndexedStringAttrs(IXMLDOMElement* element,const wchar_t* prefix,
                            const std::vector<std::wstring>& values,
                            size_t hard_limit) {
    const size_t count=std::min(values.size(),hard_limit);
    const auto count_name=IndexedAttributeName(prefix,L"_Count");
    SetAttribute(element,count_name.c_str(),static_cast<int>(count));
    for(size_t index=0;index<count;++index) {
        const auto name=std::wstring(prefix)+L"_"+std::to_wstring(index);
        SetAttribute(element,name.c_str(),values[index]);
    }
}

void SaveDesktopLyricProfile(IXMLDOMElement* element,const wchar_t* prefix,
                             const DesktopLyricColorProfile& profile,
                             bool include_name) {
    const std::wstring base(prefix);
    if(include_name)
        SetAttribute(element,(base+L"_Name").c_str(),profile.name);
    SetAttribute(element,(base+L"_BCount").c_str(),profile.background_count);
    SetAttribute(element,(base+L"_PCount").c_str(),profile.played_count);
    for(size_t index=0;index<profile.background_colors.size();++index) {
        const auto number=std::to_wstring(index+1);
        SetAttribute(element,(base+L"_BColor"+number).c_str(),
                     ColorText(profile.background_colors[index]));
        SetAttribute(element,(base+L"_PColor"+number).c_str(),
                     ColorText(profile.played_colors[index]));
    }
}

IXMLDOMElement* EnsureElement(IXMLDOMDocument* document,const wchar_t* name) {
    if(!document) return nullptr;
    const std::wstring xpath=std::wstring(L"/ttplayer/")+name;
    if(auto node=SelectOwned(document,xpath.c_str())) {
        ComPtr<IXMLDOMElement> element;
        node->QueryInterface(IID_PPV_ARGS(element.GetAddressOf()));
        if(element) return element.Detach();
    }
    auto root=SelectOwned(document,L"/ttplayer");
    if(!root) return nullptr;
    ComPtr<IXMLDOMElement> element;
    if(SUCCEEDED(document->createElement(
           _bstr_t(name),element.GetAddressOf())) && element) {
        ComPtr<IXMLDOMNode> appended;
        root->appendChild(element.Get(),appended.GetAddressOf());
    }
    return element.Detach();
}

bool OpenSettingsDocument(const std::filesystem::path& path,
                          IXMLDOMDocument** result) {
    if(!result || path.empty()) return false;
    *result=nullptr;
    ComPtr<IXMLDOMDocument> document;
    if(FAILED(CoCreateInstance(__uuidof(DOMDocument60),nullptr,
        CLSCTX_INPROC_SERVER,IID_PPV_ARGS(document.GetAddressOf()))) ||
       !document) return false;
    document->put_async(VARIANT_FALSE);
    document->put_preserveWhiteSpace(VARIANT_TRUE);
    VARIANT_BOOL loaded{};
    document->load(_variant_t(path.wstring().c_str()),&loaded);
    if(loaded!=VARIANT_TRUE) {
        ComPtr<IXMLDOMProcessingInstruction> declaration;
        document->createProcessingInstruction(_bstr_t(L"xml"),
            _bstr_t(L"version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\""),
            declaration.GetAddressOf());
        if(declaration) {
            ComPtr<IXMLDOMNode> appended;
            document->appendChild(declaration.Get(),appended.GetAddressOf());
        }
        ComPtr<IXMLDOMElement> root;
        if(FAILED(document->createElement(
               _bstr_t(L"ttplayer"),root.GetAddressOf())) ||
           !root) {
            return false;
        }
        SetAttribute(root.Get(),L"version",L"5.7.9");
        ComPtr<IXMLDOMNode> appended;
        document->appendChild(root.Get(),appended.GetAddressOf());
    }
    *result=document.Detach();
    return true;
}

bool LoadOptionsProfileNode(const std::filesystem::path& path,
                            const wchar_t* root_name,
                            const wchar_t* child_name,
                            IXMLDOMDocument** document_result,
                            IXMLDOMNode** node_result) {
    if(!document_result || !node_result || path.empty()) return false;
    *document_result=nullptr;
    *node_result=nullptr;
    ComPtr<IXMLDOMDocument> document;
    if(FAILED(CoCreateInstance(__uuidof(DOMDocument60),nullptr,
        CLSCTX_INPROC_SERVER,IID_PPV_ARGS(document.GetAddressOf()))) ||
       !document) return false;
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    document->load(_variant_t(path.wstring().c_str()),&loaded);
    if(loaded!=VARIANT_TRUE) return false;
    const std::wstring xpath=std::wstring(L"/")+root_name+L"/"+child_name;
    auto node=SelectOwned(document.Get(),xpath.c_str());
    if(!node) return false;
    *document_result=document.Detach();
    *node_result=node.Detach();
    return true;
}

bool CreateOptionsProfileDocument(const wchar_t* root_name,
                                  const wchar_t* child_name,
                                  IXMLDOMDocument** document_result,
                                  IXMLDOMElement** child_result) {
    if(!document_result || !child_result) return false;
    *document_result=nullptr;
    *child_result=nullptr;
    ComPtr<IXMLDOMDocument> document;
    if(FAILED(CoCreateInstance(__uuidof(DOMDocument60),nullptr,
        CLSCTX_INPROC_SERVER,IID_PPV_ARGS(document.GetAddressOf()))) ||
       !document) return false;
    document->put_async(VARIANT_FALSE);
    document->put_preserveWhiteSpace(VARIANT_TRUE);

    ComPtr<IXMLDOMProcessingInstruction> declaration;
    document->createProcessingInstruction(_bstr_t(L"xml"),
        _bstr_t(L"version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\""),
        declaration.GetAddressOf());
    if(declaration) {
        ComPtr<IXMLDOMNode> appended;
        document->appendChild(declaration.Get(),appended.GetAddressOf());
    }

    ComPtr<IXMLDOMElement> root;
    if(FAILED(document->createElement(
           _bstr_t(root_name),root.GetAddressOf())) || !root) return false;
    ComPtr<IXMLDOMNode> appended_root;
    document->appendChild(root.Get(),appended_root.GetAddressOf());

    ComPtr<IXMLDOMElement> child;
    if(FAILED(document->createElement(
           _bstr_t(child_name),child.GetAddressOf())) || !child) return false;
    ComPtr<IXMLDOMNode> appended_child;
    root->appendChild(child.Get(),appended_child.GetAddressOf());
    *document_result=document.Detach();
    *child_result=child.Detach();
    return true;
}

bool MergeVisualDocument(const std::filesystem::path& path,
                         VisualSettings& visual,
                         bool include_global_fields) {
    ComInit com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) return false;
    ComPtr<IXMLDOMDocument> document;
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(document.GetAddressOf()))) || !document)
        return false;
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    document->load(_variant_t(path.wstring().c_str()), &loaded);
    if (loaded != VARIANT_TRUE) return false;
    if (auto node=SelectOwned(document.Get(),L"/ttplayer/Visual"))
        ApplyVisualAttributes(node.Get(),visual,include_global_fields);
    return true;
}
}

PlayerSettings::PlayerSettings() {
    // FUN_00401E96 creates a 620x66 desktop-lyric strip centered above the
    // bottom of the full-screen work area (CSettings +0xA8).
    const int width=GetSystemMetrics(SM_CXFULLSCREEN);
    const int height=GetSystemMetrics(SM_CYFULLSCREEN);
    SetRect(&desktop_lyric_window,0,0,width,height);
    desktop_lyric_window.bottom-=50;
    desktop_lyric_window.top=desktop_lyric_window.bottom-66;
    InflateRect(&desktop_lyric_window,(620-width)/2,0);
}

HotKeySettings::HotKeySettings()
    : key_map(std::begin(kDefaultHotKeyMap),std::end(kDefaultHotKeyMap)) {}

DesktopLyricSettings::DesktopLyricSettings() {
    // FUN_00401E96 (+0x47C) constructs this LOGFONT independently of the
    // normal lyric window.  The literal face name is part of the original
    // settings default; profile names themselves come from ttpres.dll.
    font.lfHeight=-40;
    font.lfWeight=FW_BOLD;
    font.lfCharSet=DEFAULT_CHARSET;
    font.lfQuality=ANTIALIASED_QUALITY;
    wcsncpy_s(font.lfFaceName,L"幼圆",_TRUNCATE);
    font_valid=true;

    profiles[0].background_count=3;
    profiles[0].background_colors={
        RGB(0,128,255),RGB(0,255,255),RGB(0,128,255)};
    profiles[0].played_count=3;
    profiles[0].played_colors={
        RGB(255,128,128),RGB(255,0,0),RGB(255,128,128)};

    profiles[1].background_count=2;
    profiles[1].background_colors={
        RGB(37,152,10),RGB(129,249,0),RGB(0,0,0)};
    profiles[1].played_count=3;
    profiles[1].played_colors={
        RGB(253,232,0),RGB(255,120,0),RGB(255,246,0)};

    profiles[2].background_count=2;
    profiles[2].background_colors={
        RGB(94,16,223),RGB(216,147,255),RGB(0,0,0)};
    profiles[2].played_count=3;
    profiles[2].played_colors={
        RGB(255,206,223),RGB(255,60,133),RGB(255,206,223)};

    current=profiles[0];
    current.name.clear();
}

NetworkSettings::NetworkSettings() {
    std::array<wchar_t,MAX_PATH> path{};
    if(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,
                                  SHGFP_TYPE_CURRENT,path.data())))
        cache_folder=path.data();

    // FUN_00401CC3 prefers the second fixed logical drive, otherwise the
    // drive containing Program Files (and finally Windows), then appends the
    // original TTPmusic directory.
    std::filesystem::path base;
    std::array<wchar_t,MAX_PATH> system_path{};
    if(SHGetSpecialFolderPathW(nullptr,system_path.data(),
                               CSIDL_PROGRAM_FILESX86,FALSE)) {
        base=std::filesystem::path(system_path.data()).root_path();
    } else if(GetWindowsDirectoryW(
                  system_path.data(),static_cast<UINT>(system_path.size()))>0) {
        base=std::filesystem::path(system_path.data()).root_path();
    } else {
        base=L"C:\\";
    }
    std::array<wchar_t,512> drives{};
    if(GetLogicalDriveStringsW(static_cast<DWORD>(drives.size()),
                               drives.data())>0) {
        const wchar_t* drive=drives.data();
        if(*drive) drive+=wcslen(drive)+1;
        if(*drive && GetDriveTypeW(drive)==DRIVE_FIXED) base=drive;
    }
    download_folder=(base/L"TTPmusic")/L"";
}

DspPluginSettings::DspPluginSettings() {
    // FUN_00401E96 first honours Winamp's registered install directory, then
    // probes the conventional Program Files location, and only finally falls
    // back to TTPlayer's own Plugins directory.
    std::array<wchar_t,MAX_PATH+1> registered{};
    DWORD registered_type{};
    DWORD registered_bytes=static_cast<DWORD>(registered.size()*sizeof(wchar_t));
    if(SHGetValueW(HKEY_CURRENT_USER,L"Software\\Winamp",L"",
                   &registered_type,registered.data(),&registered_bytes)==
           ERROR_SUCCESS &&
       (registered_type==REG_SZ || registered_type==REG_EXPAND_SZ) &&
       registered.front()!=L'\0') {
        registered.back()=L'\0';
        folder=std::filesystem::path(registered.data())/L"Plugins";
        return;
    }

    std::array<wchar_t,MAX_PATH> program_files{};
    if(!SHGetSpecialFolderPathW(nullptr,program_files.data(),
                                CSIDL_PROGRAM_FILESX86,FALSE))
        wcscpy_s(program_files.data(),program_files.size(),
                 L"C:\\Program Files");
    const auto conventional=
        std::filesystem::path(program_files.data())/L"Winamp"/L"Plugins";
    std::error_code directory_error;
    if(std::filesystem::is_directory(conventional,directory_error)) {
        folder=conventional;
        return;
    }

    std::array<wchar_t,32768> executable{};
    const DWORD length=GetModuleFileNameW(
        nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if(length>0 && length<static_cast<DWORD>(executable.size()))
        folder=(std::filesystem::path(executable.data()).parent_path()/
                L"Plugins")/L"";
}

StartupPlaybackPlan MakeStartupPlaybackPlan(
    const Settings& settings) noexcept {
    StartupPlaybackPlan plan;
    plan.should_play = settings.playback.auto_play &&
                       !settings.player.playing_file_name.empty();
    if (plan.should_play && settings.playback.continue_play)
        plan.resume_position_ms = std::max(0, settings.player.playing_time);
    return plan;
}

void ClearPlaybackIdentity(PlayerSettings& player) noexcept {
    player.playing_time = 0;
    player.playing_file_name.clear();
    player.playing_file_subtrack = 0;
}

LyricSettings::LyricSettings() {
    // CSettings' lyric-default branch copies the current icon-title LOGFONT
    // and then replaces only lfHeight with 60 for the detached full-screen
    // LyricCtrl (TTPlayer.exe.pseudo.c 004018xx, settings offset +0x364).
    if (!SystemParametersInfoW(SPI_GETICONTITLELOGFONT,
                               sizeof(fullscreen_font),
                               &fullscreen_font, 0)) {
        const HFONT stock = static_cast<HFONT>(
            GetStockObject(DEFAULT_GUI_FONT));
        if (stock)
            GetObjectW(stock, sizeof(fullscreen_font), &fullscreen_font);
    }
    fullscreen_font.lfQuality = ANTIALIASED_QUALITY;
    fullscreen_font.lfHeight = 60;
    fullscreen_font_valid = true;
}

Settings LoadLegacyXml(const std::filesystem::path& path) {
    ComInit com; if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) throw std::runtime_error("COM init failed");
    ComPtr<IXMLDOMDocument> document;
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr,
            CLSCTX_INPROC_SERVER,IID_PPV_ARGS(document.GetAddressOf()))))
        throw std::runtime_error("MSXML6 unavailable");
    auto* const doc=document.Get();
    VARIANT_BOOL loaded{}; doc->put_async(VARIANT_FALSE); doc->load(_variant_t(path.wstring().c_str()), &loaded);
    if (loaded != VARIANT_TRUE) throw std::runtime_error("invalid settings XML");
    Settings s; s.source_path=path;
    if (auto node=SelectOwned(doc,L"/ttplayer/Player")) {
        auto* const n=node.Get();
        s.player.volume=IntAttr(n,L"Volume",100);
        s.player.balance=IntAttr(n,L"Balance",0);
        s.player.play_mode=IntAttr(n,L"PlayMode",2);
        s.player.mute=IntAttr(n,L"Mute",0)!=0;
        s.player.top_most=IntAttr(n,L"TopMost",0)!=0;
        s.player.mini_top_most=IntAttr(n,L"TopMost2",1)!=0;
        s.player.player_window=RectAttr(n,L"PlayerWnd");
        s.player.mini_player_window=RectAttr(n,L"PlayerWnd2");
        s.player.mini_mode=IntAttr(n,L"MiniMode",0)!=0;
        s.player.lyric_window=RectAttr(n,L"LyricWnd");
        s.player.lyric_visible=IntAttr(n,L"LyricVisible",0)!=0;
        s.player.lyric_top_most=IntAttr(n,L"LyricTopMost",0)!=0;
        s.player.mini_lyric_window=RectAttr(n,L"LyricWnd2");
        s.player.mini_lyric_visible=IntAttr(n,L"LyricVisible2",0)!=0;
        s.player.mini_lyric_top_most=IntAttr(n,L"LyricTopMost2",0)!=0;
        s.player.playlist_window=RectAttr(n,L"PlayListWnd");
        s.player.playlist_visible=IntAttr(n,L"PlayListVisible",0)!=0;
        s.player.equalizer_window=RectAttr(n,L"EqualizerWnd");
        s.player.equalizer_visible=IntAttr(n,L"EqualizerVisible",0)!=0;
        s.player.playlist_scan_count=IntAttr(n,L"PlayLists",100);
        s.player.active_playlist=IntAttr(n,L"ActiveList",0);
        s.player.playing_time=IntAttr(n,L"PlayingTime",0);
        s.player.playing_file_name=StringAttr(n,L"PlayingFileName");
        s.player.playing_file_subtrack=IntAttr(n,L"PlayingFileSubTrack",0);
        s.player.auto_switch_list=IntAttr(n,L"AutoSwitchList",0)!=0;
        s.player.play_follow_cursor=IntAttr(n,L"PlayFollowCursor",0)!=0;
        s.player.opaque_when_active=IntAttr(n,L"OpaqueWhenActive",0)!=0;
        s.player.alpha_percent=std::clamp(IntAttr(n,L"AlphaPercent",0),0,90);
        s.player.desktop_lyric_window=RectAttr(n,L"DesklrcWnd");
        s.player.window_shadow=IntAttr(
            n,L"WindowShadow",s.player.window_shadow ? 1 : 0)!=0;
        s.player.show_elapsed_time=IntAttr(
            n,L"ShowElapsedTime",s.player.show_elapsed_time ? 1 : 0)!=0;
        s.player.check_association=IntAttr(
            n,L"CheckAssociation",s.player.check_association ? 1 : 0)!=0;
        s.player.auto_associate=IntAttr(
            n,L"AutoAssociate",s.player.auto_associate ? 1 : 0)!=0;
        s.player.first_run_552=IntAttr(
            n,L"FirstRun_552",s.player.first_run_552 ? 1 : 0)!=0;
        s.player.user_word=StringAttr(n,L"UserWord");
        s.player.user_word_md5=StringAttr(n,L"UserWordMD5");
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/General")) {
        auto* const n=node.Get();
        s.general.startup_minimize=IntAttr(
            n,L"StartupMinimize",s.general.startup_minimize ? 1 : 0)!=0;
        s.general.tray_icon=IntAttr(n,L"TrayIcon",s.general.tray_icon ? 1 : 0)!=0;
        s.general.fade_windows=IntAttr(
            n,L"Fade_Windows",s.general.fade_windows ? 1 : 0)!=0;
        s.general.show_hotkey_in_tips=IntAttr(
            n,L"ShowHotKeyInTips",s.general.show_hotkey_in_tips ? 1 : 0)!=0;
        s.general.tips_on_open=IntAttr(
            n,L"TipsOnOpen",s.general.tips_on_open ? 1 : 0)!=0;
        s.general.menu_tips=IntAttr(
            n,L"MenuTips",s.general.menu_tips ? 1 : 0)!=0;
        s.general.menu_bar_playlist=IntAttr(
            n,L"MenuBarPlayList",s.general.menu_bar_playlist ? 1 : 0)!=0;
        s.general.scroll_title=IntAttr(
            n,L"ScrollTitle",s.general.scroll_title ? 1 : 0)!=0;
        const auto discord_switch=Attribute(n,L"SendTitleToDiscord");
        s.general.discord_sync_lyrics=IntAttr(
            n,L"DiscordSyncLyrics",s.general.discord_sync_lyrics ? 1 : 0)!=0;
        s.general.send_title_to_msn=discord_switch.vt==VT_EMPTY
            ? IntAttr(n,L"SendTitleToMSN",
                      s.general.send_title_to_msn ? 1 : 0)!=0
            : static_cast<int>(discord_switch)!=0;
        // Absence means an older TTPlayer.xml and keeps the rebuilt player's
        // registered default. A non-empty XML value is the only supported
        // override; a missing or empty attribute retains the built-in ID.
        if (auto configured=StringAttr(n,L"DiscordApplicationId");
            !configured.empty())
            s.general.discord_application_id=std::move(configured);
        s.general.snap_windows=IntAttr(
            n,L"Snap_Windows",s.general.snap_windows);
        s.general.title_slide_interval=IntAttr(
            n,L"TitleSlideInterval",s.general.title_slide_interval);
        s.general.check_update_days=IntAttr(
            n,L"CheckUpdateDays",s.general.check_update_days);
        s.general.last_message_version=IntAttr(
            n,L"LastMessageVer",s.general.last_message_version);
        s.general.last_checked_day=IntAttr(
            n,L"LastCheckedDay",s.general.last_checked_day);
        s.general.auto_shutdown=IntAttr(
            n,L"AutoShutDown",s.general.auto_shutdown ? 1 : 0)!=0;
        s.general.shutdown_time=FixedIntegerListAttr(
            n,L"ShutDownTime",L':',s.general.shutdown_time);
        s.general.clear_list_on_command=IntAttr(
            n,L"ClearListOnCmd",s.general.clear_list_on_command ? 1 : 0)!=0;
        s.general.default_list_on_command=IntAttr(
            n,L"DefaultListOnCmd",s.general.default_list_on_command ? 1 : 0)!=0;
        s.general.default_list=StringAttr(n,L"strDefaultList");
        s.general.app_icon_file=StringAttr(n,L"AppIconFile");
        s.general.mp3_read_tag_priority=IntAttr(
            n,L"MP3ReadTagPriority",s.general.mp3_read_tag_priority);
        s.general.mp3_write_tag_type=IntAttr(
            n,L"MP3WriteTagType",s.general.mp3_write_tag_type);
        s.general.mp3_id3v2_encoding=IntAttr(
            n,L"MP3ID3v2Encoding",s.general.mp3_id3v2_encoding);
        s.general.mp3_id3v2_padding=IntAttr(
            n,L"MP3ID3v2Padding",s.general.mp3_id3v2_padding ? 1 : 0)!=0;
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Playback")) {
        auto* const n=node.Get();
        s.playback.auto_play=IntAttr(n,L"AutoPlay",1)!=0;
        s.playback.continue_play=IntAttr(n,L"ContinuePlay",0)!=0;
        s.playback.stop_when_fail=IntAttr(
            n,L"StopWhenFail",s.playback.stop_when_fail ? 1 : 0)!=0;
        s.playback.file_buffer=IntAttr(n,L"FileBuffer",16384);
        s.playback.track_interval=IntAttr(n,L"TracksInterval",0);
        s.playback.thread_priority=IntAttr(n,L"ThreadPriority",15);
        s.playback.sound_fade_mode=IntAttr(
            n,L"SoundFadeMode",s.playback.sound_fade_mode);
        s.playback.fade_duration=FixedIntegerListAttr(
            n,L"FadeDuration",L',',s.playback.fade_duration);
        s.playback.track_fade_duration=IntAttr(
            n,L"TrackFadeDur",s.playback.track_fade_duration);
        s.playback.auto_gain=IntAttr(n,L"AutoGain",0)!=0;
        s.playback.auto_scan_gain=IntAttr(n,L"AutoScanGain",0)!=0;
        s.playback.skip_scan_gain=IntAttr(n,L"SkipScanGain",0)!=0;
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Device")) {
        auto* const n=node.Get();
        const auto device_type=StringAttr(n,L"DeviceType");
        if(!device_type.empty()) s.device.device_type=device_type;
        s.device.output_bits=IntAttr(n,L"OutputBits",s.device.output_bits);
        s.device.buffer_duration=IntAttr(
            n,L"BufferDuration",s.device.buffer_duration);
        s.device.hardware_buffer=IntAttr(
            n,L"HardwareBuffer",s.device.hardware_buffer ? 1 : 0)!=0;
        s.device.create_primary=IntAttr(
            n,L"CreatePrimary",s.device.create_primary ? 1 : 0)!=0;
        s.device.resample_rate=IntAttr(
            n,L"ResampleRate",s.device.resample_rate);
        s.device.ssrc_mode=IntAttr(n,L"SsrcMode",s.device.ssrc_mode);
        s.device.dither=IntAttr(n,L"Dither",s.device.dither);
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/HotKey")) {
        auto* const n=node.Get();
        s.hotkey.global=IntAttr(
            n,L"Global",s.hotkey.global ? 1 : 0)!=0;
        if(auto values=IndexedStringAttrs(
               n,L"KeyMap",kMaxHotKeyBindings)) {
            s.hotkey.key_map.clear();
            s.hotkey.key_map.reserve(values->size());
            for(auto& value:*values)
                s.hotkey.key_map.push_back(
                    ParseHotKeyBinding(std::move(value)));
        }
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Visual")) {
        auto* const n=node.Get();
        ApplyVisualAttributes(n, s.visual, true);
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/FullScreen")) {
        auto* const n=node.Get();
        s.fullscreen.visual_type=std::clamp(
            IntAttr(n,L"VisualType",s.fullscreen.visual_type),0,3);
        constexpr std::array<const wchar_t*,4> relation_names{
            L"PosRelationAll",L"PosRelationGoom",
            L"PosRelationSpectrum",L"PosRelationBlurScope"};
        constexpr std::array<const wchar_t*,4> size_names{
            L"LrcSizeAll",L"LrcSizeGoom",
            L"LrcSizeSpectrum",L"LrcSizeBlurScope"};
        for(size_t index=0;index<relation_names.size();++index) {
            // CSettings preserves the raw profile integers in TTPlayer.xml.
            // Full-screen layout normalizes them only when it consumes the
            // selected profile (FUN_0046228D); persistence must round-trip
            // values written by older/newer builds without rewriting them.
            s.fullscreen.position_relation[index]=IntAttr(
                n,relation_names[index],
                s.fullscreen.position_relation[index]);
            s.fullscreen.lyric_size[index]=IntAttr(
                n,size_names[index],s.fullscreen.lyric_size[index]);
        }
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/DeskLrc")) {
        auto* const n=node.Get();
        auto& value=s.desktop_lyric;
        value.profile=IntAttr(n,L"Profile",value.profile);
        value.lines=IntAttr(n,L"Lines",value.lines);
        value.align=IntAttr(n,L"Align",value.align);
        value.background_alpha=IntAttr(
            n,L"BkgndAlpha",value.background_alpha);
        value.text_alpha=IntAttr(n,L"TextAlpha",value.text_alpha);
        value.topmost=IntAttr(n,L"Topmost",value.topmost ? 1 : 0)!=0;
        value.karaoke_mode=IntAttr(
            n,L"KaraokeMode",value.karaoke_mode ? 1 : 0)!=0;
        value.auto_width=IntAttr(
            n,L"AutoWidth",value.auto_width ? 1 : 0)!=0;
        value.unlock_when_close=IntAttr(
            n,L"UnlockWhenClose",value.unlock_when_close ? 1 : 0)!=0;
        // CSettings deliberately ignores Lock while UnlockWhenClose is set.
        if(!value.unlock_when_close)
            value.lock=IntAttr(n,L"Lock",value.lock ? 1 : 0)!=0;
        value.background_transparent=IntAttr(
            n,L"BkgTransp",value.background_transparent ? 1 : 0)!=0;
        value.smooth=IntAttr(n,L"Smooth",value.smooth ? 1 : 0)!=0;
        value.border=IntAttr(n,L"Border",value.border ? 1 : 0)!=0;
        value.shadow=IntAttr(n,L"Shadow",value.shadow ? 1 : 0)!=0;
        value.background_show=IntAttr(
            n,L"BkgndShow",value.background_show ? 1 : 0)!=0;
        LOGFONTW font=value.font;
        if(LogFontAttr(n,L"Font",font)) {
            value.font=font;
            value.font_valid=true;
        }
        value.border_color=ColorAttr(n,L"BorderColor",value.border_color);
        value.shadow_color=ColorAttr(n,L"ShadowColor",value.shadow_color);
        value.background_color=ColorAttr(
            n,L"BkgndColor",value.background_color);
        LoadDesktopLyricProfile(n,L"Profile",value.current,false);
        for(size_t index=0;index<value.profiles.size();++index) {
            const auto prefix=L"Profile"+std::to_wstring(index+1);
            LoadDesktopLyricProfile(
                n,prefix.c_str(),value.profiles[index],true);
        }
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Equalizer")) {
        auto* const n=node.Get();
        s.equalizer.profile=IntAttr(n,L"Profile",-2);
        s.equalizer.profile_last=IntAttr(n,L"ProfileLast",-1);
        s.equalizer.surround=IntAttr(n,L"Surround",0);
        s.equalizer.custom=EqualizerAttr(n,L"Custom");
        s.equalizer.current=EqualizerAttr(n,L"Current");
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/PlayList")) {
        auto* const n=node.Get();
        LOGFONTW font=PlaylistLogFont(s.playlist);
        if(LogFontAttr(n,L"Font",font)) SetPlaylistLogFont(s.playlist,font);
        s.playlist.text_color=ColorAttr(n,L"Color_Text",s.playlist.text_color);
        s.playlist.highlight_color=ColorAttr(n,L"Color_Hilight",s.playlist.highlight_color);
        s.playlist.background_color=ColorAttr(n,L"Color_Bkgnd",s.playlist.background_color);
        s.playlist.number_color=ColorAttr(n,L"Color_Number",s.playlist.number_color);
        s.playlist.duration_color=ColorAttr(n,L"Color_Duration",s.playlist.duration_color);
        s.playlist.selected_color=ColorAttr(n,L"Color_Select",s.playlist.selected_color);
        s.playlist.alternate_background_color=ColorAttr(n,L"Color_Bkgnd2",s.playlist.alternate_background_color);
        s.playlist.legacy_playlist_generation=IntAttr(n,L"CreateNewVerPlayList",0)!=0;
        s.playlist.library_mode=IntAttr(
            n,L"LibraryMode",s.playlist.library_mode ? 1 : 0)!=0;
        s.playlist.item_tips=IntAttr(
            n,L"ItemTips",s.playlist.item_tips ? 1 : 0)!=0;
        s.playlist.disable_delete_file=IntAttr(
            n,L"DisableDelFile",s.playlist.disable_delete_file ? 1 : 0)!=0;
        s.playlist.enable_drag_drop=IntAttr(
            n,L"EnableDragDrop",s.playlist.enable_drag_drop ? 1 : 0)!=0;
        s.playlist.read_info_mode=IntAttr(
            n,L"ReadInfoMode",s.playlist.read_info_mode);
        s.playlist.title_number=IntAttr(
            n,L"TitleNumber",s.playlist.title_number ? 1 : 0)!=0;
        s.playlist.ignore_bad_files=IntAttr(n,L"IgnoreBadFiles",0)!=0;
        s.playlist.save_relative_path=IntAttr(
            n,L"SaveRelativePath",s.playlist.save_relative_path ? 1 : 0)!=0;
        s.playlist.save_tags=IntAttr(
            n,L"SaveTags",s.playlist.save_tags ? 1 : 0)!=0;
        s.playlist.tag_format=IntAttr(n,L"TagFormat",s.playlist.tag_format);
        s.playlist.click_rating=IntAttr(
            n,L"ClickRating",s.playlist.click_rating ? 1 : 0)!=0;
        s.playlist.tag_title_format=StringAttr(n,L"TagTitleFormat");
        s.playlist.default_title_format=StringAttr(n,L"DefTitleFormat");
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Library")) {
        auto* const n=node.Get();
        auto& value=s.library;
        value.enabled=IntAttr(n,L"Enabled",value.enabled ? 1 : 0)!=0;
        value.valid=IntAttr(n,L"Valid",value.valid ? 1 : 0)!=0;
        value.playing_catalog=StringAttr(n,L"PlayingCatalog");
        value.monitor_directories=IntAttr(
            n,L"MonitorDir",value.monitor_directories ? 1 : 0)!=0;
        if(auto values=IndexedStringAttrs(
               n,L"Directories",kMaxLibraryDirectories)) {
            value.directories.clear();
            value.directories.reserve(values->size());
            for(auto& directory:*values)
                value.directories.push_back(
                    ParseMediaLibraryDirectory(std::move(directory)));
        }
        value.max_item_count=IntAttr(
            n,L"MaxItemCount",value.max_item_count);
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Lyric")) {
        auto* const n=node.Get();
        s.lyric.font_valid=LogFontAttr(n,L"Font",s.lyric.font);
        s.lyric.text_color=ColorAttr(n,L"TextColor",s.lyric.text_color);
        s.lyric.highlight_color=ColorAttr(n,L"HilightColor",s.lyric.highlight_color);
        s.lyric.background_color=ColorAttr(n,L"BkgndColor",s.lyric.background_color);
        s.lyric.charset=IntAttr(n,L"CharSet",0);
        s.lyric.scroll_mode=IntAttr(n,L"ScrollMode",0);
        s.lyric.mini_scroll_mode=IntAttr(n,L"ScrollMode2",1);
        s.lyric.fullscreen_scroll_mode=IntAttr(n,L"ScrollModeFS",0);
        s.lyric.fullscreen_text_align=IntAttr(n,L"TextAlignFS",1);
        s.lyric.fullscreen_row_interval=IntAttr(n,L"RowIntervalFS",4);
        // The original settings object retains FadeIndexFS verbatim. The
        // lyric renderer decides whether the value enables edge fading.
        s.lyric.fullscreen_fade_index=IntAttr(n,L"FadeIndexFS",10);
        s.lyric.fullscreen_fade_highlight=
            IntAttr(n,L"FadeHilightFS",1)!=0;
        s.lyric.fullscreen_karaoke_mode=
            IntAttr(n,L"KaraokeModeFS",0)!=0;
        s.lyric.fullscreen_transparent=
            IntAttr(n,L"TransparentFS",0)!=0;
        s.lyric.fullscreen_auto_font=IntAttr(n,L"AutoFontFS",1)!=0;
        LOGFONTW fullscreen_font=s.lyric.fullscreen_font;
        if(LogFontAttr(n,L"FontFS",fullscreen_font)) {
            s.lyric.fullscreen_font=fullscreen_font;
            s.lyric.fullscreen_font_valid=true;
        }
        s.lyric.fullscreen_text_color=ColorAttr(
            n,L"TextColorFS",s.lyric.fullscreen_text_color);
        s.lyric.fullscreen_highlight_color=ColorAttr(
            n,L"HilightColorFS",s.lyric.fullscreen_highlight_color);
        s.lyric.fullscreen_background_color=ColorAttr(
            n,L"BkgndColorFS",s.lyric.fullscreen_background_color);
        s.lyric.text_align=IntAttr(n,L"TextAlign",1);
        s.lyric.row_interval=IntAttr(n,L"RowInterval",4);
        s.lyric.fade_index=std::max(0,IntAttr(n,L"FadeIndex",10));
        s.lyric.fade_highlight=IntAttr(n,L"FadeHilight",1)!=0;
        s.lyric.karaoke_mode=IntAttr(n,L"KaraokeMode",0)!=0;
        s.lyric.transparent=IntAttr(n,L"Transparent",0)!=0;
        s.lyric.transparent_skin=IntAttr(n,L"TransSkin",1)!=0;
        s.lyric.auto_load_lyric=IntAttr(
            n,L"AutoLoadLyric",s.lyric.auto_load_lyric ? 1 : 0)!=0;
        s.lyric.auto_save_lyric_tag=IntAttr(
            n,L"AutoSaveLyricTag",s.lyric.auto_save_lyric_tag ? 1 : 0)!=0;
        s.lyric.dont_load_lyric_tag=IntAttr(
            n,L"DontLoadLyricTag",s.lyric.dont_load_lyric_tag ? 1 : 0)!=0;
        s.lyric.auto_visible=IntAttr(
            n,L"AutoVisible",s.lyric.auto_visible ? 1 : 0)!=0;
        s.lyric.auto_width=IntAttr(
            n,L"AutoWidth",s.lyric.auto_width ? 1 : 0)!=0;
        s.lyric.auto_width_only_vertical=IntAttr(
            n,L"AutoWidthOnlyVert",s.lyric.auto_width_only_vertical ? 1 : 0)!=0;
        s.lyric.drag_lyric=IntAttr(n,L"DragLyric",1)!=0;
        s.lyric.mouse_wheel_adjust=IntAttr(n,L"MouseWheelAdjust",0)!=0;
        s.lyric.save_compress=IntAttr(
            n,L"SaveCompress",s.lyric.save_compress ? 1 : 0)!=0;
        s.lyric.trim_spaces=IntAttr(
            n,L"TrimSpaces",s.lyric.trim_spaces ? 1 : 0)!=0;
        s.lyric.lyric_save_mode=IntAttr(
            n,L"LyricSaveMode",s.lyric.lyric_save_mode);
        s.lyric.add_in_index=IntAttr(n,L"AddInIndex",s.lyric.add_in_index);
        s.lyric.auto_download=IntAttr(
            n,L"AutoDownLoad",s.lyric.auto_download ? 1 : 0)!=0;
        s.lyric.download_when_full_info=IntAttr(
            n,L"DownLoadWhenFullInfo",s.lyric.download_when_full_info ? 1 : 0)!=0;
        s.lyric.auto_associate=IntAttr(
            n,L"AutoAssociate",s.lyric.auto_associate ? 1 : 0)!=0;
        s.lyric.auto_select_download=IntAttr(
            n,L"AutoSelectDownload",s.lyric.auto_select_download ? 1 : 0)!=0;
        s.lyric.overwrite=IntAttr(
            n,L"OverWrite",s.lyric.overwrite ? 1 : 0)!=0;
        s.lyric.same_file_title=IntAttr(
            n,L"SameFileTitle",s.lyric.same_file_title ? 1 : 0)!=0;
        s.lyric.save_to_sound_folder=IntAttr(
            n,L"SaveToSoundFolder",s.lyric.save_to_sound_folder ? 1 : 0)!=0;
        s.lyric.download_folder=StringAttr(n,L"DownLoadFolder");
        s.lyric.new_line_after_tag=IntAttr(n,L"NewLineAfterTag",1)!=0;
        s.lyric.display_mode=IntAttr(n,L"DisplayMode",s.lyric.display_mode);
        if(auto values=IndexedStringAttrs(
               n,L"Folders",kMaxLyricFolders))
            s.lyric.folders=std::move(*values);
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Network")) {
        auto* const n=node.Get();
        auto& value=s.network;
        value.proxy_type=IntAttr(n,L"Proxy_Type",value.proxy_type);
        value.proxy_server=StringAttr(n,L"Proxy_Server");
        value.proxy_port=IntAttr(n,L"Proxy_Port",value.proxy_port);
        value.proxy_username=StringAttr(n,L"Proxy_UserName");
        value.proxy_password=StringAttr(n,L"Proxy_Password");
        value.freedb_auto_query=IntAttr(
            n,L"FreedbAutoQuery",value.freedb_auto_query ? 1 : 0)!=0;
        value.show_info_when_fail=IntAttr(
            n,L"ShowInfoWhenFail",value.show_info_when_fail ? 1 : 0)!=0;
        if(const auto attribute=Attribute(n,L"FreedbServer");
           attribute.vt!=VT_EMPTY)
            value.freedb_server=static_cast<const wchar_t*>(_bstr_t(attribute));
        if(const auto attribute=Attribute(n,L"CacheFolder");
           attribute.vt!=VT_EMPTY)
            value.cache_folder=static_cast<const wchar_t*>(_bstr_t(attribute));
        if(auto values=IndexedStringAttrs(
               n,L"ServerList",kMaxNetworkServers))
            value.server_list=std::move(*values);
        value.accept_recommendation_list=IntAttr(
            n,L"AcceptRecomList",
            value.accept_recommendation_list ? 1 : 0)!=0;
        value.last_recommendation_list_id=IntAttr(
            n,L"LastRecomListID",value.last_recommendation_list_id);
        value.check_update_days=IntAttr(
            n,L"CheckUpdateDays",value.check_update_days);
        value.last_checked_day=IntAttr(
            n,L"LastCheckedDay",value.last_checked_day);
        value.cache_enabled=IntAttr(
            n,L"DoCache",value.cache_enabled ? 1 : 0)!=0;
        value.cache_space_size=IntAttr(
            n,L"CacheSpaceSize",value.cache_space_size);
        value.last_message_time=IntAttr(
            n,L"LastMessageTime",value.last_message_time);
        if(const auto attribute=Attribute(n,L"DownloadFolder");
           attribute.vt!=VT_EMPTY)
            value.download_folder=static_cast<const wchar_t*>(_bstr_t(attribute));
        value.create_folder_by_artist=IntAttr(
            n,L"CreateFolderByAritst",
            value.create_folder_by_artist ? 1 : 0)!=0;
        value.replace_file=IntAttr(
            n,L"ReplaceFile",value.replace_file ? 1 : 0)!=0;
        value.download_when_listen=IntAttr(
            n,L"DownloadWhenListen",value.download_when_listen ? 1 : 0)!=0;
        value.download_lyric=IntAttr(
            n,L"DownloadLrc",value.download_lyric ? 1 : 0)!=0;
        value.max_download_tasks=IntAttr(
            n,L"MaxDownTasks",value.max_download_tasks);
        value.download_num=IntAttr(n,L"DownloadNum",value.download_num);
        value.speed_mode=IntAttr(n,L"SpeedModle",value.speed_mode);
        value.tip_showed_time=IntAttr(
            n,L"TipShowedTime",value.tip_showed_time);
        value.remember_download_mode=IntAttr(
            n,L"RemerberDownloadMode",
            value.remember_download_mode ? 1 : 0)!=0;
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Convert")) {
        auto* const n=node.Get();
        auto& value=s.convert;
        value.writer_index=IntAttr(n,L"WriterIndex",value.writer_index);
        value.lame_mode=std::clamp(IntAttr(n,L"LameMode",value.lame_mode),0,2);
        value.lame_bitrate=std::clamp(IntAttr(n,L"LameBitrate",value.lame_bitrate),8,320);
        value.lame_quality=std::clamp(IntAttr(n,L"LameQuality",value.lame_quality),0,9);
        value.output_bits=IntAttr(n,L"OutputBits",value.output_bits);
        value.resample_rate=IntAttr(n,L"ResampleRate",value.resample_rate);
        value.replay_gain=IntAttr(n,L"ReplayGain",value.replay_gain);
        value.equalizer=IntAttr(n,L"Equalizer",value.equalizer);
        value.surround=IntAttr(n,L"Surround",value.surround);
        if(const auto attribute=Attribute(n,L"Folder");
           attribute.vt!=VT_EMPTY)
            value.folder=static_cast<const wchar_t*>(_bstr_t(attribute));
        value.save_mode=IntAttr(n,L"SaveMode",value.save_mode);
        value.add_number=IntAttr(n,L"AddNumber",value.add_number);
        value.add_to_playlist=IntAttr(
            n,L"AddToPlayList",value.add_to_playlist);
        value.thread_priority=IntAttr(
            n,L"ThreadPriority",value.thread_priority);
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Plugin")) {
        auto* const n=node.Get();
        auto& value=s.plugin;
        if(const auto attribute=Attribute(n,L"Folder");
           attribute.vt!=VT_EMPTY)
            value.folder=static_cast<const wchar_t*>(_bstr_t(attribute));
        if(auto values=IndexedStringAttrs(
               n,L"Modules",kMaxPluginModules))
            value.modules=std::move(*values);
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Histroy")) {
        auto* const n=node.Get();
        s.history.last_active_page=IntAttr(
            n,L"LastActivePage",s.history.last_active_page);
        s.playlist.split_on_lists=IntAttr(n,L"SplitOnLists",55);
        s.history.sound_path=StringAttr(n,L"SoundPath");
        s.history.playlist_path=StringAttr(n,L"PlayListPath");
        s.history.folder=StringAttr(n,L"Folder");
        s.history.tag_pattern=StringAttr(n,L"TagPattern");
        s.history.check_sub_folder=IntAttr(n,L"CheckSubFolder",1)!=0;
        s.history.advance_file_info=IntAttr(
            n,L"AdvanceFileInfo",s.history.advance_file_info ? 1 : 0)!=0;
        s.history.equalizer_profile=StringAttr(n,L"EQCProfile");
        s.history.lyric_profile=StringAttr(n,L"LRCProfile");
        s.history.playlist_profile=StringAttr(n,L"PLCProfile");
        if(auto values=IndexedStringAttrs(
               n,L"TagNames",kMaxHistoryTagNames))
            s.history.tag_names=std::move(*values);
    }
    if (auto node=SelectOwned(doc,L"/ttplayer/Skin")) {
        auto* const n=node.Get();
        auto v=Attribute(n,L"PackageName");
        if (v.vt==VT_EMPTY) v=Attribute(n,L"SkinFile");
        if (v.vt!=VT_EMPTY) s.skin_file=static_cast<const wchar_t*>(_bstr_t(v));
    }
    return s;
}

bool LoadPlaylistProfile(const std::filesystem::path& path,
                         PlaylistSettings& playlist) {
    try {
        auto profile = LoadLegacyXml(path);
        // SplitOnLists belongs to the global Histroy node.  Per-skin profiles
        // contain only visual settings. Keep all global behaviour fields while
        // changing packages rather than replacing the complete structure with
        // parser defaults for attributes omitted by the sidecar.
        ApplyPlaylistProfile(profile.playlist,playlist);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool LoadPlaylistOptionsProfile(const std::filesystem::path& path,
                                PlaylistSettings& playlist) {
    ComInit com;
    if(FAILED(com.hr) && com.hr!=RPC_E_CHANGED_MODE) return false;
    IXMLDOMDocument* document{};
    IXMLDOMNode* node{};
    if(!LoadOptionsProfileNode(path,L"ttplayer_playlist",L"PlayList",
                               &document,&node)) return false;
    auto document_owner=AdoptCom(document);
    auto node_owner=AdoptCom(node);

    PlaylistSettings loaded=playlist;
    LOGFONTW font=PlaylistLogFont(loaded);
    // FUN_0048DE50 leaves the constructor's LOGFONT unchanged when Font is
    // absent or malformed; a valid root/child profile still imports colours.
    if(LogFontAttr(node,L"Font",font)) SetPlaylistLogFont(loaded,font);
    loaded.text_color=ColorAttr(node,L"Color_Text",loaded.text_color);
    loaded.highlight_color=ColorAttr(
        node,L"Color_Hilight",loaded.highlight_color);
    loaded.background_color=ColorAttr(
        node,L"Color_Bkgnd",loaded.background_color);
    loaded.number_color=ColorAttr(node,L"Color_Number",loaded.number_color);
    loaded.duration_color=ColorAttr(
        node,L"Color_Duration",loaded.duration_color);
    loaded.selected_color=ColorAttr(
        node,L"Color_Select",GetSysColor(COLOR_HIGHLIGHT));
    loaded.alternate_background_color=ColorAttr(
        node,L"Color_Bkgnd2",loaded.alternate_background_color);
    ApplyPlaylistProfile(loaded,playlist);
    return true;
}

bool SavePlaylistOptionsProfile(const std::filesystem::path& path,
                                const PlaylistSettings& playlist) {
    if(path.empty()) return false;
    ComInit com;
    if(FAILED(com.hr) && com.hr!=RPC_E_CHANGED_MODE) return false;
    IXMLDOMDocument* document{};
    IXMLDOMElement* element{};
    if(!CreateOptionsProfileDocument(L"ttplayer_playlist",L"PlayList",
                                     &document,&element)) return false;
    auto document_owner=AdoptCom(document);
    auto element_owner=AdoptCom(element);
    SetAttribute(element,L"Font",LogFontText(PlaylistLogFont(playlist)));
    SetAttribute(element,L"Color_Text",ColorText(playlist.text_color));
    SetAttribute(element,L"Color_Hilight",ColorText(playlist.highlight_color));
    SetAttribute(element,L"Color_Bkgnd",ColorText(playlist.background_color));
    SetAttribute(element,L"Color_Number",ColorText(playlist.number_color));
    SetAttribute(element,L"Color_Duration",ColorText(playlist.duration_color));
    SetAttribute(element,L"Color_Select",ColorText(playlist.selected_color));
    SetAttribute(element,L"Color_Bkgnd2",
                 ColorText(playlist.alternate_background_color));
    const HRESULT saved=document->save(_variant_t(path.wstring().c_str()));
    return SUCCEEDED(saved);
}

bool LoadLyricOptionsProfile(const std::filesystem::path& path,
                             LyricSettings& lyric) {
    ComInit com;
    if(FAILED(com.hr) && com.hr!=RPC_E_CHANGED_MODE) return false;
    IXMLDOMDocument* document{};
    IXMLDOMNode* node{};
    if(!LoadOptionsProfileNode(path,L"ttplayer_lyric",L"Lyric",
                               &document,&node)) return false;
    auto document_owner=AdoptCom(document);
    auto node_owner=AdoptCom(node);

    LyricSettings loaded=lyric;
    LOGFONTW font=loaded.font;
    if(LogFontAttr(node,L"Font",font)) {
        loaded.font=font;
        loaded.font_valid=true;
    }
    loaded.text_color=ColorAttr(node,L"TextColor",loaded.text_color);
    loaded.highlight_color=ColorAttr(
        node,L"HilightColor",loaded.highlight_color);
    loaded.background_color=ColorAttr(
        node,L"BkgndColor",loaded.background_color);
    lyric.font=loaded.font;
    lyric.font_valid=loaded.font_valid;
    lyric.text_color=loaded.text_color;
    lyric.highlight_color=loaded.highlight_color;
    lyric.background_color=loaded.background_color;
    return true;
}

bool SaveLyricOptionsProfile(const std::filesystem::path& path,
                             const LyricSettings& lyric) {
    if(path.empty()) return false;
    ComInit com;
    if(FAILED(com.hr) && com.hr!=RPC_E_CHANGED_MODE) return false;
    IXMLDOMDocument* document{};
    IXMLDOMElement* element{};
    if(!CreateOptionsProfileDocument(L"ttplayer_lyric",L"Lyric",
                                     &document,&element)) return false;
    auto document_owner=AdoptCom(document);
    auto element_owner=AdoptCom(element);
    SetAttribute(element,L"Font",LogFontText(lyric.font));
    SetAttribute(element,L"TextColor",ColorText(lyric.text_color));
    SetAttribute(element,L"HilightColor",ColorText(lyric.highlight_color));
    SetAttribute(element,L"BkgndColor",ColorText(lyric.background_color));
    const HRESULT saved=document->save(_variant_t(path.wstring().c_str()));
    return SUCCEEDED(saved);
}

bool LoadSkinVisualProfile(const std::filesystem::path& path,
                           PlayerSettings& player,
                           PlaylistSettings& playlist,
                           LyricSettings& lyric,
                           VisualSettings& visual) {
    try {
        ComInit com;
        if(FAILED(com.hr) && com.hr!=RPC_E_CHANGED_MODE) return false;
        ComPtr<IXMLDOMDocument> document;
        if(FAILED(CoCreateInstance(__uuidof(DOMDocument60),nullptr,
                CLSCTX_INPROC_SERVER,IID_PPV_ARGS(document.GetAddressOf())))) return false;
        document->put_async(VARIANT_FALSE);
        VARIANT_BOOL loaded{};
        document->load(_variant_t(path.wstring().c_str()),&loaded);
        if(loaded!=VARIANT_TRUE || !SelectOwned(document.Get(),L"/ttplayer")) return false;

        // 004B605A merges into the existing CSettings, not a new instance.
        // 0048DDB2/0048DE50 leave colours/fonts intact when an attribute is
        // absent or malformed. A fresh LoadLegacyXml result injected generic
        // playlist defaults here and discarded the target package's palette.
        auto next_player=player;
        auto next_playlist=playlist;
        auto next_lyric=lyric;
        auto next_visual=visual;
        if(auto node=SelectOwned(document.Get(),L"/ttplayer/Player")) {
            auto* n=node.Get();
            const auto rectangle=[&](const wchar_t* name,RECT& target) {
                // An explicit zero resets an unmaterialised mini pair;
                // an absent attribute is not an explicit zero rectangle.
                if(Attribute(n,name).vt!=VT_EMPTY) target=RectAttr(n,name);
            };
            rectangle(L"PlayerWnd",next_player.player_window);
            rectangle(L"PlayerWnd2",next_player.mini_player_window);
            rectangle(L"LyricWnd",next_player.lyric_window);
            rectangle(L"LyricWnd2",next_player.mini_lyric_window);
            rectangle(L"PlayListWnd",next_player.playlist_window);
            rectangle(L"EqualizerWnd",next_player.equalizer_window);
            next_player.lyric_visible=IntAttr(n,L"LyricVisible",next_player.lyric_visible)!=0;
            next_player.playlist_visible=IntAttr(n,L"PlayListVisible",next_player.playlist_visible)!=0;
            next_player.equalizer_visible=IntAttr(n,L"EqualizerVisible",next_player.equalizer_visible)!=0;
        }
        if(auto node=SelectOwned(document.Get(),L"/ttplayer/PlayList")) {
            auto* n=node.Get();
            LOGFONTW font=PlaylistLogFont(next_playlist);
            if(LogFontAttr(n,L"Font",font)) SetPlaylistLogFont(next_playlist,font);
            next_playlist.text_color=ColorAttr(n,L"Color_Text",next_playlist.text_color);
            next_playlist.highlight_color=ColorAttr(n,L"Color_Hilight",next_playlist.highlight_color);
            next_playlist.background_color=ColorAttr(n,L"Color_Bkgnd",next_playlist.background_color);
            next_playlist.number_color=ColorAttr(n,L"Color_Number",next_playlist.number_color);
            next_playlist.duration_color=ColorAttr(n,L"Color_Duration",next_playlist.duration_color);
            next_playlist.selected_color=ColorAttr(n,L"Color_Select",next_playlist.selected_color);
            next_playlist.alternate_background_color=ColorAttr(n,L"Color_Bkgnd2",next_playlist.alternate_background_color);
            next_playlist.legacy_playlist_generation=IntAttr(n,L"CreateNewVerPlayList",
                next_playlist.legacy_playlist_generation)!=0;
        }
        if(auto node=SelectOwned(document.Get(),L"/ttplayer/Lyric")) {
            auto* n=node.Get();
            if(LogFontAttr(n,L"Font",next_lyric.font)) next_lyric.font_valid=true;
            next_lyric.text_color=ColorAttr(n,L"TextColor",next_lyric.text_color);
            next_lyric.highlight_color=ColorAttr(n,L"HilightColor",next_lyric.highlight_color);
            next_lyric.background_color=ColorAttr(n,L"BkgndColor",next_lyric.background_color);
        }
        if(auto node=SelectOwned(document.Get(),L"/ttplayer/Visual"))
            ApplyVisualAttributes(node.Get(),next_visual,false);
        // Per-skin snapshots never change global ScrollMode, DragLyric,
        // Type/FramesPerSec, playback or playlist behavior. Commit together.
        player=std::move(next_player);
        playlist=std::move(next_playlist);
        lyric=std::move(next_lyric);
        visual=std::move(next_visual);
        return true;
    } catch (const _com_error&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

bool SaveSkinVisualProfile(const std::filesystem::path& path,
                           const PlayerSettings& player,
                           const PlaylistSettings& playlist,
                           const LyricSettings& lyric,
                           const VisualSettings& visual,
                           const std::filesystem::path& global_settings_path) {
    if(path.empty()) return false;
    ComInit com;
    if(FAILED(com.hr) && com.hr!=RPC_E_CHANGED_MODE) return false;
    IXMLDOMDocument* document{};
    if(!OpenSettingsDocument(path,&document)) return false;
    auto document_owner=AdoptCom(document);

    static_cast<void>(global_settings_path);

    // The profile serializer writes this complete subset even though it
    // intentionally leaves the global Type/FramesPerSec untouched.
    if(auto* element=EnsureElement(document,L"Visual")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"SpectrumTopColor",ColorText(visual.spectrum_top_color));
        SetAttribute(element,L"SpectrumBtmColor",ColorText(visual.spectrum_bottom_color));
        SetAttribute(element,L"SpectrumMidColor",ColorText(visual.spectrum_middle_color));
        SetAttribute(element,L"SpectrumPeakColor",ColorText(visual.spectrum_peak_color));
        SetAttribute(element,L"SpectrumWide",visual.spectrum_wide);
        SetAttribute(element,L"BlurSpeed",visual.blur_speed);
        SetAttribute(element,L"Blur",visual.blur ? 1 : 0);
        SetAttribute(element,L"BlurScopeColor",ColorText(visual.blur_scope_color));
        SetAttribute(element,L"TextColor",ColorText(visual.text_color));
        if(visual.font_valid)
            SetAttribute(element,L"Font",LogFontText(visual.font));
    }
    // FUN_0045D5FA sets DAT_00547744 while invoking the common settings
    // serializer. In that branch the sidecar is a per-skin visual snapshot:
    // six window rectangles, the three normal auxiliary visibility flags,
    // lyric font/colours and playlist font/colours. Global playback and mini
    // visibility/top-most values remain in TTPlayer.xml.
    if(auto* element=EnsureElement(document,L"Player")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"PlayerWnd",RectangleText(player.player_window));
        SetAttribute(element,L"PlayerWnd2",RectangleText(player.mini_player_window));
        SetAttribute(element,L"LyricWnd",RectangleText(player.lyric_window));
        SetAttribute(element,L"LyricWnd2",RectangleText(player.mini_lyric_window));
        SetAttribute(element,L"EqualizerWnd",RectangleText(player.equalizer_window));
        SetAttribute(element,L"PlayListWnd",RectangleText(player.playlist_window));
        SetAttribute(element,L"LyricVisible",player.lyric_visible ? 1 : 0);
        SetAttribute(element,L"EqualizerVisible",player.equalizer_visible ? 1 : 0);
        SetAttribute(element,L"PlayListVisible",player.playlist_visible ? 1 : 0);
    }
    if(auto* element=EnsureElement(document,L"Lyric")) {
        auto element_owner=AdoptCom(element);
        if(lyric.font_valid)
            SetAttribute(element,L"Font",LogFontText(lyric.font));
        if(lyric.text_color!=CLR_INVALID)
            SetAttribute(element,L"TextColor",ColorText(lyric.text_color));
        if(lyric.highlight_color!=CLR_INVALID)
            SetAttribute(element,L"HilightColor",ColorText(lyric.highlight_color));
        if(lyric.background_color!=CLR_INVALID)
            SetAttribute(element,L"BkgndColor",ColorText(lyric.background_color));
    }
    if(auto* element=EnsureElement(document,L"PlayList")) {
        auto element_owner=AdoptCom(element);
        // A .ttpl_cfg import supplies the complete original LOGFONT.  When no
        // profile/font chooser has supplied one, keep an existing sidecar's
        // tuple rather than replacing its charset/quality values.
        if(playlist.font_descriptor_valid ||
           StringAttr(element,L"Font").empty())
            SetAttribute(element,L"Font",LogFontText(PlaylistLogFont(playlist)));
        SetAttribute(element,L"Color_Text",ColorText(playlist.text_color));
        SetAttribute(element,L"Color_Hilight",ColorText(playlist.highlight_color));
        SetAttribute(element,L"Color_Bkgnd",ColorText(playlist.background_color));
        SetAttribute(element,L"Color_Number",ColorText(playlist.number_color));
        SetAttribute(element,L"Color_Duration",ColorText(playlist.duration_color));
        SetAttribute(element,L"Color_Select",ColorText(playlist.selected_color));
        SetAttribute(element,L"Color_Bkgnd2",
                     ColorText(playlist.alternate_background_color));
        SetAttribute(element,L"CreateNewVerPlayList",
                     playlist.legacy_playlist_generation ? 1 : 0);
    }
    const HRESULT saved=document->save(_variant_t(path.wstring().c_str()));
    return SUCCEEDED(saved);
}

void SaveWindowState(const std::filesystem::path& path,
                     const Settings& settings) {
    if(path.empty()) return;
    ComInit com; if(FAILED(com.hr) && com.hr!=RPC_E_CHANGED_MODE) return;
    IXMLDOMDocument* document{};
    if(!OpenSettingsDocument(path,&document)) return;
    auto document_owner=AdoptCom(document);
    const auto& player=settings.player;
    if(auto* element=EnsureElement(document,L"Player")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"PlayerWnd",RectangleText(player.player_window));
        SetAttribute(element,L"PlayerWnd2",RectangleText(player.mini_player_window));
        SetAttribute(element,L"LyricWnd",RectangleText(player.lyric_window));
        SetAttribute(element,L"LyricWnd2",RectangleText(player.mini_lyric_window));
        SetAttribute(element,L"DesklrcWnd",
                     RectangleText(player.desktop_lyric_window));
        SetAttribute(element,L"PlayListWnd",RectangleText(player.playlist_window));
        SetAttribute(element,L"EqualizerWnd",RectangleText(player.equalizer_window));
        SetAttribute(element,L"MiniMode",player.mini_mode ? 1 : 0);
        SetAttribute(element,L"TopMost",player.top_most ? 1 : 0);
        SetAttribute(element,L"TopMost2",player.mini_top_most ? 1 : 0);
        SetAttribute(element,L"LyricVisible",player.lyric_visible ? 1 : 0);
        SetAttribute(element,L"LyricTopMost",player.lyric_top_most ? 1 : 0);
        SetAttribute(element,L"LyricVisible2",player.mini_lyric_visible ? 1 : 0);
        SetAttribute(element,L"LyricTopMost2",player.mini_lyric_top_most ? 1 : 0);
        SetAttribute(element,L"PlayListVisible",player.playlist_visible ? 1 : 0);
        SetAttribute(element,L"EqualizerVisible",player.equalizer_visible ? 1 : 0);
        SetAttribute(element,L"OpaqueWhenActive",player.opaque_when_active ? 1 : 0);
        SetAttribute(element,L"AlphaPercent",std::clamp(player.alpha_percent,0,90));
        SetAttribute(element,L"WindowShadow",player.window_shadow ? 1 : 0);
        SetAttribute(element,L"PlayMode",player.play_mode);
        SetAttribute(element,L"AutoSwitchList",player.auto_switch_list ? 1 : 0);
        SetAttribute(element,L"PlayFollowCursor",player.play_follow_cursor ? 1 : 0);
        SetAttribute(element,L"PlayLists",player.playlist_scan_count);
        SetAttribute(element,L"ActiveList",player.active_playlist);
        SetAttribute(element,L"PlayingTime",player.playing_time);
        SetAttribute(element,L"PlayingFileName",player.playing_file_name);
        SetAttribute(element,L"PlayingFileSubTrack",player.playing_file_subtrack);
        SetAttribute(element,L"Mute",player.mute ? 1 : 0);
        SetAttribute(element,L"Volume",std::clamp(player.volume,0,100));
        SetAttribute(element,L"Balance",player.balance);
        SetAttribute(element,L"ShowElapsedTime",
                     player.show_elapsed_time ? 1 : 0);
        SetAttribute(element,L"CheckAssociation",
                     player.check_association ? 1 : 0);
        SetAttribute(element,L"AutoAssociate",player.auto_associate ? 1 : 0);
        SetAttribute(element,L"FirstRun_552",player.first_run_552 ? 1 : 0);
        SetAttribute(element,L"UserWord",player.user_word);
        SetAttribute(element,L"UserWordMD5",player.user_word_md5);
    }
    if(auto* element=EnsureElement(document,L"General")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"StartupMinimize",
                     settings.general.startup_minimize ? 1 : 0);
        SetAttribute(element,L"TrayIcon",settings.general.tray_icon ? 1 : 0);
        SetAttribute(element,L"Fade_Windows",settings.general.fade_windows ? 1 : 0);
        SetAttribute(element,L"ShowHotKeyInTips",
                     settings.general.show_hotkey_in_tips ? 1 : 0);
        SetAttribute(element,L"TipsOnOpen",settings.general.tips_on_open ? 1 : 0);
        SetAttribute(element,L"MenuTips",settings.general.menu_tips ? 1 : 0);
        SetAttribute(element,L"MenuBarPlayList",
                     settings.general.menu_bar_playlist ? 1 : 0);
        SetAttribute(element,L"ScrollTitle",settings.general.scroll_title ? 1 : 0);
        SetAttribute(element,L"SendTitleToDiscord",
                     settings.general.send_title_to_msn ? 1 : 0);
        SetAttribute(element,L"DiscordSyncLyrics",
                     settings.general.discord_sync_lyrics ? 1 : 0);
        // Retain the legacy attribute while old TTPlayer builds may still be
        // pointed at this file.  New builds always prefer the Discord name.
        SetAttribute(element,L"SendTitleToMSN",
                     settings.general.send_title_to_msn ? 1 : 0);
        SetAttribute(element,L"DiscordApplicationId",
                     settings.general.discord_application_id);
        SetAttribute(element,L"Snap_Windows",settings.general.snap_windows);
        SetAttribute(element,L"TitleSlideInterval",
                     settings.general.title_slide_interval);
        SetAttribute(element,L"CheckUpdateDays",settings.general.check_update_days);
        SetAttribute(element,L"LastMessageVer",settings.general.last_message_version);
        SetAttribute(element,L"LastCheckedDay",settings.general.last_checked_day);
        SetAttribute(element,L"AutoShutDown",
                     settings.general.auto_shutdown ? 1 : 0);
        SetAttribute(element,L"ShutDownTime",ClockText(settings.general.shutdown_time));
        SetAttribute(element,L"ClearListOnCmd",
                     settings.general.clear_list_on_command ? 1 : 0);
        SetAttribute(element,L"DefaultListOnCmd",
                     settings.general.default_list_on_command ? 1 : 0);
        SetAttribute(element,L"strDefaultList",settings.general.default_list);
        SetAttribute(element,L"AppIconFile",settings.general.app_icon_file.wstring());
        SetAttribute(element,L"MP3ReadTagPriority",
                     settings.general.mp3_read_tag_priority);
        SetAttribute(element,L"MP3WriteTagType",settings.general.mp3_write_tag_type);
        SetAttribute(element,L"MP3ID3v2Encoding",
                     settings.general.mp3_id3v2_encoding);
        SetAttribute(element,L"MP3ID3v2Padding",
                     settings.general.mp3_id3v2_padding ? 1 : 0);
    }
    if(auto* element=EnsureElement(document,L"Playback")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"AutoPlay",settings.playback.auto_play ? 1 : 0);
        SetAttribute(element,L"ContinuePlay",settings.playback.continue_play ? 1 : 0);
        SetAttribute(element,L"StopWhenFail",
                     settings.playback.stop_when_fail ? 1 : 0);
        SetAttribute(element,L"TracksInterval",settings.playback.track_interval);
        SetAttribute(element,L"ThreadPriority",settings.playback.thread_priority);
        SetAttribute(element,L"FileBuffer",settings.playback.file_buffer);
        SetAttribute(element,L"SoundFadeMode",settings.playback.sound_fade_mode);
        SetAttribute(element,L"FadeDuration",
                     FixedIntegerListText(settings.playback.fade_duration,L','));
        SetAttribute(element,L"TrackFadeDur",
                     settings.playback.track_fade_duration);
        SetAttribute(element,L"AutoGain",settings.playback.auto_gain ? 1 : 0);
        SetAttribute(element,L"AutoScanGain",settings.playback.auto_scan_gain ? 1 : 0);
        SetAttribute(element,L"SkipScanGain",settings.playback.skip_scan_gain ? 1 : 0);
    }
    if(auto* element=EnsureElement(document,L"Device")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"DeviceType",settings.device.device_type);
        SetAttribute(element,L"OutputBits",settings.device.output_bits);
        SetAttribute(element,L"BufferDuration",settings.device.buffer_duration);
        SetAttribute(element,L"HardwareBuffer",
                     settings.device.hardware_buffer ? 1 : 0);
        SetAttribute(element,L"CreatePrimary",
                     settings.device.create_primary ? 1 : 0);
        SetAttribute(element,L"ResampleRate",settings.device.resample_rate);
        SetAttribute(element,L"SsrcMode",settings.device.ssrc_mode);
        SetAttribute(element,L"Dither",settings.device.dither);
    }
    if(auto* element=EnsureElement(document,L"HotKey")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"Global",settings.hotkey.global ? 1 : 0);
        const size_t count=std::min(
            settings.hotkey.key_map.size(),kMaxHotKeyBindings);
        SetAttribute(element,L"KeyMap_Count",
                     static_cast<int>(count));
        for(size_t index=0;index<count;++index) {
            const auto name=L"KeyMap_"+std::to_wstring(index);
            SetAttribute(element,name.c_str(),
                         HotKeyBindingText(settings.hotkey.key_map[index]));
        }
    }
    if(auto* element=EnsureElement(document,L"Visual")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"SpectrumTopColor",ColorText(settings.visual.spectrum_top_color));
        SetAttribute(element,L"SpectrumBtmColor",ColorText(settings.visual.spectrum_bottom_color));
        SetAttribute(element,L"SpectrumMidColor",ColorText(settings.visual.spectrum_middle_color));
        SetAttribute(element,L"SpectrumPeakColor",ColorText(settings.visual.spectrum_peak_color));
        SetAttribute(element,L"SpectrumWide",settings.visual.spectrum_wide);
        SetAttribute(element,L"BlurSpeed",settings.visual.blur_speed);
        SetAttribute(element,L"Blur",settings.visual.blur ? 1 : 0);
        SetAttribute(element,L"BlurScopeColor",ColorText(settings.visual.blur_scope_color));
        SetAttribute(element,L"TextColor",ColorText(settings.visual.text_color));
        SetAttribute(element,L"Type",std::clamp(settings.visual.type,0,4));
        SetAttribute(element,L"FramesPerSec",
                     std::clamp(settings.visual.frames_per_second,0,100));
        if(settings.visual.font_valid)
            SetAttribute(element,L"Font",LogFontText(settings.visual.font));
    }
    if(auto* element=EnsureElement(document,L"FullScreen")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"VisualType",
                     std::clamp(settings.fullscreen.visual_type,0,3));
        constexpr std::array<const wchar_t*,4> relation_names{
            L"PosRelationAll",L"PosRelationGoom",
            L"PosRelationSpectrum",L"PosRelationBlurScope"};
        constexpr std::array<const wchar_t*,4> size_names{
            L"LrcSizeAll",L"LrcSizeGoom",
            L"LrcSizeSpectrum",L"LrcSizeBlurScope"};
        for(size_t index=0;index<relation_names.size();++index) {
            SetAttribute(element,relation_names[index],
                         settings.fullscreen.position_relation[index]);
            SetAttribute(element,size_names[index],
                         settings.fullscreen.lyric_size[index]);
        }
    }
    if(auto* element=EnsureElement(document,L"DeskLrc")) {
        auto element_owner=AdoptCom(element);
        const auto& value=settings.desktop_lyric;
        SetAttribute(element,L"Profile",value.profile);
        SetAttribute(element,L"Lines",value.lines);
        SetAttribute(element,L"Align",value.align);
        SetAttribute(element,L"BkgndAlpha",value.background_alpha);
        SetAttribute(element,L"TextAlpha",value.text_alpha);
        SetAttribute(element,L"Topmost",value.topmost ? 1 : 0);
        SetAttribute(element,L"KaraokeMode",value.karaoke_mode ? 1 : 0);
        SetAttribute(element,L"AutoWidth",value.auto_width ? 1 : 0);
        SetAttribute(element,L"UnlockWhenClose",
                     value.unlock_when_close ? 1 : 0);
        // Matches CSettings: Lock is meaningful and serialized only when the
        // close action does not force an unlock.
        if(!value.unlock_when_close)
            SetAttribute(element,L"Lock",value.lock ? 1 : 0);
        else
            element->removeAttribute(_bstr_t(L"Lock"));
        SetAttribute(element,L"BkgTransp",
                     value.background_transparent ? 1 : 0);
        SetAttribute(element,L"Smooth",value.smooth ? 1 : 0);
        SetAttribute(element,L"Border",value.border ? 1 : 0);
        SetAttribute(element,L"Shadow",value.shadow ? 1 : 0);
        SetAttribute(element,L"BkgndShow",value.background_show ? 1 : 0);
        if(value.font_valid)
            SetAttribute(element,L"Font",LogFontText(value.font));
        SetAttribute(element,L"BorderColor",ColorText(value.border_color));
        SetAttribute(element,L"ShadowColor",ColorText(value.shadow_color));
        SetAttribute(element,L"BkgndColor",ColorText(value.background_color));
        SaveDesktopLyricProfile(element,L"Profile",value.current,false);
        for(size_t index=0;index<value.profiles.size();++index) {
            const auto prefix=L"Profile"+std::to_wstring(index+1);
            SaveDesktopLyricProfile(
                element,prefix.c_str(),value.profiles[index],true);
        }
    }
    const auto values_text=[](const std::array<int,11>& values) {
        std::wostringstream output;
        for(size_t index=0; index<values.size(); ++index) {
            if(index==1) output << L':';
            else if(index>1) output << L',';
            output << values[index];
        }
        return output.str();
    };
    if(auto* element=EnsureElement(document,L"Equalizer")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"Profile",settings.equalizer.profile);
        SetAttribute(element,L"ProfileLast",settings.equalizer.profile_last);
        SetAttribute(element,L"Surround",settings.equalizer.surround);
        SetAttribute(element,L"Custom",values_text(settings.equalizer.custom));
        SetAttribute(element,L"Current",values_text(settings.equalizer.current));
    }
    if(auto* element=EnsureElement(document,L"Lyric")) {
        auto element_owner=AdoptCom(element);
        if(settings.lyric.font_valid)
            SetAttribute(element,L"Font",LogFontText(settings.lyric.font));
        if(settings.lyric.text_color!=CLR_INVALID)
            SetAttribute(element,L"TextColor",ColorText(settings.lyric.text_color));
        if(settings.lyric.highlight_color!=CLR_INVALID)
            SetAttribute(element,L"HilightColor",ColorText(settings.lyric.highlight_color));
        if(settings.lyric.background_color!=CLR_INVALID)
            SetAttribute(element,L"BkgndColor",ColorText(settings.lyric.background_color));
        SetAttribute(element,L"CharSet",settings.lyric.charset);
        SetAttribute(element,L"ScrollMode",settings.lyric.scroll_mode);
        SetAttribute(element,L"ScrollMode2",settings.lyric.mini_scroll_mode);
        SetAttribute(element,L"ScrollModeFS",settings.lyric.fullscreen_scroll_mode);
        SetAttribute(element,L"TextAlignFS",settings.lyric.fullscreen_text_align);
        SetAttribute(element,L"RowIntervalFS",settings.lyric.fullscreen_row_interval);
        SetAttribute(element,L"FadeIndexFS",settings.lyric.fullscreen_fade_index);
        SetAttribute(element,L"FadeHilightFS",
                     settings.lyric.fullscreen_fade_highlight ? 1 : 0);
        SetAttribute(element,L"KaraokeModeFS",
                     settings.lyric.fullscreen_karaoke_mode ? 1 : 0);
        SetAttribute(element,L"TransparentFS",
                     settings.lyric.fullscreen_transparent ? 1 : 0);
        SetAttribute(element,L"AutoFontFS",
                     settings.lyric.fullscreen_auto_font ? 1 : 0);
        if(settings.lyric.fullscreen_font_valid)
            SetAttribute(element,L"FontFS",
                         LogFontText(settings.lyric.fullscreen_font));
        SetAttribute(element,L"TextColorFS",
                     ColorText(settings.lyric.fullscreen_text_color));
        SetAttribute(element,L"HilightColorFS",
                     ColorText(settings.lyric.fullscreen_highlight_color));
        SetAttribute(element,L"BkgndColorFS",
                     ColorText(settings.lyric.fullscreen_background_color));
        SetAttribute(element,L"TextAlign",settings.lyric.text_align);
        SetAttribute(element,L"RowInterval",settings.lyric.row_interval);
        SetAttribute(element,L"FadeIndex",settings.lyric.fade_index);
        SetAttribute(element,L"FadeHilight",settings.lyric.fade_highlight ? 1 : 0);
        SetAttribute(element,L"KaraokeMode",settings.lyric.karaoke_mode ? 1 : 0);
        SetAttribute(element,L"Transparent",settings.lyric.transparent ? 1 : 0);
        SetAttribute(element,L"TransSkin",settings.lyric.transparent_skin ? 1 : 0);
        SetAttribute(element,L"AutoLoadLyric",
                     settings.lyric.auto_load_lyric ? 1 : 0);
        SetAttribute(element,L"AutoSaveLyricTag",
                     settings.lyric.auto_save_lyric_tag ? 1 : 0);
        SetAttribute(element,L"DontLoadLyricTag",
                     settings.lyric.dont_load_lyric_tag ? 1 : 0);
        SetAttribute(element,L"AutoVisible",settings.lyric.auto_visible ? 1 : 0);
        SetAttribute(element,L"AutoWidth",settings.lyric.auto_width ? 1 : 0);
        SetAttribute(element,L"AutoWidthOnlyVert",
                     settings.lyric.auto_width_only_vertical ? 1 : 0);
        SetAttribute(element,L"DragLyric",settings.lyric.drag_lyric ? 1 : 0);
        SetAttribute(element,L"MouseWheelAdjust",
                     settings.lyric.mouse_wheel_adjust ? 1 : 0);
        SetAttribute(element,L"SaveCompress",
                     settings.lyric.save_compress ? 1 : 0);
        SetAttribute(element,L"TrimSpaces",settings.lyric.trim_spaces ? 1 : 0);
        SetAttribute(element,L"LyricSaveMode",settings.lyric.lyric_save_mode);
        SetAttribute(element,L"AddInIndex",settings.lyric.add_in_index);
        SetAttribute(element,L"AutoDownLoad",
                     settings.lyric.auto_download ? 1 : 0);
        SetAttribute(element,L"DownLoadWhenFullInfo",
                     settings.lyric.download_when_full_info ? 1 : 0);
        SetAttribute(element,L"AutoAssociate",
                     settings.lyric.auto_associate ? 1 : 0);
        SetAttribute(element,L"AutoSelectDownload",
                     settings.lyric.auto_select_download ? 1 : 0);
        SetAttribute(element,L"OverWrite",settings.lyric.overwrite ? 1 : 0);
        SetAttribute(element,L"SameFileTitle",
                     settings.lyric.same_file_title ? 1 : 0);
        SetAttribute(element,L"SaveToSoundFolder",
                     settings.lyric.save_to_sound_folder ? 1 : 0);
        SetAttribute(element,L"DownLoadFolder",
                     settings.lyric.download_folder.wstring());
        SetAttribute(element,L"NewLineAfterTag",
                     settings.lyric.new_line_after_tag ? 1 : 0);
        SetAttribute(element,L"DisplayMode",settings.lyric.display_mode);
        const size_t folder_count=std::min(
            settings.lyric.folders.size(),kMaxLyricFolders);
        SetAttribute(element,L"Folders_Count",static_cast<int>(folder_count));
        for(size_t index=0; index<folder_count; ++index) {
            const auto name=L"Folders_"+std::to_wstring(index);
            SetAttribute(element,name.c_str(),settings.lyric.folders[index]);
        }
    }
    if(auto* element=EnsureElement(document,L"PlayList")) {
        auto element_owner=AdoptCom(element);
        if(settings.playlist.font_descriptor_valid ||
           StringAttr(element,L"Font").empty())
            SetAttribute(element,L"Font",
                         LogFontText(PlaylistLogFont(settings.playlist)));
        SetAttribute(element,L"Color_Text",ColorText(settings.playlist.text_color));
        SetAttribute(element,L"Color_Hilight",ColorText(settings.playlist.highlight_color));
        SetAttribute(element,L"Color_Bkgnd",ColorText(settings.playlist.background_color));
        SetAttribute(element,L"Color_Number",ColorText(settings.playlist.number_color));
        SetAttribute(element,L"Color_Duration",ColorText(settings.playlist.duration_color));
        SetAttribute(element,L"Color_Select",ColorText(settings.playlist.selected_color));
        SetAttribute(element,L"Color_Bkgnd2",
                     ColorText(settings.playlist.alternate_background_color));
        SetAttribute(element,L"CreateNewVerPlayList",
                     settings.playlist.legacy_playlist_generation ? 1 : 0);
        SetAttribute(element,L"LibraryMode",settings.playlist.library_mode ? 1 : 0);
        SetAttribute(element,L"ItemTips",settings.playlist.item_tips ? 1 : 0);
        SetAttribute(element,L"DisableDelFile",
                     settings.playlist.disable_delete_file ? 1 : 0);
        SetAttribute(element,L"EnableDragDrop",
                     settings.playlist.enable_drag_drop ? 1 : 0);
        SetAttribute(element,L"ReadInfoMode",settings.playlist.read_info_mode);
        SetAttribute(element,L"TitleNumber",settings.playlist.title_number ? 1 : 0);
        SetAttribute(element,L"IgnoreBadFiles",
                     settings.playlist.ignore_bad_files ? 1 : 0);
        SetAttribute(element,L"SaveRelativePath",
                     settings.playlist.save_relative_path ? 1 : 0);
        SetAttribute(element,L"SaveTags",settings.playlist.save_tags ? 1 : 0);
        SetAttribute(element,L"TagFormat",settings.playlist.tag_format);
        SetAttribute(element,L"ClickRating",
                     settings.playlist.click_rating ? 1 : 0);
        SetAttribute(element,L"TagTitleFormat",
                     settings.playlist.tag_title_format);
        SetAttribute(element,L"DefTitleFormat",
                     settings.playlist.default_title_format);
    }
    if(auto* element=EnsureElement(document,L"Library")) {
        auto element_owner=AdoptCom(element);
        const auto& value=settings.library;
        SetAttribute(element,L"Enabled",value.enabled ? 1 : 0);
        SetAttribute(element,L"Valid",value.valid ? 1 : 0);
        SetAttribute(element,L"PlayingCatalog",value.playing_catalog);
        SetAttribute(element,L"MonitorDir",
                     value.monitor_directories ? 1 : 0);
        const size_t directory_count=std::min(
            value.directories.size(),kMaxLibraryDirectories);
        SetAttribute(element,L"Directories_Count",
                     static_cast<int>(directory_count));
        for(size_t index=0;index<directory_count;++index) {
            const auto name=L"Directories_"+std::to_wstring(index);
            SetAttribute(element,name.c_str(),
                         MediaLibraryDirectoryText(value.directories[index]));
        }
        SetAttribute(element,L"MaxItemCount",value.max_item_count);
    }
    if(auto* element=EnsureElement(document,L"Network")) {
        auto element_owner=AdoptCom(element);
        const auto& value=settings.network;
        SetAttribute(element,L"Proxy_Type",value.proxy_type);
        SetAttribute(element,L"Proxy_Server",value.proxy_server);
        SetAttribute(element,L"Proxy_Port",value.proxy_port);
        SetAttribute(element,L"Proxy_UserName",value.proxy_username);
        SetAttribute(element,L"Proxy_Password",value.proxy_password);
        SetAttribute(element,L"FreedbAutoQuery",
                     value.freedb_auto_query ? 1 : 0);
        SetAttribute(element,L"ShowInfoWhenFail",
                     value.show_info_when_fail ? 1 : 0);
        SetAttribute(element,L"FreedbServer",value.freedb_server);
        SetAttribute(element,L"CacheFolder",value.cache_folder.wstring());
        SaveIndexedStringAttrs(
            element,L"ServerList",value.server_list,kMaxNetworkServers);
        SetAttribute(element,L"AcceptRecomList",
                     value.accept_recommendation_list ? 1 : 0);
        SetAttribute(element,L"LastRecomListID",
                     value.last_recommendation_list_id);
        SetAttribute(element,L"CheckUpdateDays",value.check_update_days);
        SetAttribute(element,L"LastCheckedDay",value.last_checked_day);
        SetAttribute(element,L"DoCache",value.cache_enabled ? 1 : 0);
        SetAttribute(element,L"CacheSpaceSize",value.cache_space_size);
        SetAttribute(element,L"LastMessageTime",value.last_message_time);
        SetAttribute(element,L"DownloadFolder",
                     value.download_folder.wstring());
        SetAttribute(element,L"CreateFolderByAritst",
                     value.create_folder_by_artist ? 1 : 0);
        SetAttribute(element,L"ReplaceFile",value.replace_file ? 1 : 0);
        SetAttribute(element,L"DownloadWhenListen",
                     value.download_when_listen ? 1 : 0);
        SetAttribute(element,L"DownloadLrc",value.download_lyric ? 1 : 0);
        SetAttribute(element,L"MaxDownTasks",value.max_download_tasks);
        SetAttribute(element,L"DownloadNum",value.download_num);
        SetAttribute(element,L"SpeedModle",value.speed_mode);
        SetAttribute(element,L"TipShowedTime",value.tip_showed_time);
        SetAttribute(element,L"RemerberDownloadMode",
                     value.remember_download_mode ? 1 : 0);
    }
    if(auto* element=EnsureElement(document,L"Convert")) {
        auto element_owner=AdoptCom(element);
        const auto& value=settings.convert;
        SetAttribute(element,L"WriterIndex",value.writer_index);
        SetAttribute(element,L"LameMode",value.lame_mode);
        SetAttribute(element,L"LameBitrate",value.lame_bitrate);
        SetAttribute(element,L"LameQuality",value.lame_quality);
        SetAttribute(element,L"OutputBits",value.output_bits);
        SetAttribute(element,L"ResampleRate",value.resample_rate);
        SetAttribute(element,L"ReplayGain",value.replay_gain);
        SetAttribute(element,L"Equalizer",value.equalizer);
        SetAttribute(element,L"Surround",value.surround);
        SetAttribute(element,L"Folder",value.folder.wstring());
        SetAttribute(element,L"SaveMode",value.save_mode);
        SetAttribute(element,L"AddNumber",value.add_number);
        SetAttribute(element,L"AddToPlayList",value.add_to_playlist);
        SetAttribute(element,L"ThreadPriority",value.thread_priority);
    }
    if(auto* element=EnsureElement(document,L"Plugin")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"Folder",settings.plugin.folder.wstring());
        SaveIndexedStringAttrs(
            element,L"Modules",settings.plugin.modules,kMaxPluginModules);
    }
    if(auto* element=EnsureElement(document,L"Histroy")) {
        auto element_owner=AdoptCom(element);
        SetAttribute(element,L"LastActivePage",settings.history.last_active_page);
        SetAttribute(element,L"SplitOnLists",settings.playlist.split_on_lists);
        SetAttribute(element,L"SoundPath",settings.history.sound_path.wstring());
        SetAttribute(element,L"PlayListPath",
                     settings.history.playlist_path.wstring());
        SetAttribute(element,L"Folder",settings.history.folder.wstring());
        SetAttribute(element,L"TagPattern",settings.history.tag_pattern);
        SetAttribute(element,L"CheckSubFolder",
                     settings.history.check_sub_folder ? 1 : 0);
        SetAttribute(element,L"AdvanceFileInfo",
                     settings.history.advance_file_info ? 1 : 0);
        SetAttribute(element,L"EQCProfile",
                     settings.history.equalizer_profile.wstring());
        SetAttribute(element,L"LRCProfile",
                     settings.history.lyric_profile.wstring());
        SetAttribute(element,L"PLCProfile",
                     settings.history.playlist_profile.wstring());
        SaveIndexedStringAttrs(
            element,L"TagNames",settings.history.tag_names,
            kMaxHistoryTagNames);
    }
    if(auto* element=EnsureElement(document,L"Skin")) {
        auto element_owner=AdoptCom(element);
        const std::wstring package=settings.skin_file.empty()
            ? std::wstring(L"<Default_Skin>") : settings.skin_file;
        SetAttribute(element,L"PackageName",package);
    }
    document->save(_variant_t(path.wstring().c_str()));
}
}
