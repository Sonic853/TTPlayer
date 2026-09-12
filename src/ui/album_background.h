#pragma once

#include <windows.h>

struct IPicture;

namespace ttplayer::ui::detail {

// Center crop in source coordinates. Unlike the embedded cover's contain
// policy, the fullscreen background fills both axes and may enlarge images.
RECT AlbumCoverSourceRect(SIZE source, SIZE destination) noexcept;

// Paint into the caller's back buffer (never directly into a visible HWND).
// bitmap is WIC premultiplied BGRA; picture is used only when WIC failed.
void PaintAlbumBackground(HDC dc, const RECT& bounds, COLORREF background,
                          int transparency_percent, HBITMAP bitmap,
                          SIZE bitmap_size, IPicture* picture);

} // namespace ttplayer::ui::detail
