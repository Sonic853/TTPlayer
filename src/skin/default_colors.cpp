#include "ttplayer/skin/default_colors.h"
#include "ttplayer/skin/skin_package.h"

#include <comdef.h>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <msxml6.h>
#include <string>
#include <wrl/client.h>

namespace ttplayer::skin {
namespace {
using Microsoft::WRL::ComPtr;
struct ComScope {
    HRESULT result{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct Module {
    HMODULE value{};
    ~Module() { if (value) FreeLibrary(value); }
};

ComPtr<IXMLDOMNode> ColorNode(const SkinPackage& package, HMODULE comm,
                            const char* entry, const wchar_t* xpath) {
    ComPtr<IXMLDOMNode> node;
    if (!package.Has(entry)) return node;
    try {
        const auto bytes = package.ReadEntry(entry, comm);
        ComPtr<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return node;
        ULONG written{};
        if (FAILED(stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written)) ||
            written != bytes.size() || FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr)))
            return node;
        ComPtr<IXMLDOMDocument> document;
        if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&document)))) return node;
        document->put_async(VARIANT_FALSE);
        document->put_resolveExternals(VARIANT_FALSE);
        document->put_validateOnParse(VARIANT_FALSE);
        VARIANT_BOOL loaded{};
        document->load(_variant_t(static_cast<IUnknown*>(stream.Get())), &loaded);
        if (loaded == VARIANT_TRUE)
            document->selectSingleNode(_bstr_t(xpath), &node);
    } catch (const std::exception&) {
        // One damaged descriptor must not discard the other two palettes.
    } catch (const _com_error&) {
    }
    return node;
}

void Color(IXMLDOMNode* node, const wchar_t* name, COLORREF& target) {
    if (!node) return;
    ComPtr<IXMLDOMNamedNodeMap> attributes;
    ComPtr<IXMLDOMNode> attribute;
    if (FAILED(node->get_attributes(&attributes)) || !attributes ||
        FAILED(attributes->getNamedItem(_bstr_t(name), &attribute)) || !attribute) return;
    _variant_t value;
    if (FAILED(attribute->get_nodeValue(&value)) || value.vt != VT_BSTR || !value.bstrVal)
        return;
    unsigned int red{}, green{}, blue{};
    if (swscanf_s(value.bstrVal, L"#%2x%2x%2x", &red, &green, &blue) == 3)
        target = RGB(red, green, blue);
}

SkinColors ReadRuntimeColors() noexcept {
    try {
        std::wstring filename(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, filename.data(),
                                                static_cast<DWORD>(filename.size()));
        if (!length || length >= filename.size()) return {};
        filename.resize(length);
        const auto directory = std::filesystem::path(filename).parent_path();
        Module resources{LoadLibraryExW((directory / L"ttpres.dll").c_str(),
                                        nullptr, LOAD_LIBRARY_AS_DATAFILE)};
        if (!resources.value) return {};
        Module comm{LoadLibraryW((directory / L"ttpcomm.dll").c_str())};
        return ReadDefaultSkinColors(resources.value, comm.value);
    } catch (...) {
        return {};
    }
}
} // namespace

SkinColors ReadDefaultSkinColors(HMODULE resources, HMODULE ttpcomm) {
    SkinColors colors;
    ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) return colors;
    const auto package = SkinPackage::OpenResource(resources, L"<DEFAULT_SKIN>");
    const auto playlist = ColorNode(package, ttpcomm, "Playlist.xml", L"/ttplayer_playlist/PlayList");
    Color(playlist.Get(), L"Color_Text", colors.playlist.text_color);
    Color(playlist.Get(), L"Color_Hilight", colors.playlist.highlight_color);
    Color(playlist.Get(), L"Color_Number", colors.playlist.number_color);
    Color(playlist.Get(), L"Color_Duration", colors.playlist.duration_color);
    if (playlist) colors.playlist.selected_color = GetSysColor(COLOR_HIGHLIGHT);
    Color(playlist.Get(), L"Color_Select", colors.playlist.selected_color);
    Color(playlist.Get(), L"Color_Bkgnd", colors.playlist.background_color);
    Color(playlist.Get(), L"Color_Bkgnd2", colors.playlist.alternate_background_color);
    const auto lyric = ColorNode(package, ttpcomm, "Lyric.xml", L"/ttplayer_lyric/Lyric");
    Color(lyric.Get(), L"TextColor", colors.lyric.text_color);
    Color(lyric.Get(), L"HilightColor", colors.lyric.highlight_color);
    Color(lyric.Get(), L"BkgndColor", colors.lyric.background_color);
    const auto visual = ColorNode(package, ttpcomm, "Visual.xml", L"/ttplayer_visual/Visual");
    Color(visual.Get(), L"SpectrumTopColor", colors.visual.spectrum_top_color);
    Color(visual.Get(), L"SpectrumBtmColor", colors.visual.spectrum_bottom_color);
    Color(visual.Get(), L"SpectrumMidColor", colors.visual.spectrum_middle_color);
    Color(visual.Get(), L"SpectrumPeakColor", colors.visual.spectrum_peak_color);
    Color(visual.Get(), L"BlurScopeColor", colors.visual.blur_scope_color);
    Color(visual.Get(), L"TextColor", colors.visual.text_color);
    return colors;
}

const SkinColors& DefaultSkinColors() noexcept {
    static const SkinColors colors = ReadRuntimeColors();
    return colors;
}
} // namespace ttplayer::skin
