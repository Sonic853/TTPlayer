#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include "modern_file_dialog.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <sstream>
#include <string>

#include <commctrl.h>
#include <comdef.h>
#include <msxml6.h>
#include <prsht.h>

namespace ttplayer::ui {
using namespace detail;
namespace {

constexpr UINT kVisualOptionsPage = 0x00fd;
constexpr UINT kVisualProfileMenu = 0x009e;
constexpr UINT kVisualType = 0x0452;
constexpr UINT kVisualFrames = 0x0427;
constexpr UINT kVisualFramesText = 0x07f3;
constexpr UINT kScopeSpeed = 0x0422;
constexpr UINT kSpectrumWide = 0x0849;
constexpr UINT kScopeBlur = 0x044d;
constexpr UINT kPeakColor = 0x047e;
constexpr UINT kTopColor = 0x047f;
constexpr UINT kMiddleColor = 0x0480;
constexpr UINT kBottomColor = 0x0481;
constexpr UINT kScopeColor = 0x0482;
constexpr UINT kVisualTextColor = 0x0483;
constexpr UINT kVisualFont = 0x040c;
constexpr UINT kVisualProfile = 0x086a;
constexpr UINT kLoadVisualProfile = 0x7d14;
constexpr UINT kSaveVisualProfile = 0x7d15;
constexpr UINT kSaveAllOptions = 0x04d2;
constexpr UINT kResetAllOptions = 0x04d3;
constexpr int kPropertySheetApply = 0x3021;
constexpr UINT_PTR kVisualOptionsSheetSubclass = 0x5454564f;
constexpr wchar_t kVisualButtonImageProperty[] =
    L"TTPlayer.VisualOptions.ButtonImage";

constexpr std::array<UINT, 6> kColorControls{
    kPeakColor, kTopColor, kMiddleColor, kBottomColor, kScopeColor,
    kVisualTextColor};

COLORREF* VisualColor(settings::VisualSettings& visual, UINT control) {
    switch (control) {
    case kPeakColor: return &visual.spectrum_peak_color;
    case kTopColor: return &visual.spectrum_top_color;
    case kMiddleColor: return &visual.spectrum_middle_color;
    case kBottomColor: return &visual.spectrum_bottom_color;
    case kScopeColor: return &visual.blur_scope_color;
    case kVisualTextColor: return &visual.text_color;
    default: return nullptr;
    }
}

const COLORREF* VisualColor(const settings::VisualSettings& visual,
                            UINT control) {
    return VisualColor(const_cast<settings::VisualSettings&>(visual), control);
}

void DrawColorButton(const DRAWITEMSTRUCT& item, COLORREF color) {
    RECT bounds = item.rcItem;
    UINT state = DFCS_BUTTONPUSH;
    if ((item.itemState & ODS_SELECTED) != 0) state |= DFCS_PUSHED;
    if ((item.itemState & ODS_DISABLED) != 0) state |= DFCS_INACTIVE;
    DrawFrameControl(item.hDC, &bounds, DFC_BUTTON, state);

    InflateRect(&bounds, -4, -4);
    RECT swatch = bounds;
    swatch.right = std::min(swatch.right, swatch.left + 14);
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(item.hDC, &swatch, brush);
    DeleteObject(brush);
    FrameRect(item.hDC, &swatch,
              reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    wchar_t caption[64]{};
    GetWindowTextW(item.hwndItem, caption,
                   static_cast<int>(std::size(caption)));
    bounds.left = swatch.right + 3;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, GetSysColor(
        (item.itemState & ODS_DISABLED) ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    DrawTextW(item.hDC, caption, -1, &bounds,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) != 0)
        DrawFocusRect(item.hDC, &bounds);
}

void InstallButtonBitmap(HWND dialog, UINT control, UINT resource) {
    const HWND button = GetDlgItem(dialog, static_cast<int>(control));
    if (!button || GetPropW(button, kVisualButtonImageProperty)) return;
    const HMODULE module = reinterpret_cast<HMODULE>(
        GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
    const HBITMAP bitmap = static_cast<HBITMAP>(LoadImageW(
        module, MAKEINTRESOURCEW(resource), IMAGE_BITMAP, 0, 0,
        LR_CREATEDIBSECTION));
    BITMAP details{};
    if (!bitmap || !GetObjectW(bitmap, sizeof(details), &details)) {
        if (bitmap) DeleteObject(bitmap);
        return;
    }
    const HIMAGELIST images = ImageList_Create(
        details.bmWidth, details.bmHeight, ILC_COLOR24 | ILC_MASK, 1, 0);
    if (!images) {
        DeleteObject(bitmap);
        return;
    }
    ImageList_AddMasked(images, bitmap, RGB(192, 192, 192));
    DeleteObject(bitmap);
    BUTTON_IMAGELIST layout{};
    layout.himl = images;
    layout.margin = {3, 0, 3, 0};
    layout.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
    if (!SendMessageW(button, BCM_SETIMAGELIST, 0,
                      reinterpret_cast<LPARAM>(&layout))) {
        ImageList_Destroy(images);
        return;
    }
    SetPropW(button, kVisualButtonImageProperty,
             reinterpret_cast<HANDLE>(images));
}

void DestroyButtonBitmap(HWND dialog, UINT control) {
    const HWND button = GetDlgItem(dialog, static_cast<int>(control));
    if (!button) return;
    const auto images = reinterpret_cast<HIMAGELIST>(
        RemovePropW(button, kVisualButtonImageProperty));
    if (images) ImageList_Destroy(images);
}

std::wstring XmlAttribute(IXMLDOMNode* node, const wchar_t* name) {
    if (!node) return {};
    IXMLDOMNamedNodeMap* attributes{};
    if (FAILED(node->get_attributes(&attributes)) || !attributes) return {};
    IXMLDOMNode* attribute{};
    attributes->getNamedItem(_bstr_t(name), &attribute);
    attributes->Release();
    if (!attribute) return {};
    VARIANT value{};
    VariantInit(&value);
    attribute->get_nodeValue(&value);
    attribute->Release();
    const _variant_t holder(value, false);
    if (holder.vt == VT_EMPTY || holder.vt == VT_NULL) return {};
    return static_cast<const wchar_t*>(_bstr_t(holder));
}

bool XmlInteger(IXMLDOMNode* node, const wchar_t* name, int& value) {
    const auto text = XmlAttribute(node, name);
    if (text.empty()) return false;
    wchar_t* end{};
    const long parsed = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str()) return false;
    value = static_cast<int>(parsed);
    return true;
}

bool XmlColor(IXMLDOMNode* node, const wchar_t* name, COLORREF& value) {
    const auto text = XmlAttribute(node, name);
    unsigned int red{}, green{}, blue{};
    // FUN_0048DD6C accepts the three two-digit components and deliberately
    // ignores trailing text.  Visual presets use that same parser.
    if (swscanf_s(text.c_str(), L"#%2x%2x%2x", &red, &green, &blue) != 3)
        return false;
    value = RGB(red, green, blue);
    return true;
}

bool XmlFont(IXMLDOMNode* node, LOGFONTW& result) {
    const auto text = XmlAttribute(node, L"Font");
    if (text.empty()) return false;
    std::wistringstream input(text);
    long values[5]{};
    int bytes[8]{};
    wchar_t comma{};
    for (auto& value : values) {
        if (!(input >> value) || !(input >> comma) || comma != L',')
            return false;
    }
    for (auto& value : bytes) {
        if (!(input >> value) || !(input >> comma) || comma != L',')
            return false;
    }
    std::wstring face;
    std::getline(input, face);
    result = {};
    result.lfHeight = values[0];
    result.lfWidth = values[1];
    result.lfEscapement = values[2];
    result.lfOrientation = values[3];
    result.lfWeight = values[4];
    result.lfItalic = static_cast<BYTE>(bytes[0]);
    result.lfUnderline = static_cast<BYTE>(bytes[1]);
    result.lfStrikeOut = static_cast<BYTE>(bytes[2]);
    result.lfCharSet = static_cast<BYTE>(bytes[3]);
    result.lfOutPrecision = static_cast<BYTE>(bytes[4]);
    result.lfClipPrecision = static_cast<BYTE>(bytes[5]);
    result.lfQuality = static_cast<BYTE>(bytes[6]);
    result.lfPitchAndFamily = static_cast<BYTE>(bytes[7]);
    wcsncpy_s(result.lfFaceName, face.c_str(), _TRUNCATE);
    return true;
}

std::wstring ColorText(COLORREF color) {
    wchar_t text[16]{};
    swprintf_s(text, L"#%02x%02x%02x", GetRValue(color), GetGValue(color),
               GetBValue(color));
    return text;
}

std::wstring FontText(const LOGFONTW& font) {
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

void SetXmlAttribute(IXMLDOMElement* element, const wchar_t* name,
                     const std::wstring& value) {
    if (element)
        element->setAttribute(_bstr_t(name), _variant_t(value.c_str()));
}

void SetXmlAttribute(IXMLDOMElement* element, const wchar_t* name, int value) {
    if (element) element->setAttribute(_bstr_t(name), _variant_t(value));
}

bool LoadVisualProfile(const std::filesystem::path& path,
                       settings::VisualSettings& visual) {
    IXMLDOMDocument2* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&document))) || !document)
        return false;
    document->put_async(VARIANT_FALSE);
    VARIANT_BOOL loaded{};
    document->load(_variant_t(path.wstring().c_str()), &loaded);
    if (loaded != VARIANT_TRUE) {
        document->Release();
        return false;
    }
    IXMLDOMNode* node{};
    document->selectSingleNode(_bstr_t(L"/ttplayer_visual/Visual"), &node);
    // Accept the rebuild's settings/profile spelling too.  The writer below
    // always emits the stock ttplayer_visual root.
    if (!node)
        document->selectSingleNode(_bstr_t(L"/ttplayer/Visual"), &node);
    if (!node) {
        document->Release();
        return false;
    }

