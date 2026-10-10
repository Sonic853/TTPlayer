#include "ttplayer/skin/skin_layers.h"

namespace ttplayer::skin {
SkinLayers::SkinLayers(std::vector<SkinLayer> front_to_back, COLORREF key)
    : layers_(std::move(front_to_back)) {
    const HRGN occupied = CreateRectRgn(0, 0, 0, 0);
    for (const auto& layer : layers_) {
        HRGN region{};
        if (layer.element && layer.element->four_state) {
            region = layer.element->image.CreateRegion(key, 4);
            if (region) OffsetRgn(region, layer.bounds.left, layer.bounds.top);
        }
        if (!region) region = CreateRectRgnIndirect(&layer.bounds);
        const HRGN bounds = CreateRectRgnIndirect(&layer.bounds);
        CombineRgn(region, region, bounds, RGN_AND);
        DeleteObject(bounds);
        CombineRgn(region, region, occupied, RGN_DIFF);
        CombineRgn(occupied, occupied, region, RGN_OR);
        regions_.push_back(region);
    }
    DeleteObject(occupied);
}
SkinLayers::~SkinLayers() { for (const auto region : regions_) DeleteObject(region); }
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
    const HRGN region = Region(element);
    ExtSelectClipRgn(dc, region, RGN_AND);
    DeleteObject(region);
}
} // namespace ttplayer::skin
