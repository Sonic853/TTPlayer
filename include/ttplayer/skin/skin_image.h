#pragma once

#include <filesystem>
#include <memory>
#include <windows.h>

namespace ttplayer::skin {
// Shared ownership also keeps fallback desktop-bar icons alive after the
// temporary default skin is released.
class SkinIcon {
public:
    void Reset(HICON icon) {
        handle_.reset(icon, [](void* value) { if (value) DestroyIcon(static_cast<HICON>(value)); });
    }
    [[nodiscard]] HICON Get() const noexcept { return static_cast<HICON>(handle_.get()); }
private:
    std::shared_ptr<void> handle_;
};
struct SkinIcons { SkinIcon small_icon, large_icon; };
void DrawNativeSkinIcon(HDC dc, HICON icon, const RECT& bounds);
RECT NativeSkinIconBounds(HICON icon, RECT bounds);
// 6.1.2 SkinImage (00417411/00522A01) owns either a BMP DIB or a GDI+
// image. Keep the encoded-image representation until drawing: converting
// PNG to a colour-keyed bitmap loses its partially transparent edges.
class SkinImage {
public:
    SkinImage() = default;
    SkinImage(std::nullptr_t) noexcept {}
    // Borrowed GDI handles used by existing synthetic layouts/tests.
    SkinImage(HBITMAP bitmap) noexcept : borrowed_(bitmap) {}
    // replacement_started distinguishes 004A9D8C's outer rejection (retain
    // the old slot) from 00408CF4's destructive DIB decode (replace/clear).
    static SkinImage Load(const std::filesystem::path& path, bool* replacement_started = nullptr);
    [[nodiscard]] bool IsGdiPlus() const noexcept;
    [[nodiscard]] SIZE Size() const noexcept;
    // PNG window-region source: black for alpha 0, white for alpha > 0.
    // This binary mask is only for HWND shape/hit testing, never for painting.
    // Fixed 8-bit BMP regions must read the original palette indices instead.
    [[nodiscard]] SkinImage CoverageMask() const;
    // Caller owns the returned region. NULL means rectangular/default;
    // a non-NULL NULLREGION means empty (notably an alpha-empty PNG).
    // completed distinguishes a legal NULL BMP fallback (true) from a
    // read/allocation/GDI failure (false), including on cache hits. Only
    // completed masks/fallbacks are cached for owned images.
    [[nodiscard]] HRGN CreateRegion(COLORREF key, int frames = 1,
                                   bool* completed = nullptr) const;
    [[nodiscard]] COLORREF ButtonColorKey(COLORREF key) const;
    [[nodiscard]] SkinImage DarkenedBufferFill(COLORREF key) const;
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
