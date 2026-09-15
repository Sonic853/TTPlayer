#include "ttplayer/ui/player_window.h"
#include "ttplayer/ui/media_library_playback.h"
#include "player_window_internal.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace ttplayer::testing {
namespace {
void Require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
settings::Settings QuietSettings() {
    settings::Settings settings;
    settings.general.fade_windows = settings.general.tray_icon = false;
    settings.general.send_title_to_msn = false;
    settings.lyric.auto_download = settings.lyric.auto_load_lyric = false;
    settings.player.play_follow_cursor = false;
    settings.player.mute = true;
    return settings;
}
}

struct ProgressSeekAccess {
    struct Fixture {
        ui::PlayerWindow p{QuietSettings()};
        explicit Fixture(bool library = false, size_t count = 3) {
            // No Create(), runtime files, decoder, output device or user profile.
            // The existing deferred-play path lets the real command handlers
            // publish their chosen track without opening an audio source.
            p.window_ = CreateWindowExW(0, L"STATIC", L"navigation test", 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
            Require(p.window_ != nullptr, "cannot create test message window");
            p.playlists_.NewList(L"navigation");
            for (size_t row = 0; row < count; ++row) {
                playlist::Track track;
                track.path = L"navigation-fixture-" + std::to_wstring(row) + L".wav";
                p.ActivePlaylist().Add(std::move(track));
            }
            p.settings_.playlist.library_mode = library;
            p.media_library_playback_active_ = library;
            if (library) {
                p.media_library_playback_ = ui::BuildMediaLibraryPlaybackSnapshot(
                    p.ActivePlaylist().Tracks(), 0);
            } else {
                p.playing_playlist_index_ = p.playlists_.ActiveIndex();
            }
            p.natural_completion_dispatch_ = true;
        }
        ~Fixture() {
            p.audio_.state_ = audio::PlaybackState::stopped;
            DestroyWindow(p.window_);
            p.window_ = nullptr;
        }
        void Playing(std::optional<size_t> row) {
            p.PlaybackPlaylist().SetPlayingRow(row);
            p.current_ = row;
            p.pending_natural_play_ = false;
        }
        void Drain() {
            const auto deadline = GetTickCount64() + 10000;
            while (!p.random_navigation_requests_.empty()) {
                p.PollRandomNavigation();
                Require(GetTickCount64() < deadline, "random worker did not finish");
                if (!p.random_navigation_requests_.empty()) Sleep(1);
            }
        }
        void Complete() { p.AdvanceAfterNaturalEnd(); Drain(); }
        void Command(bool next, bool context = false) {
            using namespace ui::detail;
            if (context) {
                Require(p.HandleContextCommand(next ? kCmdNext : kCmdPrevious),
                        "context playback command was not handled");
            } else {
                p.HandleMessage(WM_COMMAND, next ? kNext : kPrevious, 0);
            }
            Drain();
        }
    };

