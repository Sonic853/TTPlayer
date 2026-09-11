#pragma once

#include "ttplayer/integrations/discord_presence_config.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace ttplayer::settings {
struct GeneralSettings {
    bool startup_minimize{};
    bool tray_icon{true};
    bool fade_windows{true};
    bool show_hotkey_in_tips{true};
    bool tips_on_open{};
    bool menu_tips{true};
    bool menu_bar_playlist{};
    bool scroll_title{true};
    // Kept under the original member name so old option-page code and
    // TTPlayer.xml files remain source/binary compatible.  The rebuilt
    // runtime now maps this switch to Discord Rich Presence rather than the
    // retired MSN/Baidu Hi window-message integrations.
    bool send_title_to_msn{true};
    // Current timed LRC line in Rich Presence; subordinate to the master switch.
    bool discord_sync_lyrics{true};
    // Discord requires every Rich Presence producer to use an application
    // registered in its developer portal.  This is a public identifier, not
    // a credential; private builds may replace it only in TTPlayer.xml.
    std::wstring discord_application_id{
        integrations::kDefaultDiscordApplicationId};
    // The original stores the enabled flag and numeric value together in
    // these packed integers (for example 0x1000a and 0x10005).
    int snap_windows{65546};
    int title_slide_interval{65541};
    int check_update_days{7};
    int last_message_version{131336193};
    int last_checked_day{};
    bool auto_shutdown{};
    std::array<int, 3> shutdown_time{}; // hour, minute, second
    bool clear_list_on_command{};
    bool default_list_on_command{true};
    std::wstring default_list;
    std::filesystem::path app_icon_file;
    int mp3_read_tag_priority{67633152};
    int mp3_write_tag_type{5};
    int mp3_id3v2_encoding{};
    bool mp3_id3v2_padding{true};
};
struct PlayerSettings {
    PlayerSettings();

    int volume{100};
    int balance{};
    int play_mode{2};
    bool mute{};
    bool top_most{};
    bool mini_top_most{true};
    RECT player_window{};
    RECT mini_player_window{};
    bool mini_mode{};
    RECT lyric_window{};
    bool lyric_visible{};
    bool lyric_top_most{};
    RECT mini_lyric_window{};
    bool mini_lyric_visible{};
    bool mini_lyric_top_most{};
    RECT playlist_window{};
    bool playlist_visible{};
    RECT equalizer_window{};
    bool equalizer_visible{};
    int playlist_scan_count{100};
    int active_playlist{};
    int playing_time{};
    std::wstring playing_file_name;
    int playing_file_subtrack{};
    bool auto_switch_list{};
    bool play_follow_cursor{};
    bool opaque_when_active{};
    int alpha_percent{};
    RECT desktop_lyric_window{};
    bool window_shadow{};
    bool show_elapsed_time{true};
    bool check_association{true};
    bool auto_associate{};
    bool first_run_552{true};
    std::wstring user_word;
    std::wstring user_word_md5;
};
struct PlaybackSettings {
    bool auto_play{true};
    bool continue_play{};
    bool stop_when_fail{};
    int file_buffer{16384};
    int track_interval{};
    int thread_priority{15};
    int sound_fade_mode{15};
    std::array<int, 4> fade_duration{300, 500, 800, 800};
    int track_fade_duration{5000};
    bool auto_gain{};
    bool auto_scan_gain{};
    bool skip_scan_gain{};
};
struct DeviceSettings {
    std::wstring device_type{L"{DEF00000-9C6D-47ED-AAF1-4DDA8F2B5C03}"};
    int output_bits{16};
    int buffer_duration{1000};
    bool hardware_buffer{true};
    bool create_primary{};
    int resample_rate{};
    int ssrc_mode{1};
    // 0 disables dither; 1..4 select the four legacy noise shapes shown by
    // dialog 260's combo box.
    int dither{};
};
// The five 16-bit fields serialized by FUN_004BE51F use the exact legacy
// "command:a(key,modifiers),g(key,modifiers)" KeyMap_N grammar.  command is
// intentionally an int so bindings introduced by add-ins remain round-trippable.
struct HotKeyAccelerator {
    int virtual_key{};
    int modifiers{};
};
struct HotKeyBinding {
    int command{-1};
    HotKeyAccelerator application;
    HotKeyAccelerator global;
    // A malformed value is retained verbatim instead of being discarded when
    // a newer/third-party build has extended the original KeyMap grammar.
    std::wstring raw_text;
};
struct HotKeySettings {
    HotKeySettings();