    int integer{};
    if (XmlInteger(node, L"Type", integer))
        visual.type = std::clamp(integer, 0, 4);
    XmlColor(node, L"SpectrumTopColor", visual.spectrum_top_color);
    XmlColor(node, L"SpectrumBtmColor", visual.spectrum_bottom_color);
    XmlColor(node, L"SpectrumMidColor", visual.spectrum_middle_color);
    XmlColor(node, L"SpectrumPeakColor", visual.spectrum_peak_color);
    if (XmlInteger(node, L"SpectrumWide", integer))
        visual.spectrum_wide = std::max(0, integer);
    if (XmlInteger(node, L"BlurSpeed", integer))
        visual.blur_speed = std::clamp(integer, 0, 255);
    if (XmlInteger(node, L"Blur", integer)) visual.blur = integer != 0;
    XmlColor(node, L"BlurScopeColor", visual.blur_scope_color);
    XmlColor(node, L"TextColor", visual.text_color);
    LOGFONTW font = visual.font;
    if (XmlFont(node, font)) {
        visual.font = font;
        visual.font_valid = true;
    }
    node->Release();
    document->Release();
    return true;
}

bool SaveVisualProfile(const std::filesystem::path& path,
                       const settings::VisualSettings& visual) {
    IXMLDOMDocument2* document{};
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&document))) || !document)
        return false;
    document->put_async(VARIANT_FALSE);
    document->put_preserveWhiteSpace(VARIANT_TRUE);
    IXMLDOMProcessingInstruction* declaration{};
    document->createProcessingInstruction(
        _bstr_t(L"xml"),
        _bstr_t(L"version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\""),
        &declaration);
    if (declaration) {
        IXMLDOMNode* appended{};
        document->appendChild(declaration, &appended);
        if (appended) appended->Release();
        declaration->Release();
    }
    IXMLDOMElement* root{};
    IXMLDOMElement* element{};
    if (FAILED(document->createElement(_bstr_t(L"ttplayer_visual"), &root)) ||
        !root ||
        FAILED(document->createElement(_bstr_t(L"Visual"), &element)) ||
        !element) {
        if (element) element->Release();
        if (root) root->Release();
        document->Release();
        return false;
    }
    IXMLDOMNode* appended{};
    document->appendChild(root, &appended);
    if (appended) appended->Release();
    root->appendChild(element, &appended);
    if (appended) appended->Release();

    SetXmlAttribute(element, L"SpectrumTopColor",
                    ColorText(visual.spectrum_top_color));
    SetXmlAttribute(element, L"SpectrumBtmColor",
                    ColorText(visual.spectrum_bottom_color));
    SetXmlAttribute(element, L"SpectrumMidColor",
                    ColorText(visual.spectrum_middle_color));
    SetXmlAttribute(element, L"SpectrumPeakColor",
                    ColorText(visual.spectrum_peak_color));
    SetXmlAttribute(element, L"SpectrumWide", visual.spectrum_wide);
    SetXmlAttribute(element, L"BlurSpeed", visual.blur_speed);
    SetXmlAttribute(element, L"Blur", visual.blur ? 1 : 0);
    SetXmlAttribute(element, L"Type", std::clamp(visual.type, 0, 4));
    SetXmlAttribute(element, L"BlurScopeColor",
                    ColorText(visual.blur_scope_color));
    SetXmlAttribute(element, L"TextColor", ColorText(visual.text_color));
    if (visual.font_valid)
        SetXmlAttribute(element, L"Font", FontText(visual.font));

    const HRESULT result = document->save(_variant_t(path.wstring().c_str()));
    element->Release();
    root->Release();
    document->Release();
    return SUCCEEDED(result);
}