    static void Run() {
        for (bool library : {false, true}) {
            for (int mode : {1, 0, 2, 3}) {
                for (bool context : {false, true}) {
                    for (bool next : {false, true}) {
                        Fixture f(library);
                        f.p.settings_.player.play_mode = mode;
                        // Explicit controls wrap independently of both options.
                        f.p.settings_.player.auto_switch_list = context;
                        for (size_t row = 0; row < 3; ++row) {
                            for (auto state : {audio::PlaybackState::stopped,
                                               audio::PlaybackState::playing,
                                               audio::PlaybackState::paused}) {
                                f.Playing(row);
                                f.p.audio_.state_ = state;
                                f.Command(next, context);
                                const size_t expected = next ? (row + 1) % 3 : (row + 2) % 3;
                                Require(f.p.current_ == expected,
                                        "manual previous/next must advance and wrap in nonrandom modes");
                                Require(f.p.pending_natural_play_, "manual command did not request playback");
                                Require(f.p.settings_.player.play_mode == mode,
                                        "manual command changed the persisted playback mode");
                                Require(f.p.media_library_playback_active_ == library,
                                        "manual command changed the playback owner");
                            }
                        }
                        f.Playing(std::nullopt);
                        f.p.current_ = 1; // UI/restored selection is not CPlayList +0x1c.
                        f.Command(next, context);
                        Require(f.p.current_ == (next ? 0U : 2U),
                                "without a playing marker next starts first and previous starts last");
                        f.Playing(1);
                        f.p.current_ = 0;
                        f.Command(next, context);
                        Require(f.p.current_ == (next ? 2U : 0U),
                                "navigation ignored the playlist playing marker");
                    }
                }
            }
            for (int mode = 0; mode <= 4; ++mode) for (size_t count : {size_t{0}, size_t{1}}) {
                for (bool next : {false, true}) {
                    Fixture f(library, count);
                    f.p.SetPlaybackMode(mode);
                    f.Playing(count ? std::optional<size_t>{0} : std::nullopt);
                    f.Command(next);
                    Require(f.p.pending_natural_play_ == (count != 0),
                            "empty/single-item navigation requested the wrong playback action");
                    Require(f.p.current_ == (count ? std::optional<size_t>{0} : std::nullopt),
                            "empty/single-item navigation selected an invalid row");
                }
            }
            Fixture f(library);
            f.p.settings_.player.play_mode = 1;
            f.Playing(1);
            f.Complete();
            Require(f.p.current_ == size_t{1} && f.p.pending_natural_play_,
                    "repeat-one natural completion must still restart the same track");
            f.p.settings_.player.play_mode = 2;
            f.Playing(2);
            f.Complete();
            Require(f.p.current_ == size_t{2} && !f.p.pending_natural_play_,
                    "sequential natural completion must not wrap");
            f.p.settings_.player.play_mode = 0;
            f.Playing(1);
            f.Complete();
            Require(!f.p.pending_natural_play_, "single natural completion did not stop");
        }

        Fixture f;
        f.p.settings_.player.play_mode = 1;
        for (bool follow : {false, true}) for (bool next : {false, true}) {
            f.Playing(1);
            f.p.settings_.player.play_follow_cursor = follow;
            f.p.playlist_selection_ = 0;
            f.p.playlist_selected_rows_ = {0};
            f.Command(next);
            Require(f.p.current_ == (next && !follow ? 2U : 0U),
                    "only next with follow-cursor enabled may prefer the selected focus");
        }
        f.Playing(1);
        f.p.playlist_selection_ = 0;
        f.p.playlist_selected_rows_.clear();
        f.Command(true);
        Require(f.p.current_ == size_t{2}, "an unselected caret must not override next");
        f.Playing(1);
        f.p.playlist_selection_ = 1;
        f.p.playlist_selected_rows_ = {1};
        f.Command(true);
        Require(f.p.current_ == size_t{2}, "next must advance when focus is already on the playing item");

        CheckRandomOrder();
        CheckAsyncRandomNavigation();
        CheckCompletionMatrix();
        CheckCatalogues();
        std::cout << "all modes: commands, button states, completion, cursor, random history/rounds, "
                     "preview, catalogue switching and empty/single lists passed\n";
    }

