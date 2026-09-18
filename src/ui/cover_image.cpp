#include "ttplayer/ui/cover_image.h"
#include "ttplayer/platform/optional_windows_api.h"

#include <objidl.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

namespace ttplayer::ui {
namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t kMaximumBytes = 64U * 1024U * 1024U;

bool ValidSize(UINT width, UINT height) noexcept {
    return width && height && width <= 16384 && height <= 16384 &&
           static_cast<uint64_t>(width) * height <= 32U * 1024U * 1024U;
}
SIZE BoundedSize(UINT width, UINT height, UINT maximum_edge) noexcept {
    const UINT longest = std::max(width, height);
    if (maximum_edge && longest > maximum_edge) {
        width = std::max<UINT>(1, static_cast<UINT>(static_cast<uint64_t>(width) * maximum_edge / longest));
        height = std::max<UINT>(1, static_cast<UINT>(static_cast<uint64_t>(height) * maximum_edge / longest));
    }
    return {static_cast<LONG>(width), static_cast<LONG>(height)};
}
HBITMAP CreateBitmap(SIZE size, void** pixels) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.cx;
    info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, pixels, nullptr, 0);
}
CoverImage DecodeWic(std::span<const unsigned char> bytes, UINT maximum_edge) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory)))) return {};
    ComPtr<IStream> stream;
    stream.Attach(platform::SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    if (!stream) return {};
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width{}, height{};
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
        !ValidSize(width, height)) return {};
    const SIZE size = BoundedSize(width, height, maximum_edge);
    ComPtr<IWICBitmapScaler> scaler;
    IWICBitmapSource* source = frame.Get();
    if (size.cx != static_cast<LONG>(width) || size.cy != static_cast<LONG>(height)) {
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), size.cx, size.cy, WICBitmapInterpolationModeFant))) return {};
        source = scaler.Get();
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return {};
    void* pixels{};
    const HBITMAP bitmap = CreateBitmap(size, &pixels);
    if (!bitmap) return {};
    const UINT stride = static_cast<UINT>(size.cx) * 4U;
    if (!pixels || FAILED(converter->CopyPixels(nullptr, stride, stride * size.cy, static_cast<BYTE*>(pixels)))) {
        DeleteObject(bitmap);
        return {};
    }
    return {bitmap, size};
}

struct GraphicsRuntime {
    ULONG_PTR token{};
    GraphicsRuntime() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) token = 0;
    }
    ~GraphicsRuntime() { if (token) Gdiplus::GdiplusShutdown(token); }
};
CoverImage DecodeGdiPlus(std::span<const unsigned char> bytes, UINT maximum_edge) {
    // All GDI+ images and their stream die before shutdown. Only the copied
    // DIB crosses the worker/UI boundary, with no lazy stream dependency.
    GraphicsRuntime graphics;
    if (!graphics.token) return {};
    ComPtr<IStream> stream;
    stream.Attach(platform::SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    if (!stream) return {};
    std::unique_ptr<Gdiplus::Bitmap> image(Gdiplus::Bitmap::FromStream(stream.Get(), FALSE));
    if (!image || image->GetLastStatus() != Gdiplus::Ok) return {};
    const UINT width = image->GetWidth(), height = image->GetHeight();
    if (!ValidSize(width, height)) return {};
    const SIZE size = BoundedSize(width, height, maximum_edge);
    std::unique_ptr<Gdiplus::Bitmap> scaled;
    auto* source = image.get();
    if (size.cx != static_cast<LONG>(width) || size.cy != static_cast<LONG>(height)) {
        scaled = std::make_unique<Gdiplus::Bitmap>(size.cx, size.cy, PixelFormat32bppPARGB);
        if (scaled->GetLastStatus() != Gdiplus::Ok) return {};
        Gdiplus::Graphics canvas(scaled.get());
        canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        canvas.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        canvas.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::ImageAttributes attributes;
        attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        if (canvas.DrawImage(image.get(), Gdiplus::Rect(0, 0, size.cx, size.cy),
                0, 0, width, height, Gdiplus::UnitPixel, &attributes) != Gdiplus::Ok) return {};
        source = scaled.get();
    }
    Gdiplus::BitmapData locked{};
    const Gdiplus::Rect rect(0, 0, size.cx, size.cy);
    if (source->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &locked) != Gdiplus::Ok)
        return {};
    void* pixels{};
    const HBITMAP bitmap = CreateBitmap(size, &pixels);
    if (bitmap && pixels) {
        const size_t stride = static_cast<size_t>(size.cx) * 4U;
        for (LONG row = 0; row < size.cy; ++row)
            std::memcpy(static_cast<BYTE*>(pixels) + static_cast<size_t>(row) * stride,
                static_cast<const BYTE*>(locked.Scan0) + static_cast<ptrdiff_t>(row) * locked.Stride, stride);
    }
    const auto status = source->UnlockBits(&locked);
    if (!bitmap || !pixels || status != Gdiplus::Ok) {
        if (bitmap) DeleteObject(bitmap);
        return {};
    }
    return {bitmap, size};
}
} // namespace

