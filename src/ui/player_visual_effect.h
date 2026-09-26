#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <span>

namespace ttplayer::testing { struct PlayerVisualAccess; }
namespace ttplayer::ui::detail {

std::array<int16_t, 257> PlayerFrequencyData(std::span<const int16_t, 512> pcm) noexcept;
void PrimePlayerSpectrumScale() noexcept;

// BaiduMusic 8.2.0.9 VisualEffect.dll, modes 5 and 6. The input is the
// sound processor's signed-short frequency buffer, not PCM or a new FFT.
class PlayerVisualEffect {
public:
    PlayerVisualEffect() = default;
    ~PlayerVisualEffect();
    PlayerVisualEffect(const PlayerVisualEffect&) = delete;
    PlayerVisualEffect& operator=(const PlayerVisualEffect&) = delete;

    void Reset() noexcept;
    void Update(std::span<const int16_t> frequency) noexcept;
    void Paint(HDC dc, const RECT& bounds, int type, COLORREF color);

private:
    friend struct ttplayer::testing::PlayerVisualAccess;
    struct Band { int16_t level{}, fall{}, peak{}, previous_peak{}; };
    std::array<Band, 256> bands_{};
    HDC pulse_dc_{};
    HBITMAP pulse_bitmap_{};
    HGDIOBJ pulse_previous_{};
    uint32_t* pulse_pixels_{};
    ULONG_PTR graphics_token_{};
    bool EnsurePulseSurface();
    void PaintPulse(HDC dc, const RECT& bounds, COLORREF color);
    void PaintRipple(HDC dc, const RECT& bounds, COLORREF color);
};
}