    bool global{};
    std::vector<HotKeyBinding> key_map;
};
// CSettings keeps the visualizer fields together at offsets +0x22C..+0x2B3.
// Type/FramesPerSec are global; the remaining colours, blur parameters and
// LOGFONT are also serialized into each per-skin .skn.xml profile.
struct VisualSettings {
    int type{2};                         // 0 none, 1 dream, 2 spectrum, 3 scope, 4 cover
    int frames_per_second{25};
    COLORREF spectrum_top_color{RGB(25, 77, 92)};
    COLORREF spectrum_bottom_color{RGB(25, 77, 92)};
    COLORREF spectrum_middle_color{RGB(25, 77, 92)};
    COLORREF spectrum_peak_color{RGB(25, 77, 92)};
    int spectrum_wide{1};
    int blur_speed{3};
    bool blur{true};
    COLORREF blur_scope_color{RGB(25, 77, 92)};
    COLORREF text_color{RGB(255, 255, 255)};
    LOGFONTW font{};
    bool font_valid{};
};
// CSettings +0x788..+0x7A8.  VisualType 0 uses the shared "All" profile;
// values 1..3 select the independent Goom/Spectrum/BlurScope profile.
struct FullScreenSettings {
    int visual_type{1};
    std::array<int, 4> position_relation{0, 1, 0, 1};
    std::array<int, 4> lyric_size{2, 2, 2, 2};
};
struct DesktopLyricColorProfile {
    std::wstring name;
    int background_count{3};
    std::array<COLORREF, 3> background_colors{};
    int played_count{3};
    std::array<COLORREF, 3> played_colors{};
};
// CSettings +0x434..+0x564.  This is the independent layered desktop-lyric
// subsystem (DeskLrcCtrl/Paint/Bar), not the skinned normal/mini LyricWnd.
struct DesktopLyricSettings {
    DesktopLyricSettings();

    int profile{};
    int lines{1};
    int align{};
    int background_alpha{50};
    int text_alpha{255};
    bool topmost{true};
    bool karaoke_mode{true};
    bool auto_width{};
    bool lock{};
    bool unlock_when_close{true};
    bool background_transparent{};
    bool smooth{true};
    bool border{};
    bool shadow{true};
    bool background_show{};
    LOGFONTW font{};
    bool font_valid{};
    COLORREF border_color{RGB(255,255,255)};
    COLORREF shadow_color{RGB(20,20,20)};
    COLORREF background_color{RGB(255,255,255)};
    DesktopLyricColorProfile current;
    std::array<DesktopLyricColorProfile, 3> profiles;
};
struct EqualizerSettings {
    int profile{-2};
    int profile_last{-1};
    int surround{};
    std::array<int, 11> custom{};
    std::array<int, 11> current{}; // preamp followed by ten bands
};
struct PlaylistSettings {
    std::wstring font{L"SimSun"};
    int font_height{-13};
    // Playlist profile files serialize the complete 92-byte LOGFONT tuple.
    // Keep the legacy face/height mirrors for old skin parsers, but retain
    // every other field when a .ttpl_cfg profile is imported and exported.
    LOGFONTW font_descriptor{};
    bool font_descriptor_valid{};
    COLORREF text_color{RGB(180,180,180)};
    COLORREF highlight_color{RGB(30,30,30)};
    COLORREF background_color{RGB(59,62,67)};
    COLORREF number_color{RGB(180,180,180)};
    COLORREF duration_color{RGB(180,180,180)};
    COLORREF selected_color{RGB(255,255,255)};
    COLORREF alternate_background_color{RGB(44,47,51)};
    bool legacy_playlist_generation{};
    bool library_mode{};
    bool item_tips{true};
    bool disable_delete_file{true};
    bool enable_drag_drop{true};
    int read_info_mode{};
    bool title_number{true};
    bool ignore_bad_files{};
    bool save_relative_path{true};
    bool save_tags{};
    int tag_format{1};
    bool click_rating{};
    std::wstring tag_title_format{L"%A - %T"};
    std::wstring default_title_format{L"%F"};
    int split_on_lists{55};
};
struct MediaLibraryDirectory {
    std::filesystem::path path;
    bool enabled{};
};
struct MediaLibrarySettings {
    bool enabled{};
    bool valid{};
    std::wstring playing_catalog;
    bool monitor_directories{};
    std::vector<MediaLibraryDirectory> directories;
    int max_item_count{};
};
struct NetworkSettings {
    NetworkSettings();

