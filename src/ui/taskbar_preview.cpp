#include "ttplayer/ui/taskbar_preview.h"
#include "album_background.h"

#include <algorithm>
#include <cstdint>
#include <dwmapi.h>
#include <string>

namespace ttplayer::ui {
namespace {
SIZE Fit(SIZE source, SIZE limit) {
    if (source.cx <= 0 || source.cy <= 0 || limit.cx <= 0 || limit.cy <= 0) return {};
    if (static_cast<int64_t>(source.cx) * limit.cy > static_cast<int64_t>(source.cy) * limit.cx)
        return {limit.cx, std::max<LONG>(1, static_cast<LONG>(static_cast<int64_t>(source.cy) * limit.cx / source.cx))};
    return {std::max<LONG>(1, static_cast<LONG>(static_cast<int64_t>(source.cx) * limit.cy / source.cy)), limit.cy};
}

HBITMAP Render(HBITMAP cover, SIZE source, SIZE size) {
    if (!cover || size.cx <= 0 || size.cy <= 0) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.cx; info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels{};
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC from = CreateCompatibleDC(nullptr), to = CreateCompatibleDC(nullptr);
    bool success = false;
    if (bitmap && pixels && from && to) {
        const auto old_from = SelectObject(from, cover), old_to = SelectObject(to, bitmap);
        const size_t count = static_cast<size_t>(size.cx) * size.cy;
        std::fill_n(static_cast<DWORD*>(pixels), count, 0U);
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        success = AlphaBlend(to, 0, 0, size.cx, size.cy, from, 0, 0, source.cx, source.cy, blend) != FALSE;
        GdiFlush();
        SelectObject(from, old_from); SelectObject(to, old_to);
    }
    if (from) DeleteDC(from);
    if (to) DeleteDC(to);
    if (!success && bitmap) { DeleteObject(bitmap); bitmap = nullptr; }
    return bitmap;
}

HBITMAP RenderWindow(HWND window, SIZE size, POINT offset, HRGN window_region) {
    // Peek replaces the window on the desktop, so it must show the window's
    // own client content, not a thumbnail enlarged to the client dimensions.
    // Paint into an off-screen DC; reading the screen would capture occluding
    // windows or the preceding Peek image and fails when minimized.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.cx; info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels{};
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!bitmap || !pixels || !dc) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return nullptr;
    }
    const auto previous = SelectObject(dc, bitmap);
    const size_t count = static_cast<size_t>(size.cx) * size.cy;
    std::fill_n(static_cast<DWORD*>(pixels), count, 0U);
    const int saved = SaveDC(dc);
    // DefWindowProc(WM_PRINT) substitutes the application icon when iconic.
    // Ask the client renderer directly, then compose visible direct children
    // in Z order. Child WM_PRINT includes each child's non-client area and
    // descendants; owned playlist/lyrics windows remain separate DWM windows.
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    if (saved) RestoreDC(dc, saved);
    for (HWND child = GetWindow(GetWindow(window, GW_CHILD), GW_HWNDLAST);
         child; child = GetWindow(child, GW_HWNDPREV)) {
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT bounds{};
        if (!GetWindowRect(child, &bounds)) continue;
        MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&bounds), 2);
        const int child_saved = SaveDC(dc);
        if (!child_saved) continue;
        IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
        SetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                     PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        RestoreDC(dc, child_saved);
    }
    GdiFlush();
    auto* data = static_cast<DWORD*>(pixels);
    for (size_t i = 0; i < count; ++i) data[i] |= 0xff000000U;
    // A skin can have a shaped window. Do not turn its cut-outs into black
    // rectangles when DWM replaces the native surface with this bitmap.
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (region && window_region && CombineRgn(region, window_region, nullptr, RGN_COPY) != ERROR) {
        OffsetRgn(region, -offset.x, -offset.y);
        ExtSelectClipRgn(dc, region, RGN_DIFF);
        PatBlt(dc, 0, 0, size.cx, size.cy, BLACKNESS);
        GdiFlush();
    }
    if (region) DeleteObject(region);
    SelectObject(dc, previous);
    DeleteDC(dc);
    return bitmap;
}
} // namespace