void PopulateCombo(HWND dialog, UINT control, HMODULE resources,
                   size_t count, int selection) {
    const HWND combo = GetDlgItem(dialog, static_cast<int>(control));
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t index = 0; index < count; ++index) {
        const auto item = ResourceListItem(resources, control, index);
        if (!item.empty())
            SendMessageW(combo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(item.c_str()));
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
}

void SetFramesText(HWND dialog, HMODULE resources, int frames) {
    const auto format = LoadResourceText(resources, 0x813d);
    if (format.empty()) return;
    wchar_t text[64]{};
    swprintf_s(text, format.c_str(), frames);
    SetDlgItemTextW(dialog, kVisualFramesText, text);
}

void SyncVisualPage(HWND dialog, HMODULE resources,
                    const settings::VisualSettings& visual, bool first) {
    PopulateCombo(dialog, kVisualType, resources, 5,
                  std::clamp(visual.type, 0, 4));
    PopulateCombo(dialog, kScopeSpeed, resources, 3,
                  std::clamp((visual.blur_speed - 1) / 2, 0, 2));
    CheckDlgButton(dialog, kSpectrumWide,
                   visual.spectrum_wide ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, kScopeBlur,
                   visual.blur ? BST_CHECKED : BST_UNCHECKED);
    const HWND track = GetDlgItem(dialog, kVisualFrames);
    if (track) {
        SendMessageW(track, TBM_SETRANGE, TRUE, MAKELPARAM(10, 40));
        SendMessageW(track, TBM_SETTICFREQ, 1, 0);
        SendMessageW(track, TBM_SETPOS, TRUE, visual.frames_per_second);
    }
    SetFramesText(dialog, resources, visual.frames_per_second);
    for (const UINT control : kColorControls) {
        const HWND button = GetDlgItem(dialog, static_cast<int>(control));
        if (first && button) {
            const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
            SetWindowLongPtrW(button, GWL_STYLE,
                (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | BS_OWNERDRAW);
        }
        if (button) InvalidateRect(button, nullptr, TRUE);
    }
}

bool SelectVisualProfileFile(HWND owner, HMODULE resources, bool save,
                             std::filesystem::path& path) {
    static_cast<void>(resources);
    static std::filesystem::path last_path;
    // FUN_00493BAF supplies this literal filter to the stock file dialog;
    // the menu captions themselves still come from ttpres MENU 0x9e.
    constexpr wchar_t filter[] =
        L"Vis Profile (*.ttvi_cfg)\0*.ttvi_cfg\0\0";
    const auto filters = ParseLegacyDialogFilter(filter);
    std::optional<std::filesystem::path> selected;
    if (save) {
        ModernSaveFileOptions options;
        options.owner = owner;
        options.filters = filters;
        options.initial_path = last_path;
        options.default_extension = L"ttvi_cfg";
        selected = ModernSaveFile(options);
    } else {
        ModernOpenFileOptions options;
        options.owner = owner;
        options.filters = filters;
        options.initial_path = last_path;
        options.default_extension = L"ttvi_cfg";
        selected = ModernOpenFile(options);
    }
    if (!selected) return false;
    last_path = std::move(*selected);
    path = last_path;
    return true;
}

LRESULT CALLBACK VisualOptionsSheetProc(
    HWND sheet, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass, DWORD_PTR data) {
    if (message == WM_COMMAND &&
        (LOWORD(wparam) == kSaveAllOptions ||
         LOWORD(wparam) == kResetAllOptions)) {
        return SendMessageW(reinterpret_cast<HWND>(data), message, wparam,
                            lparam);
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(sheet, VisualOptionsSheetProc, subclass);
    return DefSubclassProc(sheet, message, wparam, lparam);
}

void InstallPropertySheetButtons(HWND page, HMODULE resources) {
    const HWND sheet = GetParent(page);
    const HWND close = sheet ? GetDlgItem(sheet, IDOK) : nullptr;
    if (!sheet || !close) return;

    // A property page can receive WM_INITDIALOG more than once while a sheet
    // is being rebuilt.  Do not duplicate the two outer-shell buttons or the
    // subclass in that case.
    if (GetDlgItem(sheet, kSaveAllOptions)) return;

    RECT close_position{};
    GetWindowRect(close, &close_position);
    MapWindowPoints(HWND_DESKTOP, sheet,
                    reinterpret_cast<POINT*>(&close_position), 2);
    int right_edge = close_position.right;
    for (const int identifier : {IDCANCEL, kPropertySheetApply}) {
        const HWND button = GetDlgItem(sheet, identifier);
        RECT bounds{};
        if (button && GetWindowRect(button, &bounds)) {
            MapWindowPoints(HWND_DESKTOP, sheet,
                            reinterpret_cast<POINT*>(&bounds), 2);
            right_edge = std::max(right_edge, static_cast<int>(bounds.right));
        }
    }

    ShowWindow(GetDlgItem(sheet, IDCANCEL), SW_HIDE);
    ShowWindow(GetDlgItem(sheet, kPropertySheetApply), SW_HIDE);
    ShowWindow(GetDlgItem(sheet, IDHELP), SW_HIDE);
    const auto close_text = LoadResourceText(resources, 8);
    if (!close_text.empty()) SetWindowTextW(close, close_text.c_str());

    RECT client{};
    GetClientRect(sheet, &client);
    const int width = close_position.right - close_position.left;
    const int height = close_position.bottom - close_position.top;
    const int gap = 6;
    // The stock sheet reserves three equal button slots.  The original
    // custom shell uses them, from left to right, for Save All, Reset All
    // and Close (runtime IDs 0x4D2, 0x4D3 and 1).
    const int right_margin = std::max(
        7, static_cast<int>(client.right) - right_edge);
    const int close_x = client.right - right_margin - width;
    SetWindowPos(close, nullptr, close_x, close_position.top, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER);

    const HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(close, WM_GETFONT, 0, 0));
    const auto make_button = [&](UINT identifier, UINT text_identifier,
                                 int x) {
        const auto text = LoadResourceText(resources, text_identifier);
        const HWND button = CreateWindowExW(
            0, L"BUTTON", text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, close_position.top, width, height, sheet,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
            reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(sheet, GWLP_HINSTANCE)), nullptr);
        if (button && font) SendMessageW(button, WM_SETFONT,
                                         reinterpret_cast<WPARAM>(font), TRUE);
    };
    const int reset_x = close_x - gap - width;
    make_button(kResetAllOptions, 0x8140, reset_x);
    make_button(kSaveAllOptions, 0x8141, reset_x - gap - width);
    SetWindowSubclass(sheet, VisualOptionsSheetProc,
                      kVisualOptionsSheetSubclass,
                      reinterpret_cast<DWORD_PTR>(page));
}

} // namespace