    int proxy_type{1};
    std::wstring proxy_server;
    int proxy_port{};
    std::wstring proxy_username;
    std::wstring proxy_password;
    bool freedb_auto_query{true};
    bool show_info_when_fail{true};
    std::wstring freedb_server{L"http://freedb.freedb.org/~cddb/cddb.cgi"};
    std::filesystem::path cache_folder;
    std::vector<std::wstring> server_list;
    bool accept_recommendation_list{true};
    int last_recommendation_list_id{-1};
    int check_update_days{7};
    int last_checked_day{};
    bool cache_enabled{true};
    int cache_space_size{600};
    int last_message_time{};
    std::filesystem::path download_folder;
    bool create_folder_by_artist{true};
    bool replace_file{};
    bool download_when_listen{};
    bool download_lyric{true};
    int max_download_tasks{3};
    int download_num{};
    int speed_mode{};
    int tip_showed_time{};
    bool remember_download_mode{};
};
// CSettings fields consumed by CConvertDlg/CWorkThread (resource 220/221).
// These values belong to the offline encoder pipeline and are deliberately
// independent from DeviceSettings, which configures real-time playback.
struct ConvertSettings {
    int writer_index{};
    int output_bits{};
    int resample_rate{};
    int replay_gain{};
    int equalizer{};
    int surround{};
    std::filesystem::path folder{L"C:\\"};
    int save_mode{1};
    int add_number{};
    int add_to_playlist{};
    int thread_priority{};
    // Optional modern LAME DLL extension; existing legacy fields retain their
    // original meanings and AddIn configuration remains owned by each DLL.
    int lame_mode{};
    int lame_bitrate{192};
    int lame_quality{2};
};
struct DspPluginSettings {
    DspPluginSettings();

    std::filesystem::path folder;
    std::vector<std::wstring> modules;
};
// The misspelt Histroy element is part of TTPlayer's persisted file format.
// CSettings keeps LastActivePage at +0x748 and the common file/folder state at
// +0x750/+0x760, independently of per-skin profiles.
struct HistorySettings {
    int last_active_page{};
    std::filesystem::path sound_path;
    std::filesystem::path playlist_path;
    std::filesystem::path folder;
    std::wstring tag_pattern;
    bool check_sub_folder{true};
    bool advance_file_info{};
    std::filesystem::path equalizer_profile;
    std::filesystem::path lyric_profile;
    std::filesystem::path playlist_profile;
    std::vector<std::wstring> tag_names;
};
struct LyricSettings {
    LyricSettings();