TaskbarPreview::Api TaskbarPreview::NativeApi() {
    static const Api api = [] {
        wchar_t directory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
        if (!length || length >= MAX_PATH) return Api{};
        // Process-lifetime module, loaded from the system directory (XP has
        // no dwmapi.dll). Never add static Win7-only DWM imports.
        const HMODULE module = LoadLibraryW((std::wstring(directory) + L"\\dwmapi.dll").c_str());
        if (!module) return Api{};
        Api result{};
        result.composition = reinterpret_cast<decltype(result.composition)>(GetProcAddress(module, "DwmIsCompositionEnabled"));
        result.attribute = reinterpret_cast<decltype(result.attribute)>(GetProcAddress(module, "DwmSetWindowAttribute"));
        result.invalidate = reinterpret_cast<decltype(result.invalidate)>(GetProcAddress(module, "DwmInvalidateIconicBitmaps"));
        result.thumbnail = reinterpret_cast<decltype(result.thumbnail)>(GetProcAddress(module, "DwmSetIconicThumbnail"));
        result.live_preview = reinterpret_cast<decltype(result.live_preview)>(GetProcAddress(module, "DwmSetIconicLivePreviewBitmap"));
        return result;
    }();
    return api;
}

TaskbarPreview::~TaskbarPreview() { Reset(); }

bool TaskbarPreview::Available() const {
    return api_.composition && api_.attribute && api_.invalidate && api_.thumbnail && api_.live_preview;
}

void TaskbarPreview::Disable() {
    if (enabled_ && window_ && IsWindow(window_) && Available()) {
        const BOOL value = FALSE;
        api_.attribute(window_, DWMWA_FORCE_ICONIC_REPRESENTATION, &value, sizeof(value));
        api_.attribute(window_, DWMWA_HAS_ICONIC_BITMAP, &value, sizeof(value));
        api_.invalidate(window_);
    }
    enabled_ = false;
}

void TaskbarPreview::Clear() {
    Disable();
    if (cover_) DeleteObject(cover_);
    cover_ = nullptr; cover_size_ = {};
}

void TaskbarPreview::Reset() {
    Clear(); window_ = nullptr;
    if (window_region_) DeleteObject(window_region_);
    window_region_ = nullptr;
    client_size_ = {}; client_offset_ = {}; monitor_ = nullptr; minimized_ = false;
}

void TaskbarPreview::SetSource(HWND window, const std::filesystem::path& path,
                               const audio::AudioMetadata& metadata) {
    if (window_ != window) Reset();
    window_ = window;
    SIZE size{};
    HBITMAP cover = window && Available() && !path.empty()
        ? detail::LoadTaskbarCoverBitmap(path, metadata, &size) : nullptr;
    // Replace an album in one transaction. Keep the DWM attributes enabled
    // between two covers so no native-window-sized frame enters its cache.
    if (cover_) DeleteObject(cover_);
    cover_ = cover; cover_size_ = size;
    if (cover_) Refresh(window, true);
    else Disable();
}

