#include "ttplayer/skin/skin_image.h"

#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

namespace ttplayer::skin {
namespace {
struct GraphicsRuntime {
    ULONG_PTR token{};
    GraphicsRuntime() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
            token = 0;
    }
    ~GraphicsRuntime() { if (token) Gdiplus::GdiplusShutdown(token); }
};
std::shared_ptr<GraphicsRuntime> AcquireGraphics() {
    // No shutdown while an image still exists (including copies across a
    // transactional skin rebind). Skin loading/drawing runs on the UI thread.
    static std::weak_ptr<GraphicsRuntime> cached;
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    auto runtime = cached.lock();
    if (!runtime) cached = runtime = std::make_shared<GraphicsRuntime>();
    return runtime;
}
}

struct SkinImage::Storage {
    std::shared_ptr<GraphicsRuntime> runtime;
    IStream* stream{};
    std::unique_ptr<Gdiplus::Bitmap> image;
    HBITMAP dib{};
    SIZE size{};
    // GetObject(DIBSECTION) normalizes biHeight to positive even when the
    // pixels still have top-down row order. Retain our creation-time layout.
    bool dib_top_down{};
    // An entry always represents a completed scan; nullptr is a legal BMP
    // rectangular fallback, never a cached allocation/read/GDI failure.
    struct RegionEntry { COLORREF key; int frames; HRGN region; COLORREF drawing_key; };
    std::vector<RegionEntry> regions;
    ~Storage() {
        for (const auto& entry : regions) DeleteObject(entry.region);
        image.reset(); // GDI+ may lazily read its stream until destruction.
        if (stream) stream->Release();
        if (dib) DeleteObject(dib);
    }
};

RECT NativeSkinIconBounds(HICON icon, RECT bounds) {
    ICONINFO info{};
    BITMAP bitmap{};
    if (icon && GetIconInfo(icon, &info)) {
        GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap);
        if (!info.hbmColor) bitmap.bmHeight /= 2;
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
    }
    bounds.right = bounds.left + bitmap.bmWidth;
    bounds.bottom = bounds.top + bitmap.bmHeight;
    return bounds;
}

void DrawNativeSkinIcon(HDC dc, HICON icon, const RECT& bounds) {
    ICONINFO info{};
    if (!icon || !GetIconInfo(icon, &info)) return;
    BITMAP bitmap{};
    GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap);
    if (!info.hbmColor) bitmap.bmHeight /= 2;
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    const int saved = SaveDC(dc);
    if (!saved) return;
    IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    DrawIconEx(dc, bounds.left + (bounds.right - bounds.left - bitmap.bmWidth) / 2,
        bounds.top + (bounds.bottom - bounds.top - bitmap.bmHeight) / 2,
        icon, bitmap.bmWidth, bitmap.bmHeight, 0, nullptr, DI_NORMAL);
    RestoreDC(dc, saved);
}