    static void CheckRandomOrder() {
        using Order = playlist::RandomPlaybackOrder;
        using Request = Order::Request;
#if defined(TTPLAYER_LEGACY_WINDOWS)
        Require(Order::kThreeRoundLimit == 0, "XP/Win7 must always use one shuffled cycle");
#else
        Require(Order::kThreeRoundLimit == 5000, "modern three-round cutoff must be 5000 inclusive");
#endif
        // All rounds are permutations; keep two actual rounds across reversals.
        for (size_t count : {size_t{2}, size_t{3}, size_t{13}, size_t{5000}, size_t{5001}}) {
            Order order;
            std::optional<size_t> current = 0;
            std::vector<std::vector<size_t>> rounds;
            for (size_t round = 0; round < 3; ++round) {
                std::vector<size_t> visited;
                if (round == 0) visited.push_back(*current);
                while (visited.size() < count) {
                    auto next = order.Select(count, current, Request::next);
                    Require(next && *next < count && next != current, "random boundary repeated or out of bounds");
                    visited.push_back(*next); current = next;
                }
                Require(order.AtEnd(count), "random completion did not detect end of round");
                auto unique = visited;
                std::sort(unique.begin(), unique.end());
                for (size_t row = 0; row < count; ++row)
                    Require(unique[row] == row, "random round was not a complete permutation");
                if (!rounds.empty()) {
                    if (count > Order::kThreeRoundLimit || count == 2)
                        Require(visited == rounds.front(), "single shuffled cycle changed its order");
                    else for (const auto& earlier : rounds)
                        Require(visited != earlier, "retained shuffled rounds must have different orders");
                }
                rounds.push_back(std::move(visited));
            }
            if (count > 5000)
                Require(!std::is_sorted(rounds[0].begin(), rounds[0].end()), "large-list fallback became sequential");
            // Back through the current round, cross into the prior round, then
            // return forward. Crossing must not regenerate the retained future.
            for (size_t offset = count - 1; offset > 0; --offset) {
                current = order.Select(count, current, Request::previous);
                Require(current == rounds[2][offset - 1], "previous lost current round");
            }
            current = order.Select(count, current, Request::previous);
            Require(current == rounds[1].back(), "previous lost preceding round at boundary");
            for (size_t row = 0; row < count; ++row) {
                current = order.Select(count, current, Request::next);
                Require(current == rounds[2][row], "forward traversal lost retained round after reversal");
            }
            auto preview = order.Select(count, current, Request::preview_next);
            Require(preview && preview != current, "preview did not cross round boundary");
            Require(order.Select(count, current, Request::next) == preview, "preview consumed next song");
            current = preview;
            order.Reset();
            preview = order.Select(count, current, Request::initialize_preview);
            Require(preview && preview != current, "initial preview repeated current anchor");
            Require(order.Select(count, current, Request::preview_next) == preview, "preview changed successor");
            Require(order.Select(count, current, Request::next) == preview, "initial preview consumed successor");
            current = preview;
            order.Reset();
            auto previous = order.Select(count, current, Request::previous);
            Require(previous && previous != current, "first previous did not enter previous shuffled round");
            Require(order.Select(count, previous, Request::next) == current, "previous/next did not restore anchor");
        }
        // Supersede an in-flight large build, then repeatedly cross the cutoff.
        Order superseded;
        superseded.Prepare(2000000, 0);
        superseded.SetContext(2, 1);
        for (size_t count : {size_t{5001}, size_t{5000}, size_t{13}, size_t{5001}, size_t{3}}) {
            superseded.Prepare(count, 0);
            superseded.SetContext(2, count);
            auto next = superseded.Select(count, 0, Request::next);
            Require(next && *next > 0 && *next < count, "stale job published into replacement list");
        }
        superseded.SetContext(3, 3);
        Require(!superseded.AtEnd(3), "same-sized source switch retained old cursor");
        Require(!superseded.Select(0, {}, Request::next), "empty random list selected a row");
        Require(superseded.Select(1, 0, Request::previous) == size_t{0}, "single random track did not repeat");
        // Destruction cancels a large unfinished job without touching UI objects.
        { Order closing; closing.Prepare(2000000, 0); }
        // The player routes manual and automatic movement through one order.
        for (bool library : {false, true}) {
            Fixture f(library, 7);
            f.p.SetPlaybackMode(4); f.Playing(0);
            std::set<size_t> visited{0};
            std::vector<size_t> history{0};
            for (size_t i = 0; i < 6; ++i) {
                if (i % 2) f.Complete(); else f.Command(true);
                Require(f.p.current_ && visited.insert(*f.p.current_).second,
                        "manual next and natural completion do not share the same random round");
                history.push_back(*f.p.current_);
            }
            f.Command(false);
            Require(f.p.current_ == history[5], "UI previous lost random history");
            f.Command(true);
            Require(f.p.current_ == history[6], "UI next lost its forward history");
            Require(f.p.HandlePlaylistCommand(ui::detail::kPlaylistModeShuffle), "playlist random mode command");
            Require(f.p.random_playback_order_.AtEnd(7), "playlist mode submenu unexpectedly reset the random order");
            // Main commands clear the queue even on reselecting Random.
            Require(f.p.HandleContextCommand(ui::detail::kCmdPlayModeFirst + 4), "random mode command");
            Require(!f.p.random_playback_order_.AtEnd(7), "main random mode command did not reset the order");
        }
    }

