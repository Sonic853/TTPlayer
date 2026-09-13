#pragma once
#include "ttplayer/core/text.h"
#include <msxml6.h>
#include <comdef.h>
#include <wrl/client.h>
#include <stdexcept>

namespace ttplayer::lyrics::xml {
using Microsoft::WRL::ComPtr;
struct Apartment {
    HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
inline ComPtr<IXMLDOMDocument2> Parse(const std::string& bytes) {
    if (bytes.size() > 2 * 1024 * 1024 || bytes.find('\0') != bytes.npos)
        throw std::runtime_error("Invalid XML size/embedded NUL");
    ComPtr<IXMLDOMDocument2> doc;
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(doc.GetAddressOf())))) throw std::runtime_error("MSXML unavailable");
    doc->put_async(VARIANT_FALSE);
    doc->put_validateOnParse(VARIANT_FALSE);
    doc->put_resolveExternals(VARIANT_FALSE);
    doc->setProperty(_bstr_t(L"ProhibitDTD"), _variant_t(true));
    auto text = core::Utf8ToWide(bytes.starts_with("\xef\xbb\xbf") ? bytes.substr(3) : bytes);
    VARIANT_BOOL ok{};
    if (FAILED(doc->loadXML(_bstr_t(text.c_str()), &ok)) || !ok) throw std::runtime_error("Invalid XML");
    return doc;
}
inline std::wstring Attribute(IXMLDOMNode* node, const wchar_t* key) {
    ComPtr<IXMLDOMNamedNodeMap> attrs; ComPtr<IXMLDOMNode> attr;
    if (!node || FAILED(node->get_attributes(&attrs)) || !attrs) return {};
    attrs->getNamedItem(_bstr_t(key), &attr);
    if (!attr) return {};
    _variant_t value; if (FAILED(attr->get_nodeValue(&value))) return {};
    return static_cast<const wchar_t*>(_bstr_t(value));
}
inline std::string Serialize(IXMLDOMDocument2* doc) {
    BSTR text{};
    if (FAILED(doc->get_xml(&text))) throw std::runtime_error("XML serialization");
    _bstr_t owned(text, false);
    return core::WideToUtf8(static_cast<const wchar_t*>(owned));
}
} // namespace ttplayer::lyrics::xml