SkinImage SkinImage::Load(const std::filesystem::path& path, bool* replacement_started) {
    if (replacement_started) *replacement_started = false;
    SkinImage result;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return result;
    const auto length = input.tellg();
    // Bound allocation and decoded dimensions for malformed skin packages.
    if (length < 6 || length > 64 * 1024 * 1024) return result;
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), length)) return result;
    auto data = std::make_shared<Storage>();
    DWORD bmp_length{};
    std::memcpy(&bmp_length, bytes.data() + 2, sizeof(bmp_length));
    if (bytes[0] == 'B' && bytes[1] == 'M') {
        if (bmp_length != bytes.size() || bytes.size() < 14) return result;
        if (replacement_started) *replacement_started = true;
        if (bytes.size() < 54) return result;
        BITMAPINFOHEADER header{};
        std::memcpy(&header, bytes.data() + 14, sizeof(header));
        DWORD offset{};
        std::memcpy(&offset, bytes.data() + 10, sizeof(offset));
        // 00408CF4 accepts a BITMAPINFOHEADER/BI_RGB DIB. Validate lengths
        // before asking GDI to decode; never restore the original overreads.
        if (header.biSize != sizeof(header) || header.biCompression != BI_RGB ||
            header.biPlanes != 1 || header.biWidth <= 0 || header.biWidth > 16384 ||
            !header.biHeight || header.biHeight < -16384 || header.biHeight > 16384 ||
            (header.biBitCount != 1 && header.biBitCount != 4 && header.biBitCount != 8 &&
             header.biBitCount != 16 && header.biBitCount != 24 && header.biBitCount != 32)) return result;
        const uint64_t height = std::abs(header.biHeight);
        const uint64_t stride = ((static_cast<uint64_t>(header.biWidth) * header.biBitCount + 31) / 32) * 4;
        const uint64_t colors = header.biClrUsed ? header.biClrUsed :
            (header.biBitCount <= 8 ? 1ULL << header.biBitCount : 0);
        if (static_cast<uint64_t>(header.biWidth) * height > 32 * 1024 * 1024 ||
            (header.biBitCount <= 8 && colors > (1ULL << header.biBitCount)) ||
            offset < 54 + colors * 4 || offset > bytes.size() || stride * height > bytes.size() - offset)
            return result;
        // 00408CF4 copies the original DIB pixels. LoadImage may remap equal
        // palette colours to one index, changing the 8-bit button hit region.
        void* pixels{};
        data->dib = CreateDIBSection(nullptr,
            reinterpret_cast<const BITMAPINFO*>(bytes.data() + 14),
            DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!data->dib || !pixels) return result;
        std::memcpy(pixels, bytes.data() + offset, static_cast<size_t>(stride * height));
        data->size = {header.biWidth, static_cast<LONG>(height)};
        data->dib_top_down = header.biHeight < 0;
    } else {
        // 00522A01 -> 0046A76A -> GdipCreateBitmapFromStream. Dispatch by
        // bytes, not filename extension; RGB PNG also belongs to this path.
        data->runtime = AcquireGraphics();
        if (!data->runtime->token) return result;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (!memory) return result;
        void* target = GlobalLock(memory);
        if (!target) { GlobalFree(memory); return result; }
        std::memcpy(target, bytes.data(), bytes.size());
        GlobalUnlock(memory);
        if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &data->stream))) {
            GlobalFree(memory);
            return result;
        }
        data->image.reset(Gdiplus::Bitmap::FromStream(data->stream, FALSE));
        if (!data->image || data->image->GetLastStatus() != Gdiplus::Ok) return result;
        const UINT width = data->image->GetWidth(), height = data->image->GetHeight();
        if (!width || !height || width > 16384 || height > 16384 ||
            static_cast<ULONGLONG>(width) * height > 32 * 1024 * 1024) return result;
        data->size = {static_cast<LONG>(width), static_cast<LONG>(height)};
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        data->dib_top_down = true;
        void* pixels{};
        data->dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!data->dib) return result;
        Gdiplus::BitmapData locked{};
        Gdiplus::Rect rect(0, 0, width, height);
        if (data->image->LockBits(&rect, Gdiplus::ImageLockModeRead,
                PixelFormat32bppARGB, &locked) != Gdiplus::Ok) return result;
        for (UINT row = 0; row < height; ++row)
            std::memcpy(static_cast<unsigned char*>(pixels) + row * width * 4,
                static_cast<unsigned char*>(locked.Scan0) +
                    static_cast<ptrdiff_t>(row) * locked.Stride, width * 4);
        data->image->UnlockBits(&locked);
    }
    if (replacement_started) *replacement_started = true;
    result.storage_ = std::move(data);
    return result;
}

SkinImage::operator HBITMAP() const noexcept {
    return storage_ ? storage_->dib : borrowed_;
}
bool SkinImage::IsGdiPlus() const noexcept { return storage_ && storage_->image; }
SIZE SkinImage::Size() const noexcept {
    if (storage_) return storage_->size;
    BITMAP bitmap{};
    if (borrowed_) GetObjectW(borrowed_, sizeof(bitmap), &bitmap);
    return {bitmap.bmWidth, bitmap.bmHeight};
}