INT_PTR CALLBACK PlayerWindow::VisualOptionsDialogProc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    PlayerWindow* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        const auto* page = reinterpret_cast<const PROPSHEETPAGEW*>(lparam);
        self = page ? reinterpret_cast<PlayerWindow*>(page->lParam) : nullptr;
        SetWindowLongPtrW(dialog, DWLP_USER,
                          reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleVisualOptionsDialog(
                      dialog, message, wparam, lparam)
                : FALSE;
}

INT_PTR PlayerWindow::HandleVisualOptionsDialog(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    const HMODULE resources = ResourceModule();
    const auto refresh = [this] {
        if (fullscreen_mode_ == 3) UpdateFullScreenLayout();
        UpdateVisualWindowLayout();
        UpdateVisualFrame();
    };

    switch (message) {
    case WM_INITDIALOG: {
        const auto description = ResourceText(kVisualOptionsPage);
        if (!description.empty()) SetWindowTextW(dialog, description.c_str());
        SyncVisualPage(dialog, resources, settings_.visual, true);
        InstallButtonBitmap(dialog, kVisualFont, 0x161);
        InstallButtonBitmap(dialog, kVisualProfile, 0x160);
        return TRUE;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) ==
                GetDlgItem(dialog, kVisualFrames)) {
            settings_.visual.frames_per_second = static_cast<int>(
                SendDlgItemMessageW(dialog, kVisualFrames,
                                    TBM_GETPOS, 0, 0));
            SetFramesText(dialog, resources,
                          settings_.visual.frames_per_second);
            refresh();
            return TRUE;
        }
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        const COLORREF* color = item
            ? VisualColor(settings_.visual, item->CtlID) : nullptr;
        if (!item || !color) break;
        DrawColorButton(*item, *color);
        return TRUE;
    }
    case WM_COMMAND: {
        const UINT control = LOWORD(wparam);
        const UINT notification = HIWORD(wparam);
        if (control == kVisualType && notification == CBN_SELCHANGE) {
            const int type = static_cast<int>(SendDlgItemMessageW(
                dialog, kVisualType, CB_GETCURSEL, 0, 0));
            if (type != CB_ERR) SetVisualType(type);
            return TRUE;
        }
        if (control == kScopeSpeed && notification == CBN_SELCHANGE) {
            const int speed = static_cast<int>(SendDlgItemMessageW(
                dialog, kScopeSpeed, CB_GETCURSEL, 0, 0));
            if (speed != CB_ERR) settings_.visual.blur_speed = speed * 2 + 1;
            refresh();
            return TRUE;
        }
        if (control == kSpectrumWide && notification == BN_CLICKED) {
            settings_.visual.spectrum_wide =
                IsDlgButtonChecked(dialog, kSpectrumWide) == BST_CHECKED;
            refresh();
            return TRUE;
        }
        if (control == kScopeBlur && notification == BN_CLICKED) {
            settings_.visual.blur =
                IsDlgButtonChecked(dialog, kScopeBlur) == BST_CHECKED;
            refresh();
            return TRUE;
        }
        if (VisualColor(settings_.visual, control) &&
            notification == BN_CLICKED) {
            COLORREF* target = VisualColor(settings_.visual, control);
            ShowLegacyPresetColor(
                dialog, GetDlgItem(dialog, static_cast<int>(control)),
                resources, *target,
                [this, dialog, control, target](COLORREF selected) {
                if (!IsWindow(dialog)) return;
                *target = selected;
                InvalidateRect(GetDlgItem(dialog, static_cast<int>(control)),
                               nullptr, TRUE);
                if (fullscreen_mode_ == 3) UpdateFullScreenLayout();
                UpdateVisualWindowLayout();
                UpdateVisualFrame();
            });
            return TRUE;
        }
        if (control == kVisualFont && notification == BN_CLICKED) {
            LOGFONTW font = settings_.visual.font;
            if (!settings_.visual.font_valid) {
                const HFONT current = reinterpret_cast<HFONT>(
                    SendMessageW(GetDlgItem(dialog, kVisualFont), WM_GETFONT,
                                 0, 0));
                if (current) GetObjectW(current, sizeof(font), &font);
            }
            CHOOSEFONTW chooser{sizeof(chooser)};
            chooser.hwndOwner = dialog;
            chooser.lpLogFont = &font;
            chooser.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT |
                            CF_NOVERTFONTS;
            if (ChooseFontW(&chooser)) {
                font.lfQuality = ANTIALIASED_QUALITY;
                settings_.visual.font = font;
                settings_.visual.font_valid = true;
                refresh();
            }
            return TRUE;
        }
        if (control == kVisualProfile && notification == BN_CLICKED) {
            const HWND button = GetDlgItem(dialog, kVisualProfile);
            RECT bounds{};
            GetWindowRect(button, &bounds);
            const HMENU menu = LoadMenuW(
                resources, MAKEINTRESOURCEW(kVisualProfileMenu));
            const HMENU popup = menu ? GetSubMenu(menu, 0) : nullptr;
            const UINT selected = popup ? TrackPopupMenu(
                popup, TPM_RIGHTBUTTON | TPM_RETURNCMD, bounds.left,
                bounds.bottom, 0, dialog, nullptr) : 0;
            if (menu) DestroyMenu(menu);
            std::filesystem::path path;
            if (selected == kLoadVisualProfile &&
                SelectVisualProfileFile(dialog, resources, false, path)) {
                auto loaded = settings_.visual;
                if (LoadVisualProfile(path, loaded)) {
                    // FUN_0048E0DC's preset object deliberately has no FPS
                    // member.  Preserve the live frame rate while applying
                    // all remaining fields immediately.
                    const int frames = settings_.visual.frames_per_second;
                    settings_.visual = loaded;
                    settings_.visual.frames_per_second = frames;
                    SetVisualType(settings_.visual.type);
                    SyncVisualPage(dialog, resources, settings_.visual, false);
                }
            } else if (selected == kSaveVisualProfile &&
                       SelectVisualProfileFile(dialog, resources, true, path)) {
                static_cast<void>(SaveVisualProfile(path, settings_.visual));
            }
            return TRUE;
        }
        if (control == kSaveAllOptions && notification == BN_CLICKED) {
            CaptureWindowState();
            settings::SaveWindowState(settings_.source_path, settings_);
            return TRUE;
        }
        if (control == kResetAllOptions && notification == BN_CLICKED) {
            const auto prompt = ResourceText(0x8143);
            auto caption = ResourceText(0x80);
            if (caption.empty()) caption = display_title_;
            if (MessageBoxW(dialog, prompt.c_str(), caption.c_str(),
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                // The original button recreates the complete Settings object
                // and then reopens the same page.  This rebuild exposes only
                // page 4 here, so reset the corresponding recovered subset
                // and leave unrelated settings untouched.
                settings_.visual = settings::VisualSettings{};
                SetVisualType(settings_.visual.type);
                SyncVisualPage(dialog, resources, settings_.visual, false);
            }
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header && header->code == PSN_APPLY) {
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        // The stock window hides Cancel, but Escape/system-close can still
        // produce PSN_RESET.  Original FUN_00493xxx has already changed the
        // globals, so closing never restores an entry snapshot.
        if (header && header->code == PSN_RESET) return TRUE;
        break;
    }
    case WM_DESTROY:
        DestroyButtonBitmap(dialog, kVisualFont);
        DestroyButtonBitmap(dialog, kVisualProfile);
        break;
    default:
        break;
    }
    return FALSE;
}

} // namespace ttplayer::ui
