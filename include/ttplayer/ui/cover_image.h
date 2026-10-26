#pragma once

#include <windows.h>
#include <span>
#include <filesystem>
#include <vector>

namespace ttplayer::ui {

// Both decoders return a top-down, premultiplied BGRA DIB. The caller owns
// bitmap and must DeleteObject it. GDI+ 1.0 works when WIC is absent on XP.
struct CoverImage {
    HBITMAP bitmap{};
    SIZE size{};
};
enum class CoverImageDecoder { automatic, wic, gdi_plus };

// Read a bounded image payload by contents, including extensionless game files.
// Empty, inaccessible and oversized files return an empty payload.
std::vector<unsigned char> ReadCoverImageFile(const std::filesystem::path&) noexcept;

CoverImage DecodeCoverImage(std::span<const unsigned char> bytes, UINT maximum_edge = 0,
                           CoverImageDecoder decoder = CoverImageDecoder::automatic);
// Opaque, centered preview for an SS_BITMAP control. Transparent cover pixels
// are composited onto the supplied background, not copied as straight BGRA.
HBITMAP DecodeCoverPreview(std::span<const unsigned char> bytes, SIZE bounds,
                          COLORREF background,
                          CoverImageDecoder decoder = CoverImageDecoder::automatic);

} // namespace ttplayer::ui