SkinImage SkinImage::CoverageMask() const {
    SkinImage result;
    if (!IsGdiPlus()) return result;
    // The PNG compatibility DIB contains straight BGRA copied by LockBits.
    // GetDIBits may discard its alpha, so read the owned DIB pixels directly.
    DIBSECTION source{};
    if (!GetObjectW(storage_->dib, sizeof(source), &source) ||
        !source.dsBm.bmBits || source.dsBm.bmBitsPixel != 32) return result;
    auto data = std::make_shared<Storage>();
    data->size = storage_->size;
    data->dib_top_down = true;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = data->size.cx;
    info.bmiHeader.biHeight = -data->size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels{};
    data->dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!data->dib || !pixels) return result;
    auto* target = static_cast<DWORD*>(pixels);
    const auto* bytes = static_cast<const unsigned char*>(source.dsBm.bmBits);
    for (LONG y = 0; y < data->size.cy; ++y) {
        const LONG source_row = storage_->dib_top_down ? y : data->size.cy - 1 - y;
        const auto* row = bytes + static_cast<size_t>(source_row) * source.dsBm.bmWidthBytes;
        for (LONG x = 0; x < data->size.cx; ++x)
            *target++ = row[x * 4 + 3] ? 0xffffffff : 0xff000000;
    }
    // A resized PNG mask must use the same nearest/half-pixel sampling as
    // the image, not StretchBlt's device-dependent downsampling mode.
    data->runtime = storage_->runtime;
    data->image = std::make_unique<Gdiplus::Bitmap>(data->size.cx, data->size.cy,
        data->size.cx * 4, PixelFormat32bppARGB, static_cast<BYTE*>(pixels));
    if (data->image->GetLastStatus() != Gdiplus::Ok) return result;
    result.storage_ = std::move(data);
    return result;
}

SkinImage SkinImage::DarkenedBufferFill(COLORREF key) const {
    // 0045269C only derives fill_image2 from a 24-bit DIB. Keep mask pixels
    // intact; halving them would turn transparent pink into a visible stripe.
    BITMAP source{};
    if (IsGdiPlus() || !GetObjectW(*this, sizeof(source), &source) ||
        source.bmBitsPixel != 24) return {};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = source.bmWidth;
    info.bmiHeader.biHeight = -source.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    auto data = std::make_shared<Storage>();
    data->size = {source.bmWidth, source.bmHeight};
    data->dib_top_down = true;
    BYTE* pixels{};
    const HDC dc = CreateCompatibleDC(nullptr);
    data->dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&pixels), nullptr, 0);
    const int rows = data->dib && pixels ? GetDIBits(dc, *this, 0, source.bmHeight,
        pixels, &info, DIB_RGB_COLORS) : 0;
    DeleteDC(dc);
    if (rows != source.bmHeight) return {};
    const int stride = (source.bmWidth * 3 + 3) & ~3;
    for (int y = 0; y < source.bmHeight; ++y) for (int x = 0; x < source.bmWidth; ++x) {
        BYTE* pixel = pixels + y * stride + x * 3;
        if (key != CLR_INVALID && RGB(pixel[2], pixel[1], pixel[0]) == key) continue;
        pixel[0] /= 2; pixel[1] /= 2; pixel[2] /= 2;
    }
    SkinImage result;
    result.storage_ = std::move(data);
    return result;
}

