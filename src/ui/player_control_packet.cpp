#include "ttplayer/ui/player_control_packet.h"

#include <cerrno>
#include <cwchar>
#include <utility>

#include <comdef.h>
#include <msxml6.h>
#include <objbase.h>
#include <wrl/client.h>

namespace ttplayer::ui {
namespace {
using Microsoft::WRL::ComPtr;

class ScopedComInitialization {
public:
    ScopedComInitialization() noexcept
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedComInitialization() {
        if (result_ == S_OK || result_ == S_FALSE) CoUninitialize();
    }
    [[nodiscard]] bool Available() const noexcept {
        // The player UI normally entered OleInitialize already.  A caller on
        // an MTA thread receives RPC_E_CHANGED_MODE, but COM is still usable.
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }
private:
    HRESULT result_{};
};

std::wstring Attribute(IXMLDOMElement* element, const wchar_t* name) {
    if (!element || !name) return {};
    VARIANT value{};
    VariantInit(&value);
    const HRESULT result = element->getAttribute(_bstr_t(name), &value);
    std::wstring text;
    if (SUCCEEDED(result) && value.vt == VT_BSTR && value.bstrVal)
        text.assign(value.bstrVal, SysStringLen(value.bstrVal));
    VariantClear(&value);
    return text;
}

int IntegerAttribute(IXMLDOMElement* element, const wchar_t* name) noexcept {
    try {
        const std::wstring value = Attribute(element, name);
        if (value.empty()) return 0;
        wchar_t* end{};
        errno = 0;
        const long parsed = std::wcstol(value.c_str(), &end, 10);
        // atoi, used at 0045F41B, accepts a numeric prefix and returns zero
        // when the first non-space character is not numeric.
        if (end == value.c_str() || errno == ERANGE) return 0;
        return static_cast<int>(parsed);
    } catch (...) {
        return 0;
    }
}

bool ElementNameIs(IXMLDOMElement* element, const wchar_t* expected) {
    if (!element || !expected) return false;
    BSTR value{};
    if (FAILED(element->get_tagName(&value)) || !value) return false;
    const bool equal = std::wcscmp(value, expected) == 0;
    SysFreeString(value);
    return equal;
}

template <typename Callback>
void ForEachChildElement(IXMLDOMNode* parent, Callback&& callback) {
    if (!parent) return;
    ComPtr<IXMLDOMNodeList> children;
    if (FAILED(parent->get_childNodes(children.GetAddressOf())) || !children)
        return;
    for (;;) {
        ComPtr<IXMLDOMNode> node;
        if (children->nextNode(node.GetAddressOf()) != S_OK || !node) break;
        DOMNodeType type{};
        if (FAILED(node->get_nodeType(&type)) || type != NODE_ELEMENT) continue;
        ComPtr<IXMLDOMElement> element;
        if (SUCCEEDED(node.As(&element)) && element) callback(element.Get());
    }
}
} // namespace

std::optional<PlayerControlPacket> ParsePlayerControlPacketXml(
    std::wstring_view xml) noexcept {
    if (xml.empty()) return std::nullopt;
    try {
        const ScopedComInitialization com;
        if (!com.Available()) return std::nullopt;

        ComPtr<IXMLDOMDocument2> document;
        if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(document.GetAddressOf()))) ||
            !document) {
            return std::nullopt;
        }
        document->put_async(VARIANT_FALSE);
        document->put_validateOnParse(VARIANT_FALSE);
        document->put_resolveExternals(VARIANT_FALSE);
        // The control packet needs elements and attributes only.  Refusing a
        // DTD prevents /e from becoming a filesystem/network entity loader.
        static_cast<void>(document->setProperty(
            _bstr_t(L"ProhibitDTD"), _variant_t(VARIANT_TRUE)));

        const std::wstring owned(xml);
        VARIANT_BOOL loaded{VARIANT_FALSE};
        if (FAILED(document->loadXML(_bstr_t(owned.c_str()), &loaded)) ||
            loaded != VARIANT_TRUE) {
            return std::nullopt;
        }

        ComPtr<IXMLDOMElement> root;
        if (FAILED(document->get_documentElement(root.GetAddressOf())) ||
            !root || !ElementNameIs(root.Get(), L"ctrlparam")) {
            return std::nullopt;
        }

        PlayerControlPacket packet;
        ForEachChildElement(root.Get(), [&](IXMLDOMElement* child) {
            if (ElementNameIs(child, L"info")) {
                packet.select = IntegerAttribute(child, L"select");
                packet.head = IntegerAttribute(child, L"head");
                packet.play = IntegerAttribute(child, L"play");
                return;
            }
            if (!ElementNameIs(child, L"songs")) return;
            ForEachChildElement(child, [&](IXMLDOMElement* item) {
                if (!ElementNameIs(item, L"item")) return;
                packet.songs.push_back({Attribute(item, L"url"),
                                        Attribute(item, L"artist"),
                                        Attribute(item, L"title")});
            });
        });
        return packet;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace ttplayer::ui