bool TaskbarPreview::UpdateGeometry() {
    const bool minimized = IsIconic(window_) != FALSE;
    const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    SIZE size = client_size_;
    POINT offset = client_offset_;
    bool region_changed = false;
    if (!minimized) {
        // Win7 replaces an iconic HWND's region with the small icon/caption
        // rectangle. Keep the real skin shape along with the real geometry.
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        if (region) {
            if (GetWindowRgn(window_, region) == ERROR) { DeleteObject(region); region = nullptr; }
            region_changed = (region == nullptr) != (window_region_ == nullptr) ||
                (region && window_region_ && !EqualRgn(region, window_region_));
            if (window_region_) DeleteObject(window_region_);
            window_region_ = region;
        }
        RECT client{}, frame{};
        POINT origin{};
        if (GetClientRect(window_, &client) && client.right > 0 && client.bottom > 0 &&
            GetWindowRect(window_, &frame) && ClientToScreen(window_, &origin)) {
            size = {client.right, client.bottom};
            offset = {origin.x - frame.left, origin.y - frame.top};
        }
    } else if (size.cx <= 0 || size.cy <= 0) {
        // Only reconstruct when no real client geometry has been observed.
        // rcNormalPosition is NOT the preceding maximized client rectangle.
        WINDOWPLACEMENT placement{sizeof(placement)};
        RECT border{};
        if (GetWindowPlacement(window_, &placement) &&
            AdjustWindowRectEx(&border, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)) & ~WS_MINIMIZE,
                GetMenu(window_) != nullptr, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)))) {
            size = {placement.rcNormalPosition.right - placement.rcNormalPosition.left - (border.right - border.left),
                    placement.rcNormalPosition.bottom - placement.rcNormalPosition.top - (border.bottom - border.top)};
            offset = {-border.left, -border.top};
        }
    }
    const bool changed = size.cx != client_size_.cx || size.cy != client_size_.cy ||
        offset.x != client_offset_.x || offset.y != client_offset_.y ||
        minimized != minimized_ || monitor != monitor_ || region_changed;
    client_size_ = size; client_offset_ = offset; minimized_ = minimized; monitor_ = monitor;
    return changed;
}

void TaskbarPreview::Refresh(HWND window, bool force) {
    if (window_ != window) {
        Disable(); window_ = window; client_size_ = {}; client_offset_ = {};
        if (window_region_) DeleteObject(window_region_);
        window_region_ = nullptr;
    }
    if (!window_ || !Available()) return;
    const bool geometry_changed = UpdateGeometry();
    BOOL composed{};
    if (!cover_ || FAILED(api_.composition(&composed)) || !composed) {
        Disable();
        return;
    }
    if (enabled_ && !force) {
        if (geometry_changed) api_.invalidate(window_);
        return;
    }
    const BOOL value = TRUE;
    // Mark before calls so partial failure also rolls back both attributes.
    enabled_ = true;
    if (FAILED(api_.attribute(window_, DWMWA_FORCE_ICONIC_REPRESENTATION, &value, sizeof(value))) ||
        FAILED(api_.attribute(window_, DWMWA_HAS_ICONIC_BITMAP, &value, sizeof(value)))) {
        Disable();
        return;
    }
    api_.invalidate(window_);
}

bool TaskbarPreview::HandleMessage(UINT message, LPARAM lparam) {
    if (!enabled_ || !cover_ || !window_) return false;
    const bool live = message == WM_DWMSENDICONICLIVEPREVIEWBITMAP;
    if (!live && message != WM_DWMSENDICONICTHUMBNAIL) return false;
    SIZE limit{};
    POINT offset{};
    if (live) {
        UpdateGeometry();
        limit = client_size_;
        offset = client_offset_;
    } else {
        limit = {HIWORD(lparam), LOWORD(lparam)};
    }
    if (limit.cx <= 0 || limit.cy <= 0) return false;
    // Never crop/resize a full-scale Peek bitmap. If the window exceeds our
    // allocation bound, give the native representation back to DWM instead.
    if (live && (limit.cx > 4096 || limit.cy > 4096)) { Disable(); return false; }
    limit.cx = std::min<LONG>(4096, limit.cx); limit.cy = std::min<LONG>(4096, limit.cy);
    // WM_DWMSENDICONICTHUMBNAIL supplies maximum bounds, not a required
    // canvas. Submit only the fitted artwork; no opaque letterbox padding.
    HBITMAP bitmap = live ? RenderWindow(window_, limit, offset, window_region_)
                          : Render(cover_, cover_size_, Fit(cover_size_, limit));
    if (!bitmap) { Disable(); return false; }
    const HRESULT status = live ? api_.live_preview(window_, bitmap, &offset, 0)
                                : api_.thumbnail(window_, bitmap, 0);
    DeleteObject(bitmap); // DWM has copied the pixels before returning.
    if (FAILED(status)) { Disable(); return false; }
    return true;
}
} // namespace ttplayer::ui
