#include "player_visual_effect.h"

#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace ttplayer::ui::detail {
namespace {
double FrequencyScale(bool spectrum) noexcept {
    // 10002990 caches this once per DLL, shared by ALL processors. Preserve
    // both launch orders: spectrum first uses 100*(4/3); pulse/ripple first
    // uses 160. Reset, skin switches and later mode changes keep that value.
    static std::atomic<int> first_mode{};
    int first = 0;
    first_mode.compare_exchange_strong(first, spectrum ? 2 : 5);
    if (!first) first = spectrum ? 2 : 5;
    return (first == 2 ? 100.0 * (4.0 / 3.0) : 160.0) / std::log(256.0);
}
struct Complex { float r{}, i{}; };
struct Twiddles {
    std::array<Complex, 256> complex{}, real{};
    Twiddles() {
        for (size_t i = 0; i < complex.size(); ++i) {
            // SoundCore.dll!10030590 / 100306F0, constants read from PE.
            const double a = double(i) * -0.02454369260617026;
            const double b = (double(i) * 0.00390625 + 0.5) * -3.141592653589793;
            complex[i] = {float(std::cos(a)), float(std::sin(a))};
            real[i] = {float(std::cos(b)), float(std::sin(b))};
        }
    }
};
Complex Multiply(Complex a, Complex b) noexcept {
    return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r};
}
// The fixed 512-real-sample transform uses four radix-4 stages on 256
// complex pairs. Preserve every single-precision operation in 1002FA90;
// doing the intermediate arithmetic in double changes boundary bins.
void Transform(Complex* out, const Complex* input, int stride, int count,
               const Twiddles& twiddle) noexcept {
    const int m = count / 4;
    for (int i = 0; i < 4; ++i) {
        if (m == 1) out[i] = input[i * stride];
        else Transform(out + i * m, input + i * stride, stride * 4, m, twiddle);
    }
    for (int i = 0; i < m; ++i) {
        const Complex a = out[i];
        const Complex b = Multiply(out[i + m], twiddle.complex[i * stride]);
        const Complex c = Multiply(out[i + m * 2], twiddle.complex[i * stride * 2]);
        const Complex d = Multiply(out[i + m * 3], twiddle.complex[i * stride * 3]);
        const Complex ac_sub{a.r - c.r, a.i - c.i};
        const Complex ac_sum{a.r + c.r, a.i + c.i};
        const Complex bd_sum{d.r + b.r, d.i + b.i};
        const Complex bd_sub{b.r - d.r, b.i - d.i};
        out[i + m * 2] = {ac_sum.r - bd_sum.r, ac_sum.i - bd_sum.i};
        out[i] = {ac_sum.r + bd_sum.r, ac_sum.i + bd_sum.i};
        out[i + m] = {bd_sub.i + ac_sub.r, ac_sub.i - bd_sub.r};
        out[i + m * 3] = {ac_sub.r - bd_sub.i, bd_sub.r + ac_sub.i};
    }
}
}

void PrimePlayerSpectrumScale() noexcept { static_cast<void>(FrequencyScale(true)); }

std::array<int16_t, 257> PlayerFrequencyData(std::span<const int16_t, 512> pcm) noexcept {
    static const Twiddles twiddle;
    std::array<Complex, 256> input{}, work{};
    std::array<Complex, 257> spectrum{};
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = {float(pcm[i * 2]), float(pcm[i * 2 + 1])};
    Transform(work.data(), input.data(), 1, 256, twiddle);
    spectrum[0] = {work[0].i + work[0].r, 0};
    for (size_t i = 1; i <= 128; ++i) {
        const Complex forward = work[i];
        const Complex backward{work[256 - i].r, -work[256 - i].i};
        const Complex even{forward.r + backward.r, forward.i + backward.i};
        const Complex odd{forward.r - backward.r, forward.i - backward.i};
        const Complex rotated = Multiply(odd, twiddle.real[i]);
        spectrum[i] = {(rotated.r + even.r) * 0.5f, (rotated.i + even.i) * 0.5f};
        spectrum[256 - i] = {(even.r - rotated.r) * 0.5f, (even.i - rotated.i) * -0.5f};
    }
    std::array<int16_t, 257> result{};
    // 1002A180 writes 256 bins. 1002A29D rounds the double sqrt to float
    // BEFORE truncating to int and shifting by 8 (lost in the pseudo-C).
    // Slot 256 is the unused second-channel slot in the zeroed mono buffer.
    for (size_t i = 0; i < 256; ++i) {
        const auto value = spectrum[i];
        const float square = value.r * value.r + value.i * value.i;
        result[i] = int16_t(uint32_t(float(std::sqrt(double(square)))) >> 8);
    }
    return result;
}

PlayerVisualEffect::~PlayerVisualEffect() {
    if (pulse_dc_ && pulse_previous_) SelectObject(pulse_dc_, pulse_previous_);
    if (pulse_bitmap_) DeleteObject(pulse_bitmap_);
    if (pulse_dc_) DeleteDC(pulse_dc_);
    if (graphics_token_) Gdiplus::GdiplusShutdown(graphics_token_);
}

void PlayerVisualEffect::Reset() noexcept { bands_ = {}; }

