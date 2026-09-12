#pragma once

#include <filesystem>
#include <memory>
#include <windows.h>

namespace ttplayer::skin {
// 6.1.2 SkinImage (00417411/00522A01) owns either a BMP DIB or a GDI+
// image. Keep the encoded-image representation until drawing: converting
// PNG to a colour-keyed bitmap loses its partially transparent edges.
class SkinImage {
public:
    SkinImage() = default;
    SkinImage(std::nullptr_t) noexcept {}
    // Borrowed GDI handles used by existing synthetic layouts/tests.
    SkinImage(HBITMAP bitmap) noexcept : borrowed_(bitmap) {}
    static SkinImage Load(const std::filesystem::path& path);
    [[nodiscard]] bool IsGdiPlus() const noexcept;
    [[nodiscard]] SIZE Size() const noexcept;
    // PNG window-region source: black for alpha 0, white for alpha > 0.
    // This binary mask is only for HWND shape/hit testing, never for painting.
    // BMP callers retain their original RGB colour-key region path.
    [[nodiscard]] SkinImage CoverageMask() const;
    // A DIB view for legacy size/region APIs, never the PNG drawing source.
    [[nodiscard]] operator HBITMAP() const noexcept;
    bool Draw(HDC dc, int x, int y, int width, int height,
              int source_x, int source_y, int source_width, int source_height,
              COLORREF transparent = CLR_INVALID, BYTE opacity = 255) const;
private:
    struct Storage;
    std::shared_ptr<Storage> storage_;
    HBITMAP borrowed_{};
};

struct SkinAnimation {
    int mode{};
    int frame_count{10};
    int frame_interval{100};
    [[nodiscard]] bool Enabled() const noexcept {
        return mode > 0 && frame_count > 0 && frame_interval > 0;
    }
};

// Pure state machine from 00418FA5/0041917E. Four button states are NOT
// frame_count animation frames. A reversed hover continues without jumping.
struct SkinHoverAnimation {
    int state{};
    int remaining{};
    ULONGLONG last_tick{};
    void SetState(int next, const SkinAnimation& definition, ULONGLONG now);
    bool Tick(const SkinAnimation& definition, ULONGLONG now);
    [[nodiscard]] BYTE HotOpacity(const SkinAnimation& definition) const;
};

struct SkinPulseAnimation {
    bool playing{};
    int remaining{};
    ULONGLONG last_tick{};
    void SetPlaying(bool value, const SkinAnimation& definition, ULONGLONG now);
    bool Tick(const SkinAnimation& definition, ULONGLONG now);
    [[nodiscard]] BYTE Opacity(const SkinAnimation& definition) const;
};
} // namespace ttplayer::skin
