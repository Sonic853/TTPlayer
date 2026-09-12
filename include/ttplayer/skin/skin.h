#pragma once

#include "ttplayer/skin/skin_image.h"

#include <filesystem>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace ttplayer::skin {
// The first 0x24 bytes of TTPlayer's private skin-package object are the
// package identity and the metadata parsed from the root <skin> element by
// CSkinParser_ParseMetadata (004A8315).  Keep these fields independent from
// the materialized GDI layout so the skin menu can inspect every package
// without extracting hundreds of bitmaps.
struct SkinMetadata {
    int version{};
    std::wstring name;
    std::wstring author;
    std::wstring url;
    std::wstring email;
    COLORREF transparent_color{CLR_INVALID};
};

// Parses the same permissive legacy XML dialect used by LegacySkin::Load.
// A missing/invalid document or any root version other than 2 is rejected,
// matching CSkinManager_LoadPackageXml -> CSkinParser_ParseMetadata.
[[nodiscard]] std::optional<SkinMetadata> ParseLegacySkinMetadata(
    std::span<const unsigned char> xml_bytes);

struct SkinElement {
    std::wstring name;
    RECT bounds{};
    SkinImage image{};
    SIZE image_size{};
    int frames{1};
    bool four_state{};
    bool vertical{};
    SkinImage bar_image{};
    SIZE bar_size{};
    SkinImage fill_image{};
    SIZE fill_size{};
    SkinImage fill_image2{};
    SIZE fill_size2{};
    SkinImage thumb_image{};
    SIZE thumb_size{};
    SkinImage flash_image{};
    SIZE flash_size{};
    SkinAnimation animation;
    COLORREF color{RGB(255, 255, 255)};
    COLORREF background{0xff000000};
    unsigned int alignment{};
    std::wstring font{L"Tahoma"};
    int font_size{12};
};

struct SkinBitmap {
    SkinImage image{};
    SIZE size{};
};

// CSkinManager keeps the ordinary player_window and mini_window descriptions
// in the same package object.  They use the same element grammar but have
// independent backgrounds, native sizes and control coordinates.
struct PlayerSkinLayout {
    SkinImage background{};
    SIZE window_size{};
    std::vector<SkinElement> elements;
};

// Layout recovered from CSkinParser_ParsePlaylistWindow (004A8DC6).
// The original keeps this window in the same skin object as player_window;
// consequently these handles have the same lifetime as LegacySkin.
struct PlaylistSkin {
    bool valid{};
    RECT position{};
    RECT resize_rect{};
    bool resize_tile{};
    SkinBitmap background;
    SkinElement title;
    SkinElement close;
    RECT toolbar_bounds{};
    unsigned int toolbar_alignment{};
    SkinBitmap toolbar;
    SkinBitmap toolbar_hot;
    SkinAnimation toolbar_animation;
    RECT list_bounds{};
    SkinBitmap selected;
    SkinBitmap splitter_bar;
    SkinBitmap splitter_arrow;
    SkinBitmap scrollbar_buttons;
    SkinBitmap scrollbar_thumb;
    SkinBitmap scrollbar_bar;
    // XML thumb_resize_center is the height of the stretch/tile source
    // segment, not a boolean (00488463 reads it as an integer at +0xEB4).
    int scrollbar_thumb_resize_center{};
    bool scrollbar_thumb_resize_tile{};
    std::wstring font{L"SimSun"};
    int font_height{-13};
    COLORREF text_color{RGB(180, 180, 180)};
    COLORREF highlight_color{RGB(30, 30, 30)};
    COLORREF background_color{RGB(59, 62, 67)};
    COLORREF number_color{RGB(180, 180, 180)};
    COLORREF duration_color{RGB(180, 180, 180)};
    COLORREF selected_color{RGB(255, 255, 255)};
    std::optional<COLORREF> selected_text_color; // 6.1.2 Playlist.xml Color_SelText
    COLORREF alternate_background_color{RGB(44, 47, 51)};
};

// CSkinParser_ParseEqualizerWindow (004A8B73) stores the equalizer as a
// separate owned popup layout.  eqfactor is a template: the native parser
// clones it ten times and advances by template width + eq_interval.
struct EqualizerSkin {
    bool valid{};
    RECT position{};
    int eq_interval{};
    SkinBitmap background;
    SkinElement title;
    SkinElement close;
    SkinElement enabled;
    SkinElement profile;
    SkinElement reset;
    SkinElement balance;
    SkinElement surround;
    SkinElement preamp;
    std::array<SkinElement, 10> bands;
};

// CSkinParser_ParseLyricWindow (004A88D0) parses the popup chrome from
// Skin.xml, while CSkinManager_LoadPackageXml loads the Lyric.xml descriptor
// used by the child LyricCtrl.  They are one runtime object and must switch
// together with the package.
struct LyricSkin {
    bool valid{};
    RECT position{};
    RECT resize_rect{};
    bool resize_tile{};
    SkinBitmap background;
    SkinElement title;
    SkinElement close;
    SkinElement ontop;
    SkinElement desklrc;
    RECT lyric_bounds{};
    COLORREF mini_border_left_top{0xff000000};
    COLORREF mini_border_right_bottom{0xff000000};
    LOGFONTW font{};
    COLORREF text_color{RGB(255, 255, 255)};
    COLORREF highlight_color{RGB(154, 234, 255)};
    COLORREF background_color{RGB(21, 75, 126)};
};