    static void CheckAsyncRandomNavigation() {
        using Order = playlist::RandomPlaybackOrder;
        using Request = Order::Request;
        for (bool library : {false, true}) {
            Fixture f(library, 5001);
            f.Playing(0); f.p.SetPlaybackMode(4);
            // Build a full reference cycle through the same core, without decoding.
            std::vector<size_t> cycle{0};
            auto current = std::optional<size_t>{0};
            for (size_t i = 1; i < 5001; ++i) {
                current = f.p.random_playback_order_.Select(5001, current, Request::next);
                Require(current.has_value(), "large UI cycle build failed");
                cycle.push_back(*current);
            }
            f.Playing(current);
            f.Command(true);
            Require(f.p.current_ == size_t{0}, "large UI list did not wrap within its shuffled cycle");
            f.Command(true);
            Require(f.p.current_ == cycle[1], "large UI fallback used list row order");
            f.Command(false); f.Command(false);
            Require(f.p.current_ == cycle.back(), "large UI previous did not wrap in shuffled cycle");
            f.Complete();
            Require(f.p.current_ == size_t{0}, "large natural completion did not share shuffled cycle");
            f.Command(false); // Last item in the internal cycle, not row 5000.
            if (!library) {
                const auto old = f.p.playlists_.ActiveIndex();
                const auto target = f.p.playlists_.NewList(L"large-cycle destination");
                playlist::Track track; track.path = L"destination.wav";
                f.p.ActivePlaylist().Add(track); f.p.playlists_.SetActive(old);
                f.p.settings_.player.auto_switch_list = true;
                f.Complete();
                Require(f.p.playlists_.ActiveIndex() == target && f.p.current_ == size_t{0},
                        "large auto-switch did not use internal cycle boundary");
            }
        }
        Fixture burst(false, 13);
        burst.Playing(0); burst.p.SetPlaybackMode(4);
        burst.p.QueueRandomNavigation(true, false);
        burst.p.QueueRandomNavigation(true, false);
        burst.p.QueueRandomNavigation(false, false);
        burst.Drain();
        const auto first = burst.p.current_;
        burst.Command(false);
        Require(burst.p.current_ == size_t{0} && first != size_t{0}, "queued clicks lost their order");
        // Exercise the real UI timer's retry path without releasing the queued
        // action through the synchronous test helper.
        burst.p.random_navigation_requests_.push_back({true, false, true});
        const auto deadline = GetTickCount64() + 10000;
        while (!burst.p.random_navigation_requests_.empty()) {
            burst.p.HandleMessage(WM_TIMER, ui::detail::kUiTimer, 0);
            Require(GetTickCount64() < deadline, "UI timer did not dispatch completed index work");
            Sleep(1);
        }
        Require(burst.p.current_ == first, "UI timer consumed wrong queued direction");
        burst.p.random_playback_order_.Reset();
        burst.p.QueueRandomNavigation(true, false);
        burst.p.Stop(); burst.p.PollRandomNavigation();
        Require(burst.p.random_navigation_requests_.empty() && !burst.p.pending_natural_play_,
                "stop did not cancel queued random navigation");

        Fixture edited(false, 3);
        edited.Playing(0); edited.p.SetPlaybackMode(4);
        edited.Command(true); edited.Command(true);
        Require(edited.p.random_playback_order_.AtEnd(3), "edit fixture did not reach end");
        const auto revision = edited.p.ActivePlaylist().OrderRevision();
        edited.p.ActivePlaylist().SetRating(0, 3);
        Require(edited.p.ActivePlaylist().OrderRevision() == revision, "tag edit invalidated index mapping");
        edited.p.ActivePlaylist().Reorder({0}, 3);
        Require(edited.p.ActivePlaylist().OrderRevision() != revision, "same-count reorder kept revision");
        edited.p.PrepareRandomPlaybackOrder();
        Require(!edited.p.random_playback_order_.AtEnd(3), "same-count reorder reused stale random order");
        edited.p.ActivePlaylist().Remove(0);
        playlist::Track replacement; replacement.path = L"replacement.wav";
        edited.p.ActivePlaylist().Add(replacement);
        edited.Command(true);
        Require(edited.p.current_ && *edited.p.current_ < 3, "same-count replacement used stale indices");
    }