namespace {
bool IndexedImageRuns(HBITMAP bitmap, const DIBSECTION& section, COLORREF key,
    int width, bool top_down, std::vector<RECT>& runs, bool& disable_key) {
    if (!section.dsBm.bmBits || section.dsBm.bmBitsPixel != 8 ||
        section.dsBm.bmWidthBytes < section.dsBm.bmWidth ||
        width <= 0 || width > section.dsBm.bmWidth || section.dsBm.bmHeight <= 0) return false;
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    const auto old = SelectObject(dc, bitmap);
    if (!old || old == HGDI_ERROR) { DeleteDC(dc); return false; }
    RGBQUAD colors[256]{};
    const UINT count = GetDIBColorTable(dc, 0, 256, colors);
    const auto restored = SelectObject(dc, old);
    const BOOL released = DeleteDC(dc);
    if (!count || !restored || restored == HGDI_ERROR || !released) return false;
    UINT transparent = count;
    for (UINT i = 0; i < count; ++i)
        if (RGB(colors[i].rgbRed, colors[i].rgbGreen, colors[i].rgbBlue) == key) {
            transparent = i; break;
        }
    disable_key = transparent == count;
    if (disable_key) return true;
    const int height = section.dsBm.bmHeight;
    for (int y = 0; y < height; ++y) {
        const int row = top_down ? y : height - 1 - y;
        const auto* pixels = static_cast<const BYTE*>(section.dsBm.bmBits) +
            static_cast<size_t>(row) * section.dsBm.bmWidthBytes;
        int start = -1;
        for (int x = 0; x <= width; ++x) {
            const bool opaque = x < width && pixels[x] != transparent;
            if (opaque && start < 0) start = x;
            if (!opaque && start >= 0) { runs.push_back({start,y,x,y+1}); start = -1; }
        }
    }
    // The image generator itself returns NULL for no match/no runs. Only
    // 00452FFF's four-state button caller also disables the drawing key.
    disable_key = runs.empty();
    return true;
}

bool RgbImageRuns(HBITMAP bitmap, SIZE size, int width, COLORREF key, std::vector<RECT>& runs) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.cx;
    info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    // Read the full source stride, then scan only the normal frame. Checking
    // the returned row count prevents an unread bitmap becoming a legal NULL.
    std::vector<DWORD> pixels(static_cast<size_t>(size.cx) * size.cy);
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    const int rows = GetDIBits(dc, bitmap, 0, static_cast<UINT>(size.cy),
        pixels.data(), &info, DIB_RGB_COLORS);
    const BOOL released = DeleteDC(dc);
    if (rows != size.cy || !released) return false;
    const DWORD rgb = (GetRValue(key) << 16) | (GetGValue(key) << 8) | GetBValue(key);
    for (int y = 0; y < size.cy; ++y) {
        int start = -1;
        for (int x = 0; x <= width; ++x) {
            const bool opaque = x < width && (key == CLR_INVALID ||
                (pixels[static_cast<size_t>(y) * size.cx + x] & 0xffffff) != rgb);
            if (opaque && start < 0) start = x;
            if (!opaque && start >= 0) { runs.push_back({start,y,x,y+1}); start = -1; }
        }
    }
    return true;
}
}

COLORREF SkinImage::ButtonColorKey(COLORREF key) const {
    if (IsGdiPlus()) return key;
    bool completed{};
    const HRGN region = CreateRegion(key, 4, &completed);
    if (region) DeleteObject(region);
    if (!completed) return key;
    if (storage_) for (const auto& entry : storage_->regions)
        if (entry.key == key && entry.frames == 4) return entry.drawing_key;
    // Borrowed bitmaps have no cache; retain their existing button semantics.
    try {
        DIBSECTION section{};
        std::vector<RECT> runs;
        bool disable{};
        return GetObjectW(*this, sizeof(section), &section) == static_cast<int>(sizeof(section)) &&
            IndexedImageRuns(*this, section, key, section.dsBm.bmWidth / 4,
                section.dsBmih.biHeight < 0, runs, disable) &&
            disable ? CLR_INVALID : key;
    } catch (const std::bad_alloc&) {
        return key;
    } catch (const std::length_error&) {
        return key;
    }
}

