#include "ttplayer/skin/skin_layers.h"
#include <algorithm>

namespace ttplayer::skin {
SkinLayers::SkinLayers(std::vector<SkinLayer> front_to_back, COLORREF key,
    const std::vector<std::wstring>* repaint)
    : layers_(std::move(front_to_back)) {
    const HRGN occupied = CreateRectRgn(0, 0, 0, 0);
    const HRGN paint_occupied = CreateRectRgn(0, 0, 0, 0);
    for (const auto& layer : layers_) {
        HRGN region{};
        if (layer.element && layer.element->four_state &&
            !(layer.element->image.IsGdiPlus() && layer.element->rectangular_hit_test) &&
            (layer.element->image.IsGdiPlus() || layer.element->image.ButtonColorKey(key) != CLR_INVALID)) {
            region = layer.element->image.CreateRegion(key, 4);
            if (region) OffsetRgn(region, layer.bounds.left, layer.bounds.top);
        }
        if (!region) region = CreateRectRgnIndirect(&layer.bounds);
        const HRGN bounds = CreateRectRgnIndirect(&layer.bounds);
        CombineRgn(region, region, bounds, RGN_AND);
        const bool png = layer.element && IsPngSkinButton(*layer.element);
        const HRGN paint = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(paint, png ? bounds : region, nullptr, RGN_COPY);
        DeleteObject(bounds);
        if (layer.clip_siblings) CombineRgn(paint, paint, paint_occupied, RGN_DIFF);
        else if (!repaint) {
            // These original children paint later without sibling clipping.
            // Their complete client area replaces earlier pixels, while
            // their mouse target still remains below those HWNDs.
            for (const auto earlier : paint_regions_)
                CombineRgn(earlier, earlier, region, RGN_DIFF);
        }
        paint_regions_.push_back(paint);
        if (repaint && (!layer.element || std::find(repaint->begin(), repaint->end(),
            layer.element->name) == repaint->end())) SetRectRgn(paint, 0, 0, 0, 0);
        // Transparent PNG pixels need the lower sibling's composed backing.
        // PNGs are drawn in a separate source-over pass; BMP ownership stays
        // identical to the original colour-keyed HWND rules.
        if (!png) CombineRgn(paint_occupied, paint_occupied, region, RGN_OR);
        CombineRgn(region, region, occupied, RGN_DIFF);
        CombineRgn(occupied, occupied, region, RGN_OR);
        regions_.push_back(region);
    }
    DeleteObject(occupied);
    DeleteObject(paint_occupied);
}

void SkinLayers::PaintPngButtons(HDC dc,
    const std::function<void(const SkinElement&)>& draw) const {
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        if (!it->element || !IsPngSkinButton(*it->element)) continue;
        const SkinLayerClip clip(dc, *this, it->element);
        draw(*it->element);
    }
}
SkinLayers::~SkinLayers() {
    for (const auto region : regions_) DeleteObject(region);
    for (const auto region : paint_regions_) DeleteObject(region);
}
const SkinLayer* SkinLayers::Hit(POINT point) const {
    for (size_t i = 0; i < layers_.size(); ++i)
        if (PtInRegion(regions_[i], point.x, point.y)) return &layers_[i];
    return nullptr;
}
HRGN SkinLayers::Region(const SkinElement* element) const {
    const HRGN result = CreateRectRgn(0, 0, 0, 0);
    for (size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i].element == element) {
            CombineRgn(result, regions_[i], nullptr, RGN_COPY);
            break;
        }
    return result;
}
void SkinLayers::Clip(HDC dc, const SkinElement* element) const {
    for (size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i].element == element) {
            ExtSelectClipRgn(dc, paint_regions_[i], RGN_AND);
            return;
        }
    IntersectClipRect(dc, 0, 0, 0, 0);
}
void SkinLayers::ClipRepaint(HDC dc) const {
    const HRGN region = CreateRectRgn(0, 0, 0, 0);
    for (const auto paint : paint_regions_)
        CombineRgn(region, region, paint, RGN_OR);
    ExtSelectClipRgn(dc, region, RGN_AND);
    DeleteObject(region);
}
} // namespace ttplayer::skin
