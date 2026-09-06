#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>

#include <windows.h>

namespace ttplayer::ui {

// CCommandLine_FindSwitch in 004C038B/004C0DB2 writes these values into
// COPYDATASTRUCT::dwData.  /a is the ordinary append-without-play route;
// /e is the retired XML ctrlparam packet route.
enum class CommandLineFileMode : ULONG_PTR {
    open_and_play = 0,
    append = 1,
    control_packet = 2,
};

enum class OutputRestartState {
    none,
    playing,
    paused,
};

// CPlayerWnd's shutdown path (004616BD) and the shared geometry sampler
// (004684D6) both reject an iconic main window.  GetWindowRect on a minimized
// popup can expose its iconic coordinates; those must never replace the last
// normal/mini placement or the owned-window snapshot.
[[nodiscard]] constexpr bool ShouldCaptureWindowGeometry(
    bool main_window_is_iconic) noexcept {
    return !main_window_is_iconic;
}

// FUN_0045D5FA gives the embedded package a fixed Default.xml sidecar and
// appends .xml to an external package name (therefore Foo.skn.xml).  Keep the
// startup loader and shutdown writer on the same deterministic naming rule.
[[nodiscard]] inline std::filesystem::path ResolveSkinProfilePath(
    const std::filesystem::path& global_settings_path,
    std::wstring_view package_name) {
    if (global_settings_path.empty()) return {};
    const auto skin_directory = global_settings_path.parent_path() / L"Skin";
    if (package_name.empty() ||
        _wcsicmp(std::wstring(package_name).c_str(), L"<Default_Skin>") == 0) {
        return skin_directory / L"Default.xml";
    }
    auto package = std::filesystem::path(package_name).filename();
    if (package.extension().empty()) package += L".skn";
    package += L".xml";
    return skin_directory / package;
}

// Device is the only options page whose committed value cannot be applied to
// an already-open output object. Page notifications use this mapping instead
// of calling ApplyOptionsRuntime(260) directly, so the ordinary immediate,
// deferred-edit and property-page commit routes all retain FUN_0046228D's
// 0x400/0x7EB reopen semantics.
[[nodiscard]] constexpr UINT OptionsPageRuntimeChangeMask(
    UINT template_id) noexcept {
    return template_id == 260U ? 0x0400U : 0U;
}

// FUN_0046228D marks PlayerWnd+0x4364 only for Device mask 0x400 (or the
// 0xffff reset-all transaction). FUN_0045BF4B consumes that marker only while
// a current sound exists. Keeping this decision independent prevents the
// initial ApplyOptionsRuntime() call from accidentally reopening startup audio.
[[nodiscard]] constexpr OutputRestartState PlanOutputRestart(
    UINT change_mask, bool source_open, bool playing, bool paused) noexcept {
    const bool device_changed = change_mask == 0xffffU ||
                                (change_mask & 0x0400U) != 0;
    if (!device_changed || !source_open) return OutputRestartState::none;
    if (paused) return OutputRestartState::paused;
    if (playing) return OutputRestartState::playing;
    return OutputRestartState::none;
}

[[nodiscard]] constexpr CommandLineFileMode ResolveCommandLineFileMode(
    bool append_switch, bool control_packet_switch) noexcept {
    // CCommandLine_FindSwitch checks /a first, so it also wins if malformed
    // input supplies both switches.
    return append_switch ? CommandLineFileMode::append
        : control_packet_switch ? CommandLineFileMode::control_packet
                                : CommandLineFileMode::open_and_play;
}

struct PackedRuntimeOption {
    bool enabled{};
    int value{};
};

// Dialog 259 constrains Playback/@TracksInterval to 0..10 seconds.  The
// recovered player performs the wait between selecting the successor and
// submitting its decoder request, so it must be timer-driven on the UI side.
[[nodiscard]] constexpr std::uint64_t TrackIntervalMilliseconds(
    int seconds) noexcept {
    return static_cast<std::uint64_t>(std::clamp(seconds, 0, 10)) * 1000U;
}

[[nodiscard]] constexpr bool ShouldAdvanceAfterPlaybackFailure(
    bool stop_when_fail) noexcept {
    return !stop_when_fail;
}

// SnapWindows and TitleSlideInterval store their checkbox in bit 16 and the
// edit value in the low WORD (FUN_00491E98/FUN_0046228D).
[[nodiscard]] constexpr PackedRuntimeOption DecodePackedRuntimeOption(
    int packed, int minimum, int maximum) noexcept {
    return {(static_cast<unsigned int>(packed) & 0x10000U) != 0,
            std::clamp(static_cast<int>(
                static_cast<unsigned int>(packed) & 0xffffU),
                minimum, maximum)};
}

// Timer 0x0c in FUN_0046010E accepts only the configured second and its
// immediate successor.  It deliberately does not wrap across a minute.
[[nodiscard]] constexpr bool IsAutoShutdownDue(
    const SYSTEMTIME& now, const std::array<int, 3>& configured) noexcept {
    if (configured[0] < 0 || configured[0] > 23 ||
        configured[1] < 0 || configured[1] > 59 ||
        configured[2] < 0 || configured[2] > 59) {
        return false;
    }
    return now.wHour == configured[0] &&
           now.wMinute == configured[1] &&
           now.wSecond >= configured[2] &&
           now.wSecond < configured[2] + 2;
}

} // namespace ttplayer::ui
