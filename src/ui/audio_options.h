#pragma once
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/settings/settings.h"

namespace ttplayer::ui::detail {

// Value snapshot shared by playback, conversion and background downloads.
// Each consumer supplies its own cancellation/lifetime after taking this copy.
inline audio::PlaybackOptions PlaybackOptionsFromSettings(
    const settings::Settings& settings, const std::filesystem::path& runtime) {
    audio::PlaybackOptions options;
    options.pcm_mode = settings.device.pcm_mode == 1 ? audio::PcmPlaybackMode::high_precision :
        settings.device.pcm_mode == 2 ? audio::PcmPlaybackMode::bit_exact : audio::PcmPlaybackMode::compatible;
    options.mp3_tags = {static_cast<std::uint32_t>(settings.general.mp3_read_tag_priority),
        static_cast<std::uint32_t>(settings.general.mp3_write_tag_type),
        static_cast<std::uint32_t>(settings.general.mp3_id3v2_encoding),
        settings.general.mp3_id3v2_padding};
    options.lyric_recovery_folder = (settings.source_path.empty() ? runtime :
        settings.source_path.parent_path()) / L"LyricRecovery";
    options.file_buffer_bytes = settings.playback.file_buffer;
    options.output_buffer_ms = settings.device.buffer_duration;
    options.output_bits = settings.device.output_bits;
    options.resample_rate = settings.device.resample_rate;
    options.thread_priority = settings.playback.thread_priority;
    options.balance = settings.player.balance;
    options.auto_gain = settings.playback.auto_gain;
    options.auto_scan_gain = settings.playback.auto_scan_gain;
    options.skip_scan_gain = settings.playback.skip_scan_gain;
    options.equalizer_profile = settings.equalizer.profile;
    options.surround = settings.equalizer.surround;
    options.equalizer_values = settings.equalizer.current;
    options.device_type = settings.device.device_type;
    options.hardware_buffer = settings.device.hardware_buffer;
    options.create_primary = settings.device.create_primary;
    options.ssrc_mode = settings.device.ssrc_mode;
    options.dither = settings.device.dither;
    options.dsp_folder = settings.plugin.folder;
    options.dsp_modules = settings.plugin.modules;
    options.sound_fade_mode = settings.playback.sound_fade_mode;
    options.fade_duration = settings.playback.fade_duration;
    options.track_fade_duration = settings.playback.track_fade_duration;
    options.network_proxy_type = settings.network.proxy_type;
    options.network_proxy_port = settings.network.proxy_port;
    options.network_proxy_server = settings.network.proxy_server;
    options.network_proxy_username = settings.network.proxy_username;
    options.network_proxy_password = settings.network.proxy_password;
    options.network_cache_enabled = settings.network.cache_enabled;
    options.network_cache_folder = settings.network.cache_folder;
    options.network_cache_space_mb = settings.network.cache_space_size;
    options.network_download_folder = settings.network.download_folder;
    options.network_download_list = (settings.source_path.empty() ? runtime :
        settings.source_path.parent_path()) / L"HttpDownloads" / L"cachelist.xml";
    return options;
}

} // namespace ttplayer::ui::detail