void PlayerVisualEffect::Update(std::span<const int16_t> frequency) noexcept {
    // 10002840: max(trunc(pow(2, i * 7.5 / 256)), previous + 1).
    // With exactly 256 columns this is 1..256. Bin 256 must be retained:
    // clamping it to 255 subtly changes the outside column/ripple.
    if (frequency.size() < 257) return;
    const double scale = FrequencyScale(false); // 10003b90, 10002990
    for (size_t i = 0; i < bands_.size(); ++i) {
        const int shifted = std::max(0, int(frequency[i + 1])) >> 5;
        const int value = shifted ? int(std::log(double(shifted)) * scale) : 0;
        const int left = i ? bands_[i - 1].level : value;
        const int right = i + 1 < bands_.size() ? bands_[i + 1].level : value;
        const int target = std::clamp((left + value * 2 + right) / 4, 0, 159);
        auto& band = bands_[i];
        if (band.level < target) band.level = static_cast<int16_t>(target);
        else if (band.level > 2) band.level -= 2;
        if (band.level < band.peak) {
            if (band.peak > 0) {
                band.peak -= band.fall >> 3;
                ++band.fall;
            }
            if (band.fall >= 159) band.peak = 0;
        } else {
            band.peak = band.level;
            band.fall = 0;
        }
        band.previous_peak = band.peak;
    }
}

bool PlayerVisualEffect::EnsurePulseSurface() {
    if (pulse_pixels_) return true;
    pulse_dc_ = CreateCompatibleDC(nullptr);
    if (!pulse_dc_) return false;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), 512, 160, 1, 32, BI_RGB};
    void* pixels{};
    pulse_bitmap_ = CreateDIBSection(pulse_dc_, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!pulse_bitmap_) { DeleteDC(pulse_dc_); pulse_dc_ = nullptr; return false; }
    pulse_previous_ = SelectObject(pulse_dc_, pulse_bitmap_);
    pulse_pixels_ = static_cast<uint32_t*>(pixels);
    return true;
}

void PlayerVisualEffect::PaintPulse(HDC dc, const RECT& bounds, COLORREF color) {
    if (!EnsurePulseSurface()) return;
    GdiFlush();
    std::fill_n(pulse_pixels_, 512 * 160, 0U);
    // 100034D0: four nested vertical strokes per band, mirrored about
    // x=255/256. LineTo excludes its final pixel, as in the original.
    for (int i = 0; i < 256; ++i) {
        const int level = bands_[i].level;
        if (!level) continue;
        const double amount = double(level) / 160.0;
        const int red = int(GetRValue(color) * amount);
        const int green = int(GetGValue(color) * amount);
        const int blue = int(GetBValue(color) * amount);
        for (int layer = 4; layer > 0; --layer) {
            const int top = int(80.0 - layer * double(level) * 0.125);
            const int bottom = int(layer * double(level) * 0.25 + top);
            if (top == bottom) continue;
            const int add = (4 - layer) * 16;
            const HPEN pen = CreatePen(PS_SOLID, 1, RGB(
                std::min(255, red + add), std::min(255, green + add), std::min(255, blue + add)));
            if (!pen) continue;
            const HGDIOBJ old = SelectObject(pulse_dc_, pen);
            MoveToEx(pulse_dc_, i + 256, top, nullptr);
            LineTo(pulse_dc_, i + 256, bottom);
            MoveToEx(pulse_dc_, 255 - i, top, nullptr);
            LineTo(pulse_dc_, 255 - i, bottom);
            SelectObject(pulse_dc_, old);
            DeleteObject(pen);
        }
    }
    StretchBlt(dc, bounds.left, bounds.top, bounds.right - bounds.left,
               bounds.bottom - bounds.top, pulse_dc_, 0, 0, 512, 160, SRCCOPY);
}

void PlayerVisualEffect::PaintRipple(HDC dc, const RECT& bounds, COLORREF color) {
    if (!graphics_token_) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&graphics_token_, &input, nullptr) != Gdiplus::Ok) {
            graphics_token_ = 0;
            return;
        }
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality); // 2, not 4
    const double sx = double(bounds.right - bounds.left) / 256.0;
    const double sy = double(bounds.bottom - bounds.top) / 160.0;
    std::array<bool, 160> drawn{};
    // 10003820: draw each nonzero amplitude once, in frequency order.
    // These rings follow spectrum levels; there is no time/radius simulation.
    for (const auto& band : bands_) {
        const int level = band.level;
        if (!level || drawn[level]) continue;
        drawn[level] = true;
        const double amount = double(160 - level) / 160.0;
        const int red = int(GetRValue(color) * amount);
        const int green = int(GetGValue(color) * amount);
        const int blue = int(GetBValue(color) * amount);
        if (!(red || green || blue)) continue;
        const int y = bounds.top - int((160 - level) * sy * -0.5);
        const int height = int(level * sy);
        if (!height) continue;
        const int x = bounds.left - int((256 - level) * sx * -0.5);
        const int width = int(level * sx);
        Gdiplus::Pen outer(Gdiplus::Color(0xd5, BYTE(red), BYTE(green), BYTE(blue)), 3.0f);
        graphics.DrawEllipse(&outer, x, y, width, height);
        Gdiplus::Pen inner(Gdiplus::Color(0xd5, BYTE(std::min(255, red + 16)),
            BYTE(std::min(255, green + 16)), BYTE(std::min(255, blue + 16))), 1.0f);
        graphics.DrawEllipse(&inner, x, y, width, height);
    }
}

void PlayerVisualEffect::Paint(HDC dc, const RECT& bounds, int type, COLORREF color) {
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    if (type == 5) PaintPulse(dc, bounds, color);
    else if (type == 6) PaintRipple(dc, bounds, color);
}
}