// CSkinParser_ParseDeskLrcBar (004A92A2) parses a third lyric-owned popup
// independently from lyric_window.  Every command image is a horizontal
// four-frame strip and the bar background uses its own colour key.
struct DesktopLyricBarSkin {
    bool valid{};
    SkinBitmap background;
    COLORREF transparent_color{RGB(255, 0, 255)};
    RECT icon{};
    SkinElement play;
    SkinElement pause;
    SkinElement previous;
    SkinElement next;
    SkinElement list;
    SkinElement settings;
    SkinElement karaoke;
    SkinElement lines;
    SkinElement lock;
    SkinElement ontop;
    // Optional compatibility extensions used by the Baidu 8.2 toolbar.
    SkinElement zoom_in;
    SkinElement zoom_out;
    SkinElement return_to_window;
    SkinElement close;
};

// Visual.xml is a sparse package override.  CSkinManager first seeds its
// 0x90-byte visual descriptor from the current settings and then overwrites
// only attributes present in /ttplayer_visual/Visual (FUN_0048E0DC/0048E152).
struct VisualSkin {
    bool valid{};
    std::optional<COLORREF> spectrum_top_color;
    std::optional<COLORREF> spectrum_bottom_color;
    std::optional<COLORREF> spectrum_middle_color;
    std::optional<COLORREF> spectrum_peak_color;
    std::optional<int> spectrum_wide;
    std::optional<int> blur_speed;
    std::optional<int> type;
    std::optional<bool> blur;
    std::optional<COLORREF> blur_scope_color;
    std::optional<COLORREF> text_color;
    std::optional<LOGFONTW> font;
};

class LegacySkin {
public:
    static LegacySkin Load(const std::filesystem::path& directory);
    LegacySkin() = default;
    ~LegacySkin();
    LegacySkin(const LegacySkin&) = delete;
    LegacySkin& operator=(const LegacySkin&) = delete;
    LegacySkin(LegacySkin&& other) noexcept;
    LegacySkin& operator=(LegacySkin&& other) noexcept;

    [[nodiscard]] bool Valid() const noexcept { return background_ != nullptr; }
    [[nodiscard]] const SkinImage& Background() const noexcept { return background_; }
    [[nodiscard]] HICON Icon() const noexcept { return icon_; }
    [[nodiscard]] SIZE WindowSize() const noexcept { return window_size_; }
    [[nodiscard]] COLORREF TransparentColor() const noexcept { return transparent_color_; }
    [[nodiscard]] const std::vector<SkinElement>& Elements() const noexcept { return elements_; }
    // FUN_004530FD tests the first pointer at skin offset +0x4A0.  That field
    // is the loaded mini_window background, so a node with a missing/invalid
    // image does not advertise mini-player support.
    [[nodiscard]] bool SupportsMiniMode() const noexcept {
        return mini_.background != nullptr;
    }
    [[nodiscard]] bool MiniValid() const noexcept { return SupportsMiniMode(); }
    [[nodiscard]] const SkinImage& MiniBackground() const noexcept { return mini_.background; }
    [[nodiscard]] SIZE MiniWindowSize() const noexcept { return mini_.window_size; }
    [[nodiscard]] const std::vector<SkinElement>& MiniElements() const noexcept {
        return mini_.elements;
    }
    [[nodiscard]] const PlaylistSkin& Playlist() const noexcept { return playlist_; }
    [[nodiscard]] const EqualizerSkin& Equalizer() const noexcept { return equalizer_; }
    [[nodiscard]] const LyricSkin& Lyric() const noexcept { return lyric_; }
    [[nodiscard]] const DesktopLyricBarSkin& DesktopLyricBar() const noexcept {
        return desktop_lyric_bar_;
    }
    [[nodiscard]] const VisualSkin& Visual() const noexcept { return visual_; }
    [[nodiscard]] const SkinElement* Find(std::wstring_view name) const;
    [[nodiscard]] const SkinElement* FindMini(std::wstring_view name) const;
    [[nodiscard]] HRGN CreateWindowRegion(bool mini = false) const;

    // Cache and layouts share image ownership, including across rebinds.
    SkinImage LoadBitmap(const std::filesystem::path& path);

private:
    std::unordered_map<std::wstring, SkinImage> bitmaps_;
    SkinImage background_{};
    HICON icon_{};
    SIZE window_size_{};
    COLORREF transparent_color_{RGB(255, 0, 255)};
    std::vector<SkinElement> elements_;
    PlayerSkinLayout mini_;
    LyricSkin lyric_;
    DesktopLyricBarSkin desktop_lyric_bar_;
    PlaylistSkin playlist_;
    EqualizerSkin equalizer_;
    VisualSkin visual_;
};
} // namespace ttplayer::skin