    static void CheckCompletionMatrix() {
        for (bool library : {false, true}) for (int mode = 0; mode <= 4; ++mode) {
            for (size_t row = 0; row < 3; ++row) {
                Fixture f(library);
                f.p.SetPlaybackMode(mode); f.Playing(row);
                Require(f.p.NavigationEnabled(false) == (mode == 1 || mode >= 3 || row > 0), "previous button state differs from the configured wrap policy");
                Require(f.p.NavigationEnabled(true) == (mode == 1 || mode >= 3 || row < 2), "next button state differs from the configured wrap policy");
                f.Complete();
                if (mode == 0 || (mode == 2 && row == 2)) {
                    Require(!f.p.pending_natural_play_, "completion should stop without another play request");
                } else {
                    Require(f.p.pending_natural_play_, "completion failed to schedule the next play request");
                    if (mode == 1) Require(f.p.current_ == row, "repeat one advanced on completion");
                    if (mode == 2 || mode == 3) Require(f.p.current_ == (row + 1) % 3, "ordered completion chose the wrong row");
                    if (mode == 4) Require(f.p.current_ != row, "random completion immediately repeated its anchor");
                }
            }
            Fixture idle(library);
            idle.p.SetPlaybackMode(mode); idle.Playing(std::nullopt);
            Require(idle.p.NavigationEnabled(true) && idle.p.NavigationEnabled(false) == (mode == 1 || mode >= 3),
                    "missing playing marker produced incorrect button states");
            for (bool selected : {false, true}) {
                Fixture f(library);
                f.p.SetPlaybackMode(mode); f.Playing(1);
                f.p.settings_.player.play_follow_cursor = true;
                f.p.playlist_selection_ = 0;
                if (selected) f.p.playlist_selected_rows_ = {0};
                f.Complete();
                if (mode == 0) Require(!f.p.pending_natural_play_, "single mode followed the cursor on completion");
                else if (selected) Require(f.p.current_ == size_t{0}, "completion did not follow selected focus");
                else if (mode != 4) Require(f.p.current_ == (mode == 1 ? 1U : 2U), "unselected caret overrode completion");
            }
            for (size_t count : {size_t{0}, size_t{1}}) {
                Fixture f(library, count); f.p.SetPlaybackMode(mode);
                f.Playing(count ? std::optional<size_t>{0} : std::nullopt);
                f.Complete();
                Require(f.p.pending_natural_play_ == (count == 1 && (mode == 1 || mode >= 3)),
                        "empty/single list natural completion differs from original");
            }
        }
        for (int mode : {1, 2}) {
            Fixture f(false, 0);
            f.p.SetPlaybackMode(mode);
            playlist::Track detached; detached.path = L"retained-source.wav";
            f.p.opened_track_ = detached;
            f.Complete();
            Require(f.p.pending_natural_play_, "empty list lost repeat of its detached open source");
        }
        Fixture interval;
        interval.p.SetPlaybackMode(2); interval.Playing(0);
        interval.Complete();
        Require(interval.p.PlaybackPlaylist().PlayingRow() == size_t{1}, "track interval did not publish the chosen marker");
        interval.Command(true);
        Require(interval.p.current_ == size_t{2}, "next during track interval used the stale playing marker");
    }

