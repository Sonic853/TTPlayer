#include "ttplayer/ui/player_window.h"
#include "player_window_internal.h"
#include <algorithm>

namespace ttplayer::ui {
using namespace detail;

skin::SkinHoverAnimation& PlayerWindow::SkinHoverState(HWND owner, std::wstring_view key,
    RECT bounds, int state, const skin::SkinAnimation& definition) const {
    auto found = std::find_if(skin_control_animations_.begin(), skin_control_animations_.end(),
        [&](const auto& item) { return item.owner == owner && item.key == key; });
    if (found == skin_control_animations_.end()) {
        skin_control_animations_.push_back({owner, std::wstring(key), bounds, definition, {}});
        found = std::prev(skin_control_animations_.end());
    }
    found->bounds = bounds;
    found->definition = definition;
    found->hover.SetState(state, definition, GetTickCount64());
    if (found->hover.remaining > 0 && window_ && !skin_control_animation_timer_)
        skin_control_animation_timer_ = SetTimer(window_, kSkinControlAnimationTimer,
            16, nullptr) != 0;
    return found->hover;
}

void PlayerWindow::AdvanceSkinControlAnimations() {
    bool running = false;
    const auto now = GetTickCount64();
    for (auto it = skin_control_animations_.begin(); it != skin_control_animations_.end();) {
        if (!IsWindow(it->owner) || !IsWindowVisible(it->owner)) {
            it = skin_control_animations_.erase(it);
            continue;
        }
        if (it->pulse ? it->pulse->Tick(it->definition, now)
                      : it->hover.Tick(it->definition, now))
            InvalidateRect(it->owner, &it->bounds, FALSE);
        running |= it->pulse ? (it->pulse->playing || it->pulse->remaining > 0)
                             : it->hover.remaining > 0;
        ++it;
    }
    if (!running) {
        KillTimer(window_, kSkinControlAnimationTimer);
        skin_control_animation_timer_ = false;
    }
}

void PlayerWindow::ResetSkinControlAnimations() {
    if (window_) KillTimer(window_, kSkinControlAnimationTimer);
    skin_control_animation_timer_ = false;
    skin_control_animations_.clear();
}

void PlayerWindow::DrawAnimatedSkinFrame(HDC dc, const skin::SkinElement& element,
    RECT bounds, int state, HWND owner) const {
    if (!skin_) return;
    const auto& def = element.animation;
    if (!element.image.IsGdiPlus() || !element.four_state ||
        !def.Enabled() || def.mode > 2) {
        DrawElementFrame(dc, element, bounds, state, skin_->TransparentColor());
        return;
    }
    const auto& transition = SkinHoverState(owner, element.name, bounds, state, def);
    if (transition.remaining <= 0 || state > 1) {
        DrawElementFrame(dc, element, bounds, state, skin_->TransparentColor());
        return;
    }
    const int width = element.image_size.cx / 4, height = element.image_size.cy;
    // Caller has restored the skin background into its double buffer. This
    // is the equivalent of the original child's parent 0x10400 background DC.
    if (def.mode == 1) {
        DrawElementFrame(dc, element, bounds, 0, skin_->TransparentColor());
        element.image.Draw(dc, bounds.left, bounds.top, width, height,
            width, 0, width, height, CLR_INVALID, transition.HotOpacity(def));
    } else if (element.flash_image.IsGdiPlus()) {
        const int frame_width = element.flash_size.cx / def.frame_count;
        const int frame = state == 1 ? def.frame_count - transition.remaining
                                     : transition.remaining - 1;
        element.flash_image.Draw(dc, bounds.left, bounds.top, frame_width,
            element.flash_size.cy, frame * frame_width, 0, frame_width,
            element.flash_size.cy);
    } else {
        DrawElementFrame(dc, element, bounds, state, skin_->TransparentColor());
    }
}

void PlayerWindow::DrawSkinSliderThumb(HDC dc, const skin::SkinElement& thumb,
    int state, bool playing, RECT control_bounds) const {
    if (!skin_) return;
    const auto& def = thumb.animation;
    if (!def.Enabled() || !thumb.image.IsGdiPlus()) {
        DrawElementFrame(dc, thumb, thumb.bounds, state, skin_->TransparentColor());
        return;
    }
    const std::wstring key = L"slider:" + thumb.name;
    auto found = std::find_if(skin_control_animations_.begin(), skin_control_animations_.end(),
        [&](const auto& item) { return item.owner == window_ && item.key == key; });
    if (found == skin_control_animations_.end()) {
        skin_control_animations_.push_back({window_, key, control_bounds, def, {},
            skin::SkinPulseAnimation{}});
        found = std::prev(skin_control_animations_.end());
    }
    found->bounds = control_bounds;
    found->definition = def;
    found->pulse->SetPlaying(playing, def, GetTickCount64());
    if ((found->pulse->playing || found->pulse->remaining > 0) &&
        !skin_control_animation_timer_)
        skin_control_animation_timer_ = SetTimer(window_, kSkinControlAnimationTimer,
            16, nullptr) != 0;
    const auto& pulse = *found->pulse;
    if (state == 2 || state == 3 || (!pulse.playing && pulse.remaining <= 0)) {
        DrawElementFrame(dc, thumb, thumb.bounds, state, skin_->TransparentColor());
        return;
    }
    const int width = thumb.image_size.cx / 4, height = thumb.image_size.cy;
    if (def.mode == 3) {
        thumb.image.Draw(dc, thumb.bounds.left, thumb.bounds.top, width, height,
            state * width, 0, width, height, CLR_INVALID, 255 - pulse.Opacity(def));
        return;
    }
    DrawElementFrame(dc, thumb, thumb.bounds, state, skin_->TransparentColor());
    if (!thumb.flash_image.IsGdiPlus()) return;
    int flash_width = thumb.flash_size.cx;
    const int flash_height = thumb.flash_size.cy;
    int frame = 0;
    BYTE opacity = pulse.Opacity(def);
    if (def.mode == 2) {
        const int frames = def.frame_count / 2;
        if (frames < 1) return;
        flash_width /= frames;
        frame = std::clamp(std::min(pulse.remaining, def.frame_count - pulse.remaining),
                           0, frames - 1);
        opacity = 255;
    }
    thumb.flash_image.Draw(dc, thumb.bounds.left + (width - flash_width) / 2,
        thumb.bounds.top + (height - flash_height) / 2, flash_width, flash_height,
        frame * flash_width, 0, flash_width, flash_height, CLR_INVALID, opacity);
}
} // namespace ttplayer::ui
