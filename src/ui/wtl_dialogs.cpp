#include "ttplayer/ui/wtl_runtime.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include <atldlgs.h>
#include <memory>
#include <vector>

namespace ttplayer::ui {
namespace {
BOOL DispatchDialog(DLGPROC handler, HWND window, UINT message, WPARAM wp,
                    LPARAM lp, LRESULT& result) {
    const INT_PTR handled = handler(window, message, wp, lp);
    result = handled;
    if (message == WM_NCDESTROY || !handled) return FALSE;
    switch (message) {
    case WM_COMPAREITEM: case WM_VKEYTOITEM: case WM_CHARTOITEM:
    case WM_INITDIALOG: case WM_QUERYDRAGICON:
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC: break;
    default:
        result = ::IsWindow(window) ? ::GetWindowLongPtrW(window, DWLP_MSGRESULT) : 0;
        break;
    }
    return TRUE;
}

class ResourceDialog final : public ATL::CDialogImpl<ResourceDialog> {
public:
    enum { IDD = 0 };
    DLGPROC handler{};
    bool creating{true}, started{};
    BOOL ProcessWindowMessage(HWND window, UINT message, WPARAM wp, LPARAM lp,
                              LRESULT& result, DWORD = 0) override {
        started = true;
        return DispatchDialog(handler, window, message, wp, lp, result);
    }
    HWND CreateResource(HINSTANCE resources, LPCWSTR name, HWND parent, LPARAM data) {
        if (!m_thunk.Init(nullptr, nullptr)) return nullptr;
        ATL::_AtlWinModule.AddCreateWndData(&m_thunk.cd,
            static_cast<ATL::CDialogImplBase*>(this));
        // Unlike CDialogImpl::Create, preserve the caller's resource module.
        const HWND window = ::CreateDialogParamW(resources, name, parent, StartDialogProc, data);
        if (!started) ATL::_AtlWinModule.ExtractCreateWndData();
        return window;
    }
    void OnFinalMessage(HWND) override { if (!creating) delete this; }
};

class ResourcePage final : public WTL::CPropertyPageImpl<ResourcePage> {
public:
    enum { IDD = 0 };
    PROPSHEETPAGEW original;
    explicit ResourcePage(const PROPSHEETPAGEW& page) : original(page) {
        const auto callback = m_psp.pfnCallback;
        const auto procedure = m_psp.pfnDlgProc;
        m_psp = page;
        m_psp.dwFlags |= PSP_USECALLBACK;
        m_psp.pfnCallback = callback;
        m_psp.pfnDlgProc = procedure;
        m_psp.lParam = reinterpret_cast<LPARAM>(this);
    }
    BOOL ProcessWindowMessage(HWND window, UINT message, WPARAM wp, LPARAM lp,
                              LRESULT& result, DWORD = 0) override {
        // SDK dialog callbacks return BOOL and write notification results into
        // DWLP_MSGRESULT; ATL handlers return that result separately.
        return DispatchDialog(original.pfnDlgProc, window, message, wp,
            message == WM_INITDIALOG ? reinterpret_cast<LPARAM>(&original) : lp, result);
    }
};

class ResourceSheet final : public WTL::CPropertySheetImpl<ResourceSheet> {
public:
    std::vector<std::unique_ptr<ResourcePage>> pages;
    PFNPROPSHEETCALLBACK callback{};
    bool creating{true}, modeless{};
    explicit ResourceSheet(const PROPSHEETHEADERW& header) {
        callback = (header.dwFlags & PSH_USECALLBACK) ? header.pfnCallback : nullptr;
        m_psh = header;
        m_psh.dwFlags = (header.dwFlags & ~PSH_PROPSHEETPAGE) | PSH_USECALLBACK;
        m_psh.pfnCallback = PropSheetCallback;
        modeless = (header.dwFlags & PSH_MODELESS) != 0;
    }
    ~ResourceSheet() {
        // On partial construction the native page handles must die before
        // their C++ page objects; the base destructor otherwise runs last.
        for (int i = 0; i < m_arrPages.GetSize(); ++i)
            ::DestroyPropertySheetPage(m_arrPages[i]);
        m_arrPages.RemoveAll();
    }
    bool AddResourcePages(const PROPSHEETHEADERW& header) {
        for (UINT i = 0; i < header.nPages; ++i) {
            auto page = std::make_unique<ResourcePage>(header.ppsp[i]);
            const auto handle = page->Create();
            if (!handle) return false;
            if (!AddPage(handle)) { ::DestroyPropertySheetPage(handle); return false; }
            pages.push_back(std::move(page));
        }
        return true;
    }
    void OnSheetInitialized() {
        if (callback) callback(m_hWnd, PSCB_INITIALIZED, 0);
    }
    void OnFinalMessage(HWND) override {
        if (modeless && !creating) delete this;
    }
    BEGIN_MSG_MAP(ResourceSheet)
    END_MSG_MAP()
};
}

HWND CreateWtlDialog(HINSTANCE resources, LPCWSTR name, HWND parent,
                     DLGPROC handler, LPARAM data) {
    if (!handler || FAILED(EnsureWtlRuntime())) return nullptr;
    auto dialog = std::make_unique<ResourceDialog>();
    dialog->handler = handler;
    const HWND window = dialog->CreateResource(resources, name, parent, data);
    dialog->creating = false;
    if (!window || !::IsWindow(window)) return nullptr;
    dialog.release();
    return window;
}

INT_PTR ShowWtlPropertySheet(const PROPSHEETHEADERW& header) {
    if (FAILED(EnsureWtlRuntime())) return -1;
    if (!(header.dwFlags & PSH_PROPSHEETPAGE) || !header.ppsp) return -1;
    auto sheet = std::make_unique<ResourceSheet>(header);
    if (!sheet->AddResourcePages(header)) return -1;
    if (!sheet->modeless) return sheet->DoModal(header.hwndParent);
    const HWND window = sheet->Create(header.hwndParent);
    sheet->creating = false;
    if (!window || window == reinterpret_cast<HWND>(-1)) return -1;
    sheet.release(); // ATL releases the modeless object at final destruction.
    return reinterpret_cast<INT_PTR>(window);
}
}
