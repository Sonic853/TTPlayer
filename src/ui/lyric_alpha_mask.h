#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace ttplayer::ui::detail {
// Reusable grayscale coverage buffer. Composite into premultiplied BGRA,
// independently of the selected text/background colors (including black).
class LyricAlphaMask {
public:
    LyricAlphaMask(HDC reference, int width, int height) : width_(width), height_(height) {
        dc_ = CreateCompatibleDC(reference);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        bitmap_ = CreateDIBSection(reference, &info, DIB_RGB_COLORS,
            reinterpret_cast<void**>(&pixels_), nullptr, 0);
        clip_ = CreateRectRgn(0, 0, 0, 0);
        if (dc_ && bitmap_) old_ = SelectObject(dc_, bitmap_);
    }
    ~LyricAlphaMask() {
        if (old_) SelectObject(dc_, old_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
        if (clip_) DeleteObject(clip_);
    }
    LyricAlphaMask(const LyricAlphaMask&) = delete;
    LyricAlphaMask& operator=(const LyricAlphaMask&) = delete;
    explicit operator bool() const { return dc_ && bitmap_ && pixels_ && clip_ && old_; }

    template<class Paint>
    void Draw(HDC context, uint32_t* target, RECT bounds, Paint paint) {
        if (!*this || !target) return;
        RECT client{0, 0, width_, height_}, clipped{};
        if (!IntersectRect(&bounds, &bounds, &client) ||
            GetClipBox(context, &clipped) == ERROR ||
            !IntersectRect(&bounds, &bounds, &clipped)) return;
        const int saved = SaveDC(dc_);
        if (!saved) return;
        // Clear without clipping: a previous karaoke pass must not leak
        // coverage into the other half of the row.
        SelectClipRgn(dc_, nullptr);
        FillRect(dc_, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (GetClipRgn(context, clip_) == 1) SelectClipRgn(dc_, clip_);
        IntersectClipRect(dc_, bounds.left, bounds.top, bounds.right, bounds.bottom);
        const HGDIOBJ font = GetCurrentObject(context, OBJ_FONT);
        LOGFONTW description{};
        HFONT grayscale{};
        if (GetObjectW(font, sizeof(description), &description) &&
            description.lfQuality != ANTIALIASED_QUALITY) {
            description.lfQuality = ANTIALIASED_QUALITY;
            grayscale = CreateFontIndirectW(&description);
        }
        SelectObject(dc_, grayscale ? grayscale : font);
        SelectObject(dc_, GetStockObject(DC_PEN));
        SetDCPenColor(dc_, RGB(255, 255, 255));
        SetTextColor(dc_, RGB(255, 255, 255));
        SetBkColor(dc_, RGB(0, 0, 0));
        SetBkMode(dc_, TRANSPARENT);
        paint(dc_);
        GdiFlush(); // Complete batched GDI work before accessing DIB bits.
        const COLORREF color = GetTextColor(context);
        for (int y = bounds.top; y < bounds.bottom; ++y) {
            for (int x = bounds.left; x < bounds.right; ++x) {
                const size_t index = static_cast<size_t>(y) * width_ + x;
                const unsigned coverage = pixels_[index] & 0xffU;
                if (!coverage) continue;
                const uint32_t previous = target[index];
                const unsigned keep = 255 - coverage;
                const auto channel = [&](unsigned shift, unsigned foreground) {
                    return (foreground * coverage + ((previous >> shift) & 255U) * keep + 127) / 255;
                };
                const unsigned alpha = coverage + (((previous >> 24) * keep + 127) / 255);
                target[index] = (alpha << 24) | (channel(16, GetRValue(color)) << 16) |
                    (channel(8, GetGValue(color)) << 8) | channel(0, GetBValue(color));
            }
        }
        RestoreDC(dc_, saved);
        if (grayscale) DeleteObject(grayscale);
    }

    void Text(HDC context, uint32_t* target, const std::wstring& text, RECT bounds, UINT format) {
        Draw(context, target, bounds, [&](HDC mask) {
            DrawTextW(mask, text.c_str(), static_cast<int>(text.size()), &bounds, format);
        });
    }

private:
    int width_{}, height_{};
    HDC dc_{};
    HBITMAP bitmap_{};
    HGDIOBJ old_{};
    HRGN clip_{};
    uint32_t* pixels_{};
};
} // namespace ttplayer::ui::detail
