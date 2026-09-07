#include "ttplayer/ui/taskbar_playback.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cwchar>
#include <vector>

namespace ttplayer::ui {
namespace {
// Resolution-independent transport glyphs on a blue disc stay legible on
// both light and dark taskbars. Rasterize premultiplied alpha at native icon
// dimensions; the shell supplies hover, pressed and disabled rendering.
HICON MakeTransportIcon(int width, int height, size_t glyph) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits{};
    const HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                          &bits, nullptr, 0);
    if (!color || !bits) { if (color) DeleteObject(color); return nullptr; }
    const auto rect = [](double x, double y, double l, double t, double r, double b) {
        return x >= l && x <= r && y >= t && y <= b;
    };
    auto* pixels = static_cast<std::uint32_t*>(bits);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned a{}, r{}, g{}, b{};
            for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
                double u = (x + (sx + 0.5) / 4) / width;
                const double v = (y + (sy + 0.5) / 4) / height;
                if ((u - .5) * (u - .5) + (v - .5) * (v - .5) > .46 * .46) continue;
                bool ink{};
                if (glyph == 2) {
                    ink = rect(u, v, .32, .27, .44, .73) || rect(u, v, .56, .27, .68, .73);
                } else if (glyph == 1) {
                    ink = u >= .36 && u <= .72 && std::abs(v - .5) <= (.72 - u) * .7;
                } else {
                    if (glyph == 0) u = 1 - u;
                    ink = rect(u, v, .66, .28, .75, .72) ||
                          (u >= .28 && u <= .63 && std::abs(v - .5) <= (.63 - u) * .66);
                }
                a += 255; r += ink ? 255 : 30; g += ink ? 255 : 115; b += ink ? 255 : 190;
            }
            pixels[y * width + x] = ((a / 16) << 24) | ((r / 16) << 16) |
                                    ((g / 16) << 8) | (b / 16);
        }
    }
    std::vector<unsigned char> mask_bits(static_cast<size_t>((width + 15) / 16 * 2 * height));
    const HBITMAP mask = CreateBitmap(width, height, 1, 1, mask_bits.data());
    ICONINFO icon{};
    icon.fIcon = TRUE;
    icon.hbmColor = color;
    icon.hbmMask = mask;
    const HICON result = mask ? CreateIconIndirect(&icon) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return result;
}
} // namespace

HRESULT TaskbarPlaybackControls::CreateNativeTaskbar(ITaskbarList3** taskbar) {
    return CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(taskbar));
}

TaskbarPlaybackControls::~TaskbarPlaybackControls() { Reset(); }

void TaskbarPlaybackControls::Reset() {
    if (taskbar_) taskbar_->Release();
    taskbar_ = nullptr;
    window_ = nullptr;
    added_ = have_state_ = false;
    for (auto& icon : icons_) { if (icon) DestroyIcon(icon); icon = nullptr; }
    icon_width_ = icon_height_ = 0;
}

bool TaskbarPlaybackControls::EnsureIcons() {
    const int width = std::max(16, GetSystemMetrics(SM_CXICON));
    const int height = std::max(16, GetSystemMetrics(SM_CYICON));
    if (icon_width_ == width && icon_height_ == height && icons_[0]) return true;
    std::array<HICON, 4> next{};
    for (size_t index = 0; index < next.size(); ++index) {
        next[index] = MakeTransportIcon(width, height, index);
        if (!next[index]) {
            for (auto icon : next) if (icon) DestroyIcon(icon);
            return false;
        }
    }
    for (auto icon : icons_) if (icon) DestroyIcon(icon);
    icons_ = next;
    icon_width_ = width;
    icon_height_ = height;
    have_state_ = false;
    return true;
}

std::array<THUMBBUTTON, 3> TaskbarPlaybackControls::Buttons(
    const TaskbarPlaybackState& state, const TaskbarPlaybackLabels& labels) const {
    std::array<THUMBBUTTON, 3> buttons{};
    const std::wstring* tips[]{&labels.previous, state.playing ? &labels.pause : &labels.play, &labels.next};
    const bool enabled[]{state.previous_enabled, state.play_pause_enabled, state.next_enabled};
    const HICON icons[]{icons_[0], icons_[state.playing ? 2 : 1], icons_[3]};
    for (size_t index = 0; index < buttons.size(); ++index) {
        auto& button = buttons[index];
        button.iId = static_cast<UINT>(TaskbarPlaybackAction::previous) + static_cast<UINT>(index);
        button.dwMask = static_cast<THUMBBUTTONMASK>(THB_ICON | THB_TOOLTIP | THB_FLAGS);
        button.hIcon = icons[index];
        wcsncpy_s(button.szTip, tips[index]->c_str(), _TRUNCATE);
        button.dwFlags = enabled[index] ? THBF_ENABLED : THBF_DISABLED;
        // Do not dismiss the flyout or restore the window on transport clicks.
    }
    return buttons;
}

HRESULT TaskbarPlaybackControls::OnButtonCreated(
    HWND window, const TaskbarPlaybackState& state, const TaskbarPlaybackLabels& labels) {
    if (!window || !IsWindow(window)) return E_INVALIDARG;
    if (window_ && window_ != window) Reset();
    if (!taskbar_) {
        HRESULT result = factory_(&taskbar_);
        if (FAILED(result)) { Reset(); return result; }
        if (!taskbar_) return E_NOINTERFACE;
        result = taskbar_->HrInit();
        if (FAILED(result)) { Reset(); return result; }
    }
    window_ = window;
    have_state_ = false;
    if (added_) {
        const HRESULT result = Update(state, labels);
        if (SUCCEEDED(result)) return result; // duplicate notification, same HWND
        added_ = false; // the shell may have recreated the taskbar entry
    }
    if (!EnsureIcons()) return E_OUTOFMEMORY;
    auto buttons = Buttons(state, labels);
    const HRESULT result = taskbar_->ThumbBarAddButtons(window_, static_cast<UINT>(buttons.size()), buttons.data());
    if (SUCCEEDED(result)) {
        added_ = have_state_ = true;
        state_ = state;
        labels_ = labels;
    }
    return result;
}

HRESULT TaskbarPlaybackControls::Update(
    const TaskbarPlaybackState& state, const TaskbarPlaybackLabels& labels) {
    if (!taskbar_ || !added_) return S_FALSE;
    if (!EnsureIcons()) return E_OUTOFMEMORY;
    if (have_state_ && state == state_ && labels == labels_) return S_FALSE;
    auto buttons = Buttons(state, labels);
    const HRESULT result = taskbar_->ThumbBarUpdateButtons(window_, static_cast<UINT>(buttons.size()), buttons.data());
    if (SUCCEEDED(result)) {
        have_state_ = true;
        state_ = state;
        labels_ = labels;
    }
    return result;
}

TaskbarPlaybackAction TaskbarPlaybackControls::DecodeClick(
    WPARAM wparam, const TaskbarPlaybackState& state) noexcept {
    if (HIWORD(wparam) != THBN_CLICKED) return TaskbarPlaybackAction::none;
    const auto action = static_cast<TaskbarPlaybackAction>(LOWORD(wparam));
    if ((action == TaskbarPlaybackAction::previous && state.previous_enabled) ||
        (action == TaskbarPlaybackAction::play_pause && state.play_pause_enabled) ||
        (action == TaskbarPlaybackAction::next && state.next_enabled)) return action;
    return TaskbarPlaybackAction::none;
}
} // namespace ttplayer::ui
