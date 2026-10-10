#pragma once
#include "ttplayer/skin/skin.h"

namespace ttplayer::skin {
// A native child owns its entire client rectangle, except SkinButton's
// normal-frame window region. Keep paint clipping and hit testing identical.
struct SkinLayer {
    const SkinElement* element{};
    RECT bounds{};
    int id{};
};
class SkinLayers {
public:
    SkinLayers(std::vector<SkinLayer> front_to_back, COLORREF key);
    ~SkinLayers();
    SkinLayers(const SkinLayers&) = delete;
    SkinLayers& operator=(const SkinLayers&) = delete;
    const SkinLayer* Hit(POINT point) const;
    // Returned region belongs to the caller, in parent client coordinates.
    HRGN Region(const SkinElement* element) const;
    void Clip(HDC dc, const SkinElement* element) const;
private:
    std::vector<SkinLayer> layers_;
    std::vector<HRGN> regions_;
};
class SkinLayerClip {
public:
    SkinLayerClip(HDC dc, const SkinLayers& layers, const SkinElement* element)
        : dc_(dc), saved_(SaveDC(dc)) { if (saved_) layers.Clip(dc, element); }
    ~SkinLayerClip() { if (saved_) RestoreDC(dc_, saved_); }
    SkinLayerClip(const SkinLayerClip&) = delete;
    SkinLayerClip& operator=(const SkinLayerClip&) = delete;
private:
    HDC dc_{};
    int saved_{};
};
} // namespace ttplayer::skin
