#pragma once

#include <windows.h>
#include <filesystem>

struct IPicture;
namespace ttplayer::audio { struct AudioMetadata; }

namespace ttplayer::ui::detail {

// Same reader/embedded-picture policy as the visual cover. Caller owns the
// bounded 32-bit bitmap. Invalid or absent artwork returns nullptr.
HBITMAP LoadTaskbarCoverBitmap(const std::filesystem::path& path,
                              const audio::AudioMetadata& metadata, SIZE* size);

// Center crop in source coordinates. Unlike the embedded cover's contain
// policy, the fullscreen background fills both axes and may enlarge images.
RECT AlbumCoverSourceRect(SIZE source, SIZE destination) noexcept;

// Paint into the caller's back buffer (never directly into a visible HWND).
// bitmap is premultiplied BGRA from WIC or GDI+; picture is the OLE fallback.
void PaintAlbumBackground(HDC dc, const RECT& bounds, COLORREF background,
                          int transparency_percent, HBITMAP bitmap,
                          SIZE bitmap_size, IPicture* picture);

} // namespace ttplayer::ui::detail