CoverImage DecodeCoverImage(std::span<const unsigned char> bytes, UINT maximum_edge,
                           CoverImageDecoder decoder) {
    if (bytes.empty() || bytes.size() > kMaximumBytes) return {};
    try {
        if (decoder != CoverImageDecoder::gdi_plus) {
            const auto image = DecodeWic(bytes, maximum_edge);
            if (image.bitmap || decoder == CoverImageDecoder::wic) return image;
        }
        return DecodeGdiPlus(bytes, maximum_edge);
    } catch (const std::bad_alloc&) {
        // The file-info preview runs on the UI thread, including low-memory
        // XP processes. Treat allocation failure like an undecodable cover.
        return {};
    }
}

HBITMAP DecodeCoverPreview(std::span<const unsigned char> bytes, SIZE bounds,
                          COLORREF background, CoverImageDecoder decoder) {
    if (!ValidSize(bounds.cx, bounds.cy)) return nullptr;
    const auto cover = DecodeCoverImage(bytes, std::max(bounds.cx, bounds.cy), decoder);
    if (!cover.bitmap) return nullptr;
    void* pixels{};
    HBITMAP bitmap = CreateBitmap(bounds, &pixels);
    HDC destination = CreateCompatibleDC(nullptr), source = CreateCompatibleDC(nullptr);
    bool okay = bitmap && pixels && destination && source;
    if (okay) {
        const DWORD background_pixel = 0xff000000U | (GetRValue(background) << 16) |
                                       (GetGValue(background) << 8) | GetBValue(background);
        std::fill_n(static_cast<DWORD*>(pixels), static_cast<size_t>(bounds.cx) * bounds.cy, background_pixel);
        const double scale = std::min(static_cast<double>(bounds.cx) / cover.size.cx,
                                      static_cast<double>(bounds.cy) / cover.size.cy);
        const int width = std::clamp(static_cast<int>(cover.size.cx * scale), 1, static_cast<int>(bounds.cx));
        const int height = std::clamp(static_cast<int>(cover.size.cy * scale), 1, static_cast<int>(bounds.cy));
        const auto old_destination = SelectObject(destination, bitmap);
        const auto old_source = SelectObject(source, cover.bitmap);
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        okay = AlphaBlend(destination, (bounds.cx - width) / 2, (bounds.cy - height) / 2,
            width, height, source, 0, 0, cover.size.cx, cover.size.cy, blend) != FALSE;
        GdiFlush();
        SelectObject(source, old_source);
        SelectObject(destination, old_destination);
    }
    if (source) DeleteDC(source);
    if (destination) DeleteDC(destination);
    DeleteObject(cover.bitmap);
    if (!okay && bitmap) { DeleteObject(bitmap); bitmap = nullptr; }
    return bitmap;
}
} // namespace ttplayer::ui