    LOGFONTW font{};
    bool font_valid{};
    COLORREF text_color{CLR_INVALID};
    COLORREF highlight_color{CLR_INVALID};
    COLORREF background_color{CLR_INVALID};
    int charset{};
    int scroll_mode{};
    // TTPlayer keeps three independent fields at settings offsets +0x2B4,
    // +0x2B8 and +0x344.  FUN_00401CA7 selects one for normal, mini or
    // full-screen lyrics instead of sharing the normal-window value.
    int mini_scroll_mode{1};
    int fullscreen_scroll_mode{};
    int fullscreen_text_align{1};
    int fullscreen_row_interval{4};
    int fullscreen_fade_index{10};
    bool fullscreen_fade_highlight{true};
    bool fullscreen_karaoke_mode{};
    bool fullscreen_transparent{};
    bool fullscreen_auto_font{true};
    LOGFONTW fullscreen_font{};
    bool fullscreen_font_valid{};
    COLORREF fullscreen_text_color{RGB(0,128,192)};
    COLORREF fullscreen_highlight_color{RGB(0,255,0)};
    COLORREF fullscreen_background_color{RGB(0,0,0)};
    int text_align{1};
    int row_interval{4};
    int fade_index{10};
    bool fade_highlight{true};
    bool karaoke_mode{};
    bool transparent{};
    bool transparent_skin{true};
    bool auto_load_lyric{true};
    bool auto_save_lyric_tag{};
    bool dont_load_lyric_tag{};
    bool auto_visible{};
    bool auto_width{true};
    bool auto_width_only_vertical{true};
    bool drag_lyric{true};
    bool mouse_wheel_adjust{};
    bool save_compress{};
    bool trim_spaces{true};
    int lyric_save_mode{1};
    int add_in_index{};
    bool auto_download{true};
    bool download_when_full_info{};
    bool auto_associate{};
    bool auto_select_download{};
    bool overwrite{};
    bool same_file_title{};
    bool save_to_sound_folder{};
    std::filesystem::path download_folder;
    bool new_line_after_tag{true};
    int display_mode{};
    std::vector<std::wstring> folders{
        L"*<Sound Folder>", L"*<Lyrics Download Folder>"};
};
struct Settings {
    GeneralSettings general;
    PlayerSettings player;
    PlaybackSettings playback;
    DeviceSettings device;
    HotKeySettings hotkey;
    VisualSettings visual;
    FullScreenSettings fullscreen;
    DesktopLyricSettings desktop_lyric;
    EqualizerSettings equalizer;
    PlaylistSettings playlist;
    MediaLibrarySettings library;
    LyricSettings lyric;
    NetworkSettings network;
    ConvertSettings convert;
    DspPluginSettings plugin;
    HistorySettings history;
    std::wstring skin_file;
    std::filesystem::path source_path;
};

struct StartupPlaybackPlan {
    bool should_play{};
    int resume_position_ms{};
};

[[nodiscard]] StartupPlaybackPlan MakeStartupPlaybackPlan(
    const Settings& settings) noexcept;
void ClearPlaybackIdentity(PlayerSettings& player) noexcept;

Settings LoadLegacyXml(const std::filesystem::path& path);
bool LoadPlaylistProfile(const std::filesystem::path& path,
                         PlaylistSettings& playlist);
bool LoadPlaylistOptionsProfile(const std::filesystem::path& path,
                                PlaylistSettings& playlist);
bool SavePlaylistOptionsProfile(const std::filesystem::path& path,
                                const PlaylistSettings& playlist);
bool LoadLyricOptionsProfile(const std::filesystem::path& path,
                             LyricSettings& lyric);
bool SaveLyricOptionsProfile(const std::filesystem::path& path,
                             const LyricSettings& lyric);
bool LoadSkinVisualProfile(const std::filesystem::path& path,
                           PlayerSettings& player,
                           PlaylistSettings& playlist,
                           LyricSettings& lyric,
                           VisualSettings& visual);
bool SaveSkinVisualProfile(const std::filesystem::path& path,
                           const PlayerSettings& player,
                           const PlaylistSettings& playlist,
                           const LyricSettings& lyric,
                           const VisualSettings& visual,
                           const std::filesystem::path& global_settings_path = {});
void SaveWindowState(const std::filesystem::path& path,
                     const Settings& settings);
}