    static void CheckCatalogues() {
        for (int mode = 0; mode <= 4; ++mode) for (bool automatic : {false, true}) {
            Fixture f;
            const auto first = f.p.playlists_.ActiveIndex();
            const auto second = f.p.playlists_.NewList(L"second");
            playlist::Track track; track.path = L"second-list.wav";
            f.p.ActivePlaylist().Add(track);
            f.p.playlists_.SetActive(first);
            f.p.SetPlaybackMode(mode); f.p.settings_.player.auto_switch_list = automatic;
            f.Playing(0);
            if (mode == 4) { f.Command(true); f.Command(true); }
            else f.Playing(2);
            f.p.pending_natural_play_ = false;
            f.Complete();
            const bool switches = automatic && mode >= 2;
            Require(f.p.playlists_.ActiveIndex() == (switches ? second : first),
                    "mode/AutoSwitchList selected the wrong catalogue on completion");
            if (switches) Require(f.p.current_ == size_t{0} && f.p.pending_natural_play_, "next catalogue did not start at row zero");
        }
        for (int mode : {2, 3, 4}) {
            Fixture f;
            f.p.SetPlaybackMode(mode); f.p.settings_.player.auto_switch_list = true;
            f.Playing(0);
            if (mode == 4) { f.Command(true); f.Command(true); } else f.Playing(2);
            f.p.pending_natural_play_ = false;
            f.Complete();
            Require(f.p.pending_natural_play_ == (mode != 2), "one-list catalogue wrap/stop is incorrect");
            if (mode != 2) Require(f.p.current_ == size_t{0}, "wrapped catalogue must start at its first song");
        }
        Fixture empty;
        const auto first = empty.p.playlists_.ActiveIndex();
        const auto destination = empty.p.playlists_.NewList(L"empty");
        empty.p.playlists_.SetActive(first); empty.Playing(2);
        empty.p.SetPlaybackMode(3); empty.p.settings_.player.auto_switch_list = true;
        empty.p.fullscreen_mode_ = 1;
        empty.Complete();
        Require(empty.p.playlists_.ActiveIndex() == destination && !empty.p.pending_natural_play_,
                "empty destination reopened the previous song or was skipped");
        Require(empty.p.fullscreen_mode_ == 1, "empty destination incorrectly took the end-of-catalogue exit branch");
        empty.p.fullscreen_mode_ = 0;

        // A different displayed list owns traversal; repeat-one still owns
        // the opened song in the previous list (0045BF4B).
        for (int mode : {1, 2}) {
            Fixture f; const auto owner = f.p.playlists_.ActiveIndex(); f.Playing(1);
            const auto shown = f.p.playlists_.NewList(L"shown");
            playlist::Track t; t.path = L"shown.wav"; f.p.ActivePlaylist().Add(t);
            f.p.SetPlaybackMode(mode); f.Complete();
            Require(f.p.playing_playlist_index_ == (mode == 1 ? owner : shown),
                    "completion confused displayed catalogue and opened-song ownership");
        }
    }
};
}

int main() {
    try {
        ttplayer::testing::ProgressSeekAccess::Run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "playback navigation: " << error.what() << '\n';
        return 1;
    }
}