HRGN SkinImage::CreateRegion(COLORREF key, int frames, bool* completed) const {
    if (completed) *completed = false;
    const auto copy = [completed](HRGN source) -> HRGN {
        if (!source) {
            if (completed) *completed = true;
            return nullptr;
        }
        const HRGN result = CreateRectRgn(0, 0, 0, 0);
        if (!result) return nullptr;
        if (CombineRgn(result, source, nullptr, RGN_COPY) == ERROR) {
            DeleteObject(result);
            return nullptr;
        }
        if (completed) *completed = true;
        return result;
    };
    try {
        const HBITMAP bitmap = *this;
        const SIZE size = Size();
        frames = std::max(1, frames);
        const int width = size.cx / frames;
        DIBSECTION section{};
        const int object_size = bitmap ? GetObjectW(bitmap, sizeof(section), &section) : 0;
        if (object_size < static_cast<int>(sizeof(BITMAP)) || width <= 0 || size.cy <= 0 ||
            size.cx > 16384 || size.cy > 16384 ||
            static_cast<uint64_t>(size.cx) * size.cy > 32 * 1024 * 1024 ||
            section.dsBm.bmWidth != size.cx || section.dsBm.bmHeight != size.cy) return nullptr;
        if (storage_) for (const auto& entry : storage_->regions)
            if (entry.key == key && entry.frames == frames) return copy(entry.region);
        if (!GdiFlush()) return nullptr;
        const bool alpha = IsGdiPlus();
        const bool indexed = !alpha && (frames == 1 || frames == 4) && section.dsBm.bmBitsPixel == 8;
        // Owned images know their original sign; borrowed HBITMAPs retain
        // the existing GetObject-based orientation policy.
        const bool top_down = storage_ ? storage_->dib_top_down : section.dsBmih.biHeight < 0;
        std::vector<RECT> runs;
        bool disable_key{};
        if (indexed) {
            if (object_size != static_cast<int>(sizeof(section)) ||
                !IndexedImageRuns(bitmap, section, key, width, top_down, runs, disable_key)) return nullptr;
            // Do not apply this button-only policy to fixed backgrounds:
            // 0048C9CA's NULL is a legal rectangular HWND fallback there.
            if (frames == 4 && disable_key) runs.push_back({0, 0, width, size.cy});
        } else if (alpha) {
            // Read straight BGRA from the owned PNG DIB. RGB conversion or
            // source-over drawing can discard alpha=1 coverage or key pink.
            if (object_size != static_cast<int>(sizeof(section)) || !section.dsBm.bmBits ||
                section.dsBm.bmBitsPixel != 32 || section.dsBm.bmWidthBytes < size.cx * 4)
                return nullptr;
            for (int y = 0; y < size.cy; ++y) {
                const int row = top_down ? y : size.cy - 1 - y;
                const auto* pixels = static_cast<const BYTE*>(section.dsBm.bmBits) +
                    static_cast<size_t>(row) * section.dsBm.bmWidthBytes;
                int start = -1;
                for (int x = 0; x <= width; ++x) {
                    const bool opaque = x < width && pixels[x * 4 + 3] > 0;
                    if (opaque && start < 0) start = x;
                    if (!opaque && start >= 0) { runs.push_back({start,y,x,y+1}); start = -1; }
                }
            }
        } else if (!RgbImageRuns(bitmap, size, width, key, runs)) return nullptr;
        HRGN region{};
        if (!runs.empty()) {
            const size_t max_bytes = (std::numeric_limits<DWORD>::max)();
            if (runs.size() > (max_bytes - sizeof(RGNDATAHEADER)) / sizeof(RECT)) return nullptr;
            const size_t bytes = sizeof(RGNDATAHEADER) + runs.size() * sizeof(RECT);
            std::vector<unsigned char> data(bytes);
            auto* rgn = reinterpret_cast<RGNDATA*>(data.data());
            rgn->rdh = {sizeof(RGNDATAHEADER), RDH_RECTANGLES, static_cast<DWORD>(runs.size()),
                static_cast<DWORD>(runs.size() * sizeof(RECT)), {0,0,width,size.cy}};
            std::memcpy(rgn->Buffer, runs.data(), runs.size() * sizeof(RECT));
            region = ExtCreateRegion(nullptr, static_cast<DWORD>(bytes), rgn);
            if (!region) return nullptr;
        } else if (alpha) {
            // Alpha-empty PNG is a real empty region, never a BMP fallback.
            region = CreateRectRgn(0,0,0,0);
            if (!region) return nullptr;
        }
        if (storage_) {
            try {
                storage_->regions.push_back({key, frames, region,
                    indexed && frames == 4 && disable_key ? CLR_INVALID : key});
            } catch (...) {
                if (region) DeleteObject(region);
                throw;
            }
            return copy(region);
        }
        if (completed) *completed = true;
        return region;
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (const std::length_error&) {
        return nullptr;
    }
}

bool SkinImage::Draw(HDC dc, int x, int y, int width, int height,
    int sx, int sy, int sw, int sh, COLORREF transparent, BYTE opacity) const {
    if (!dc || !static_cast<HBITMAP>(*this) || width <= 0 || height <= 0 ||
        sw <= 0 || sh <= 0) return false;
    if (opacity == 0) return true;
    if (IsGdiPlus()) {
        // 0046A938/0046A9F6: native pixels, nearest-neighbour + half-pixel
        // offset, source-over alpha; the BMP magenta key does NOT apply.
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::ImageAttributes attributes;
        Gdiplus::ColorMatrix matrix{};
        for (int i = 0; i < 5; ++i) matrix.m[i][i] = 1.0f;
        matrix.m[3][3] = opacity / 255.0f;
        if (opacity != 255) attributes.SetColorMatrix(&matrix);
        return graphics.DrawImage(storage_->image.get(), Gdiplus::Rect(x,y,width,height),
            sx,sy,sw,sh,Gdiplus::UnitPixel, opacity == 255 ? nullptr : &attributes)
            == Gdiplus::Ok;
    }
    const HDC source = CreateCompatibleDC(dc);
    if (!source) return false;
    const HGDIOBJ old = SelectObject(source, static_cast<HBITMAP>(*this));
    const BOOL drawn = transparent == CLR_INVALID
        ? StretchBlt(dc,x,y,width,height,source,sx,sy,sw,sh,SRCCOPY)
        : TransparentBlt(dc,x,y,width,height,source,sx,sy,sw,sh,transparent);
    SelectObject(source, old);
    DeleteDC(source);
    return drawn != FALSE;
}

void SkinHoverAnimation::SetState(int next, const SkinAnimation& def, ULONGLONG now) {
    if (state == next) return;
    if (def.Enabled() && ((state == 0 && next == 1) || (state == 1 && next == 0))) {
        remaining = remaining > 0 ? def.frame_count - remaining : def.frame_count;
        last_tick = now;
    } else remaining = 0;
    state = next;
}
bool SkinHoverAnimation::Tick(const SkinAnimation& def, ULONGLONG now) {
    if (remaining <= 0 || !def.Enabled() ||
        now - last_tick < static_cast<ULONGLONG>(def.frame_interval)) return false;
    --remaining;
    last_tick = now;
    return true;
}
BYTE SkinHoverAnimation::HotOpacity(const SkinAnimation& def) const {
    if (remaining <= 0 || !def.Enabled()) return state == 1 ? 255 : 0;
    const int alpha = static_cast<int>(static_cast<int64_t>(remaining) * 255 / def.frame_count);
    return static_cast<BYTE>(std::clamp(state == 1 ? 255 - alpha : alpha, 0, 255));
}

void SkinPulseAnimation::SetPlaying(bool value, const SkinAnimation& def, ULONGLONG now) {
    if (value == playing) return;
    playing = value;
    if (value) {
        remaining = def.Enabled() ? def.frame_count : 0; // 004A9ED7
        last_tick = now;
    } else if (remaining > def.frame_count / 2) {
        remaining = def.frame_count - remaining; // 0042FFDB stop/reverse
    }
}
bool SkinPulseAnimation::Tick(const SkinAnimation& def, ULONGLONG now) {
    if (!def.Enabled() || (!playing && remaining <= 0) ||
        now - last_tick < static_cast<ULONGLONG>(def.frame_interval)) return false;
    if (playing && remaining <= 0) remaining = def.frame_count;
    if (remaining > 0) --remaining;
    last_tick = now;
    return true;
}
BYTE SkinPulseAnimation::Opacity(const SkinAnimation& def) const {
    if (!def.Enabled()) return 0;
    const int distance = std::min(remaining, def.frame_count - remaining);
    return static_cast<BYTE>(std::clamp<int64_t>(
        static_cast<int64_t>(distance) * 510 / def.frame_count, 0, 255));
}
} // namespace ttplayer::skin
