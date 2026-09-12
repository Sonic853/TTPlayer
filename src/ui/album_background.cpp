#include "album_background.h"

#include <algorithm>
#include <cstdint>
#include <olectl.h>

namespace ttplayer::ui::detail {

RECT AlbumCoverSourceRect(SIZE source, SIZE destination) noexcept {
    if (source.cx <= 0 || source.cy <= 0 || destination.cx <= 0 || destination.cy <= 0)
        return {};
    LONG width = source.cx;
    LONG height = source.cy;
    if (int64_t(source.cx) * destination.cy > int64_t(source.cy) * destination.cx)
        width = std::max<LONG>(1, static_cast<LONG>(int64_t(source.cy) * destination.cx / destination.cy));
    else
        height = std::max<LONG>(1, static_cast<LONG>(int64_t(source.cx) * destination.cy / destination.cx));
    const LONG x = (source.cx - width) / 2;
    const LONG y = (source.cy - height) / 2;
    return {x, y, x + width, y + height};
}

void PaintAlbumBackground(HDC dc, const RECT& bounds, COLORREF background,
                          int transparency_percent, HBITMAP bitmap,
                          SIZE bitmap_size, IPicture* picture) {
    const SIZE size{bounds.right - bounds.left, bounds.bottom - bounds.top};
    if (!dc || size.cx <= 0 || size.cy <= 0) return;
    const HBRUSH brush = CreateSolidBrush(background);
    FillRect(dc, &bounds, brush);
    const BYTE alpha = static_cast<BYTE>(MulDiv(
        100 - std::clamp(transparency_percent, 0, 100), 255, 100));
    if (alpha && bitmap && bitmap_size.cx > 0 && bitmap_size.cy > 0) {
        const RECT crop = AlbumCoverSourceRect(bitmap_size, size);
        if (const HDC source = CreateCompatibleDC(dc)) {
            const HGDIOBJ previous = SelectObject(source, bitmap);
            const BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
            AlphaBlend(dc, bounds.left, bounds.top, size.cx, size.cy, source,
                crop.left, crop.top, crop.right - crop.left, crop.bottom - crop.top, blend);
            SelectObject(source, previous);
            DeleteDC(source);
        }
    } else if (alpha && picture) {
        LONG width{}, height{};
        picture->get_Width(&width);
        picture->get_Height(&height);
        const RECT crop = AlbumCoverSourceRect({width, height}, size);
        // IPicture uses HIMETRIC bottom-up source coordinates. Composite its
        // rendered surface at constant alpha; it has no WIC PBGRA contract.
        if (!IsRectEmpty(&crop)) {
            const HDC source = CreateCompatibleDC(dc);
            const HBITMAP image = CreateCompatibleBitmap(dc, size.cx, size.cy);
            if (source && image) {
                const HGDIOBJ previous = SelectObject(source, image);
                const RECT local{0, 0, size.cx, size.cy};
                FillRect(source, &local, brush);
                if (SUCCEEDED(picture->Render(source, 0, 0, size.cx, size.cy,
                        crop.left, height - crop.top, crop.right - crop.left,
                        -(crop.bottom - crop.top), nullptr))) {
                    const BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
                    AlphaBlend(dc, bounds.left, bounds.top, size.cx, size.cy,
                               source, 0, 0, size.cx, size.cy, blend);
                }
                SelectObject(source, previous);
            }
            if (image) DeleteObject(image);
            if (source) DeleteDC(source);
        }
    }
    DeleteObject(brush);
}

} // namespace ttplayer::ui::detail
