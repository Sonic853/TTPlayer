# Discord music presentation

This is a rebuilt-player extension, not a recovered Discord feature of the
original TTPlayer. Album artwork is deliberately deferred at the user's request:
no `assets`, image lookup, upload service or artwork setting is added.

## Display contract

All non-stopped activities use `type: 2` (Listening) and
`status_display_type: 2` (Details). The member list uses the song/program title;
the registered application name and XML-only Application ID remain unchanged.

| Source/state | Details | State | Timestamps |
| --- | --- | --- | --- |
| Known-length song, playing | Song title | Artist · 专辑：Album | Start and end |
| Known-length song, paused | Song title | 已暂停 · 01:23 / 04:56 · Artist · 专辑：Album | None |
| Unknown-length local audio | Title or filename stem | 时长未知 · Artist · 专辑：Album | Start only |
| Network audio without station metadata | Title, or 网络音频 | 网络音频 [· 时长未知] · music fields | End only when duration is known; always a start while playing |
| Radio with unknown length | Program title, or station name | 电台直播 · Station · music fields | Start only |
| Radio paused, unknown length | Program title | 已暂停 · 已收听 10:00 · Station · music fields | None |
| Stopped, disabled, normal exit | Cleared with `activity: null` | — | — |

Unknown-length non-radio pauses display `已暂停 · 01:23 / 未知时长`.
Clocks longer than an hour use `H:MM:SS`. Negative positions become zero;
known-length positions are bounded by duration. A radio item with a known
duration is labeled `电台`, not `电台直播`.

Titles get a Unicode-safe ellipsis at the 128-character limit. State layout
reserves the pause/time prefix and fairly distributes remaining space among
station, artist and album, keeping short fields intact and marking truncated
fields with an ellipsis. Album text is explicitly labeled because this revision
has no cover-hover area. Empty fields do not produce stray separators.

## Player metadata adapter

`BuildDiscordTrackPresence` reads the **playing item**, not the selected row.
It uses existing title/artist/album fields and their metadata fallbacks. For
network media it additionally recognizes the following aliases, ignoring ASCII
case and punctuation in keys:

- Station: `icy-name`, `StationName`, `RadioStationName`,
  `WM/RadioStationName`, `Station`.
- Program/song title: `StreamTitle`, `icy-title`, `NowPlaying`.
- Stream-specific music tags: `StreamArtist`, `StreamAlbum`.

A URL alone is not evidence that an item is a live radio station. Known-length
remote songs retain their end time; unknown-length network audio without a
station tag stays labeled `网络音频 · 时长未知`. URL-valued fallback titles and
station names are not published, to avoid exposing credentials/query tokens.

This adapter does not implement an HTTP/ICY client or a new decoder ABI. Live
program changes can be displayed when the playback item's metadata is updated;
it cannot manufacture dynamic metadata a reader does not supply. Existing CUE
subtrack titles and durations pass through without substituting the container's
filename/duration. Missing/invalid UTF-8 tags fall back safely.

## Clock integration

The sender always retains the latest audio sample, independently of whether a
visible update needs publishing. After every blocking connection/READY handshake
it reads a new snapshot, including any intervening track change, seek, pause or
disable. Before sending it projects that sample forward using the monotonic
clock; paused and pending-seek snapshots stay fixed. Retry backoff is separate
from activity changes, so lyric changes cannot spin connection retries. Unknown
durations emit a start-only elapsed timer, never a fake end time. Local RPC
timestamps remain Unix **seconds**.

`AudioEngine::ClockSnapshot` reads the accepted position and seek revision under
the same lock. Presence uses this local-only revision and path/subtrack identity,
not title equality or a three-second seek threshold. Main/mini progress release,
lyric drag release and lyric-line keyboard seek immediately update presence;
other seek sources are observed by the ordinary UI timer. Decoder acknowledgement
also refreshes the clock basis. The existing three-second tolerance is only a
fallback for unannounced clock drift, not a gate on explicit seeks. Subsecond
seeks trigger IPC updates, although the rendered RPC timer is second-granularity.

The native PCM output loop previously used `min(duration, position)` even when
duration was zero. `BoundPlaybackClock` now caps only known positive durations,
for DirectSound and the shared waveOut/KS/ASIO clock. Unknown-duration EOF does
not reset the last observed position to zero, and end-of-track fade is disabled
until a positive endpoint exists. This does not add seeking to unseekable streams.

## Synchronized lyric lines

`General/@DiscordSyncLyrics` defaults to `1` and is saved in `TTPlayer.xml`.
Set it to `0` to disable lyric sharing without disabling song presence. It remains
subordinate to `SendTitleToDiscord`; the Application ID is still XML-only.

The integration shares only the current line from the player's loaded `Lyrics`
object, not the entire LRC file or its pathname. `Lyrics::LineAt` supplies the same
timestamp/offset behavior as the player. It works with hidden, normal, mini,
fullscreen or desktop lyric windows because selection is not tied to any HWND.
Paused playback retains the current line and fixed position; seeking selects the
new line. Before the first timestamp, on blank/invalid lines, or with no lyrics,
the state falls back to the ordinary music fields. Each selected line carries its
validity interval so an expired line is not sent after delayed I/O or projection.
Manual time adjustments and reloaded lyrics are observed on the next UI refresh.

The current line is prefixed by `♪` in `state`, while `details` and member-list
text continue showing the song title. The pause/time prefix stays intact; the
lyric receives three shares of the remaining text budget versus one each for
station/artist/album. Unicode-safe truncation keeps the 128-character boundary.

Ordinary lyric changes are coalesced to at most one send per two seconds, always
using the newest line at the deadline. No queue replays intermediate lyrics after
a disconnect or rapid passage. Seeks, playback-state changes and clear bypass
that lyric-only delay. This is a local scheduling policy, **not** a documented
Discord rate-limit guarantee. Discord may throttle/profile-cache updates, so this
is best-effort line sharing, not a native karaoke surface or guaranteed word-level
synchronization. No new lyrics downloads, artwork, music links, playback controls
or synchronized listening sessions are introduced.

The existing RPC error-response parser is outside this change.

## Verification

Host Release build and all 29 CTest cases passed on 2026-09-11 (30.21 s),
including the new `discord_sync_tests`.

`discord_presence_tests` covers the production track adapter, member-list field,
finite/unknown/live/paused layouts, pause seeks, source/station changes, Unicode
and long text, missing/invalid tags, private-URL fallback, and reconnect clock
projection. Its named-pipe integration test uses a per-process private endpoint
and structured acknowledgements, never a real Discord IPC slot; pipe failures
are test failures rather than successful skips. `progress_seek_tests` additionally
checks unknown-duration versus known-duration end-of-track gain.

`discord_sync_tests` uses real private-pipe disconnect/reconnect and delayed READY
handshakes. It covers tiny forward/backward seeks, pending/acknowledged targets,
same-title track identity changes, latest samples even when deduplicated, pause
and disable during handshake, clock projection on reconnect, lyric coalescing,
seek priority, blank lines, LRC offsets, manual adjustment and expired lines.

These tests validate player output/IPC, not another account's rendered Discord
profile. Real-client presentation remains dependent on the client and its
activity-sharing settings.

Official references:

- [Discord activity fields and status display types](https://docs.discord.com/developers/events/gateway-events#activity-object)
- [Local RPC SET_ACTIVITY and its timestamp example](https://docs.discord.com/developers/topics/rpc#set-activity)
- [Rich Presence elapsed/remaining timers](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence#setting-timestamps)
