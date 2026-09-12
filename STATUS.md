# Reconstruction status

2026-09-13: fix DEFAULT_SKIN__6120 LED/progress overlap. Follow
0045127C/004512E8's glyph-sized, left/right-anchored LED child extent for
both painting and mouse hit testing, instead of retaining the full XML
anchor as an invisible click target. Preview renders 00:00 rather than the
entire twelve-glyph strip. Cover the actual package's elapsed/remaining and
playing/paused progress drags, native-height/left-alignment rules and preview
pixels. Release builds and six targeted host regressions pass, including
clicking the visible time to toggle elapsed/remaining. No skin package or
user configuration changes.

2026-09-13: complete PNG drawing for existing skin bitmap slots. Replace the
remaining raw playlist background/splitter/scrollbar blits with SkinImage,
retain sprite/cap/tile geometry, composite visual backing and lyric button
backing before use, and flatten preview BMP keys before PNG children.
Use PNG alpha coverage for native/resized window regions with matching PNG
sampling; leave BMP colour keys unchanged. Synthetic tests exercise 532 XML
image references, three-state scrollbars and mixed BMP/PNG preview pixels;
36 PNG/75 BMP reference images and 182 local skin packages pass. Release
build and ten targeted host regressions pass; native hover passes eight
consecutive runs after waiting for actual mouse-message delivery.
See SKIN_PNG_RECOVERY.md for scope and rendering boundaries.

2026-09-13: fix desktop lyric toolbar hover flicker. Trace 5.7.9's
0040A5E3 state-change-only invalidation, 00426434 no-op erase and
0040FC78/0040A35B single-button sprite paint, distinct from the toolbar's
0041AB1F background path. Stop invalidating the entire rebuilt bar for every
mouse move; invalidate affected child rectangles only and present one
offscreen-composited BMP/PNG frame. Cover capture/outside/reentry/cancel and
enable transitions. Native host hover tests visit 12 BMP and 4 PNG controls;
metafile recording verifies no intermediate destination clears/draws.
Release builds successfully and eight targeted host regressions pass.
See DESKTOP_LYRIC_HOVER_PAINT.md for evidence and the implementation boundary.

2026-09-13: add optional per-item playlist toolbar rectangles for the approved
BaiduMusic8209 layout. Keep legacy seven-cell/two-row behavior when absent.
Use a 70px My Music header and equally divided Add/Sort/Delete/Lists/Edit/Mode
buttons, with Find inside the open-file field. Restore the missing volume
bar_image layer; align this package's fill with its 53px thumb-centre travel
so zero volume no longer leaves a visible filled stub. Preserve user skin
edits and configuration. Release/Debug, four regressions and native host
mouse tests pass; see PLAYLIST_TOOLBAR_LAYOUT.md.

2026-09-12: add Dream/Spectrum/Scope/Album as the first four commands in
the fullscreen lyric context menu, before a separator and the original
lyric commands. Reuse DLL string-list 2232, the EXE album label and the
existing 0x8086..0x8089 WM_COMMAND path. Check only the displayed combined
effect; selecting an effect from lyrics-only fullscreen enters combined
mode on the current monitor without restarting playback or losing restore
state. Native popup tests cover all labels/IDs/checks, both fullscreen lyric
modes and retained lyric/monitor menus; command tests cover all four effects
while playing/paused. See FULLSCREEN_ALBUM_BACKGROUND.md.

2026-09-12: fix normal/mini lyric text dragging. Earlier recovery mistakenly
used every timed row's text rectangle as 00442360's clickable-link hit test.
00441FEF instead tests separate prompt/link entries; 00442671 forwards their
link to 0x7F9/open. Remove this false exclusion and hand cursor from timed
rows; both text and blank space now capture and seek through the same path.
Keep normal/mini DragLyric independent of fullscreen DragLyricFS. The new
regression fails before the fix and passes afterwards; native child-window
SendInput verifies text/blank dragging, both axes, paused/playing state,
stationary clicks and cursors. See LYRIC_WINDOW_RECOVERY.md and
FULLSCREEN_LYRIC_DRAG.md (which corrects the previous text-hit interpretation).

2026-09-12: fix fullscreen lyric dragging from transparent blank areas, not
only glyphs. Colour-keyed pixels bypass the lyric HWND entirely. Add an
input-only no-redirection popup covering the lyric client rectangle, with
coordinate forwarding to the existing capture/direct-seek handler and lyric
context menu. Preserve the underlying image pixels and follow monitor,
size, visibility and z-order changes; destroy the proxy when no longer needed.
Native host SendInput tests cover text/blank dragging in both fullscreen
modes/scroll directions, paused/playing seeks, blank-area menus, unchanged
background pixels, both attached monitors and proxy lifecycle cleanup.
See FULLSCREEN_LYRIC_DRAG.md for the Windows 8+/DWM compatibility boundary.

2026-09-12: add "允许拖拽歌词" to Options / Fullscreen / Lyrics fullscreen.
Persist Lyric/DragLyricFS independently, migrating absent values from the
old shared DragLyric preference. Allow fullscreen text glyphs to initiate
capture (colour-keyed background pixels cannot), while retaining normal/
mini hit-test behaviour. Reuse direct seeking and its clock handoff; cancel
on disable, Escape, capture loss or WM_CANCELMODE without seeking. Release
and six host regressions pass. See FULLSCREEN_LYRIC_DRAG.md.

2026-09-12: add the community album-image background to combined fullscreen.
Keep 5.7.9's lyric layout/scrolling and add an independent fourth effect
profile (or share All); preserve the visual-only fullscreen modes. Album
art takes precedence over a user-picked fallback image, center-cropped with
cover fill and an image-only transparency slider. Reuse Lyric/BkgndColorFS
instead of introducing another background colour. Decode WIC first, OLE
second, cache complete frames, and publish reconfiguration/cover state under
one render lock. Original resource-263 options and fullscreen context menus
are extended without altering resource DLLs or existing user configuration.
See FULLSCREEN_ALBUM_BACKGROUND.md for persistence, scope and host tests.

2026-09-12: restore desktop-lyric track popups and state-dependent control
tips from 5.7.9. Share 00461BAE -> 004813C1 track-menu initialization with
the main window; remove NONOTIFY from desktop tracking so the root popup
actually receives WM_INITMENUPOPUP. Restore resource callback tips for all
12 toolbar controls, including play/pause, next line mode, karaoke and
topmost actions; retain native tooltip styles and no forced wrap width.
Release and eight host regressions pass, including the real production
popup callback, generated playlist commands and live tooltip text queries.
User configurations unchanged. See DESKTOP_LYRIC_MENU.md for evidence/scope.

2026-09-12: default General/DiscordSyncLyrics to off for new settings, missing
XML attributes and Reset All. Preserve explicitly saved on/off preferences;
the master Discord song-presence switch is unchanged. See DISCORD_PRESENCE.md.

2026-09-12: fix sparse per-skin profile loading. Apply target package lyric/
playlist defaults before its sidecar, then merge only specified attributes
instead of copying generic defaults from a freshly constructed Settings.
Follow 5.7.9's 0045D5FA / 004B605A ordering; retain independent Default.xml
and external .skn.xml identities and leave user configurations unchanged.
DEFAULT_SKIN_579.skn and the DLL ZIP have identical bytes (75 entries);
their existing profile differences are intentional and are not erased.
Host isolated profile/pixel regression and five existing regressions pass,
including missing, empty, malformed and partial profiles. Release builds.
See SKIN_PROFILE_LOADING.md for evidence and the verification boundary.

2026-09-12: fix Classic.skn playlist/lyric title offsets using 5.7.9's
0042912E -> 0047A9C5 alignment rules. Center within the current client using
the title image dimensions; keep right/bottom insets and per-axis undersize
guards. Preserve the parsed XML and use the shared rule for auxiliary titles.
Release build and four regressions pass, including real Classic caption pixels
at widths 268, 269, 400, 401 and back to 268. See SKIN_ALIGNMENT.md.

2026-09-12: fix community-link icons and main-popup icon shadows. Bind new
link IDs to native web/edit images, remove the non-native submenu/command
shadow exclusions, and capture colour-key masks before ImageList_AddMasked
mutates the bitmap. Use only the original 5.7.9 menu/toolbar reference;
TTPlayer6120 remains a skin-only reference. Extend project_links_tests with
the real EXE icon/manifest, all main icons and five-state pixel checks against independent source masks
for the original 5.7.9 DLL and the staged Release resource DLL.
Release build, project_links_tests and both skin regressions pass.
See MENU_STYLE.md for the corrected focus-shadow behavior.

2026-09-12: populate the main context menu's resource 0x9D related-links
placeholder with Github仓库 and 提交反馈. Share labels, HTTPS targets and
browser dispatch with Options; preserve resource parent/icon and owner-draw
popup styling. Host project_links_tests verifies both resource DLL generations,
repeat menu construction, labels/commands, tooltip preference and shell arguments
without opening a browser. Release build and both skin regressions pass.

2026-09-12: hide parsed login/login_name and music-browser skin controls by
request. Share the suppression policy across painting, hit testing, tooltips,
action dispatch and skin previews; retain their XML/image data in the parser.
Release build and both skin regressions pass, including hidden-region pixels,
mouse hit testing and absence of the music-browser tooltip.

2026-09-12: add 6.1.2 mixed BMP/PNG skin support. Keep BMP colour keys;
decode other encoded images through an owned GDI+ stream/image path and
draw with the recovered alpha/sampling settings. Parse flash attributes,
animate buttons/toolbars and playback progress thumb, bind set/mode_* and
read Color_SelText. Host pixel checks cover 36 PNG + 75 BMP assets; all
179 local skin packages load. Resource/external PNG skins, mini round trips
and legacy HWND-preserving rebind regression pass. Cloud/mobile/browser
business windows are not recovered by this change. See SKIN_PNG_RECOVERY.md.

2026-09-12: multi-monitor fullscreen now stays active across process focus
changes. Resolve entry from the main/lyric host (including lyric chrome's
forwarded main menu); keep mode changes on the active display. Add screen
selection ONLY to detached lyric/visual right-click menus, not ordinary
fullscreen submenus. Preserve surface HWNDs while moving/resizing, fix lyric
popup coordinate double-offset, and refresh display/visual buffer dimensions.
Release build and taskbar_playback_tests pass. Two host runs passed 24 cases
across both displays, three modes, split/overlay/transparent layouts, actual
menu selections and foreign-window clicks, Esc/menu exit and HWND reuse.
Release runtime configuration restored by hash. See FULLSCREEN_RECOVERY.md.

2026-09-12: desktop lyrics can move between monitors. Replace primary-only
work-area queries and zero left/top clamps in snap/PositionWindows with the
proposed rectangle's monitor work area. Keep control/paint/bar synchronized
and restore saved secondary-screen positions. Preserve staged 400..10000 width
limits; update old UI scroll-test dimensions accordingly. Release build and
desktop_lyrics_tests pass; physical host tests pass 16 two-screen edge cases
and primary/secondary/primary lyric drags. See WINDOW_DRAG.md.

2026-09-12: skin-window move/resize snapping now resolves the proposed
rectangle's monitor work area instead of always using the primary screen.
Attached groups retain a common translation and the existing Snap_Windows
enable/distance setting. Release build and ttplayer_tests pass; host physical
drag probes pass 16 edge/threshold cases across both displays. Negative and
vertically offset layouts are covered by geometry tests. See WINDOW_DRAG.md.

2026-09-12: restore thumbnail playback controls after mini/fullscreen returns.
Main-window SWP_HIDEWINDOW now resets the cached shell toolbar registration;
the next TaskbarButtonCreated reinstalls buttons even when the HWND is reused.
Minimization, tool-window visibility rules and playback dispatch are unchanged.
Release build/taskbar_playback_tests pass; host default-skin tests pass two
mini and two combined-fullscreen round trips with real thumbnail pause/resume
clicks. Existing Release configs restored by hash. See TASKBAR_PLAYBACK.md.

2026-09-11: fixed playlist Add/Delete/List/Sort/Find/Edit/Mode hover loss.
Child mouse forwarding had armed leave tracking on the playlist parent,
immediately clearing the already-loaded skin hot image. Track the real input
HWND, cancel obsolete tracking, and reject stale parent/child leaves. Match
the native drop-down normal/hot transition and Escape hot-item retention;
keep skin bitmaps, color keys, resource labels and geometry unchanged.
Compared against 004A9937/0047E6FC/0047AB54/00482BAF and host original-player
captures for default, LX-iPlay, TT2012, Let's Vista and Media Player 10.
222 common-region toolbar frames and 35 popup mappings match. Release build
and ttplayer_tests pass; original 30 Release configs preserved by hash checks.
This is scoped toolbar compatibility, not an all-skins/binary-identity claim.
Details and reproducible local probes: PLAYLIST_MENU_INPUT.md.

2026-09-11: General options now exposes "向 Discord 发送歌词", bound to the
existing General/@DiscordSyncLyrics setting (default unchanged). The master
song-presence checkbox disables this child option without losing its check state;
the application ID stays XML-only. Explicit lyric-policy changes bypass ordinary
line coalescing, suppress pending/stale lyric text and preserve song/pause fields.
Release build and both Discord regression targets pass; private IPC toggle tests
and host General-page save/close/reopen/restart tests pass without real Discord
publication. Existing 30 Release configs restored byte-for-byte after building.
Implementation/verification details: DISCORD_PRESENCE.md.

2026-09-11: appended 13 optional PATH-based FFmpeg CLI presets to the root,
Debug and Release ttp_clienc.xml: AAC-LC, MP3, Opus, Vorbis, FLAC, ALAC,
WavPack and PCM WAV. Existing 29 presets and each current selection remain
unchanged. Uses the user's Scoop ffmpeg.exe, without copying/installing an
executable, hard-coding an absolute path or changing PATH. All 13 pass real
Release host-window tone conversions and independent ffprobe/PCM checks;
six lossless/PCM presets reproduce the 16-bit fixture exactly after decoding.
Tagtype 0 intentionally does not claim original metadata/cover preservation.
No EXE rebuild or new full CTest run needed for this configuration-only change.
Details, commands and verification scope: EXTERNAL_ENCODERS.md.

2026-09-11: supplied all ten previously missing CLI encoder executables in the
local Release/Encoders directory, including Apple dependencies for QAAC and the
original Nero download recovered from Wayback (historical SHA-256 matches).
Restored the host CreateStreamOnFile export (004C51D3, ordinal 3): ttp_clienc
6020147F otherwise rejects every %s temporary-WAV preset before child creation.
Corrected six Nero -ignorelenth typos, preserving preset selection and settings.
All 29 current CLI presets finish real host-window conversions with a generated
3-second tone; all 30 Release CTest cases pass (36.37 s). Independent decoders
validate all eight outputs that the generic PCM probe could not read correctly;
those separate probe/decoder-path anomalies remain, not claimed fixed here.
No system installs or registry/PATH changes. Explicit hash-pinned deployment
script and redistribution limitations: EXTERNAL_ENCODERS.md.

2026-09-11: fixed Nero conversion loading and output sharing violations.
Resolve NeroIPP/aacenc32/Aac by absolute AddIn paths before original creator
calls; retain dependencies across background library snapshots. Replace the
empty output placeholder with a reserved directory/absent media file, matching
00412575 -> 004CD30C: Nero's failed existing-MP4 parser otherwise leaks a read
handle even though Finalize closes the writer. No forced handle closure or DLL
patching. Nero now passes real encoding/PCM/tag/exclusive-handle, overwrite,
cancel, Unicode, 48 kHz and batch-import tests. Wave/APE/native MP3/WMA and all
five supplied CLI LAME presets pass original/rebuilt host window conversions;
Wave/APE/native MP3 fixture files are byte-identical. Stage optional CLI LAME
from the supplied ZIP into Encoders without replacing existing executables;
no player startup/playback/DLL conversion EXE dependency. The other 24 CLI
presets still lack ten external programs. Nero >48 kHz remains unsupported as
in the original. Details and explicit boundaries: CONVERSION_RECOVERY.md.
Final Release regression: all 30 CTest cases pass (32.43 s); all six encoder
configuration dialogs pass close/OK/cancel (18 opens), with DEP enabled.
Restored all 30 pre-test runtime configuration files and verified their hashes;
all temporary host player processes exited.

2026-09-11: fixed conversion encoder configuration dialogs faulting before
becoming visible. Original 0047D9A4 -> 004CD21E uses creator slot 7 with the
conversion dialog as owner; that synchronous modal ABI remains unchanged.
Actual host tracing identified legacy ATL heap-thunk execute faults (C0000005,
access 8, PAGE_READWRITE), followed by C000041D/Windows Error Reporting waits.
Pair /NXCOMPAT:NO with early SetProcessDEPPolicy(PROCESS_DEP_ENABLE), retaining
DEP and ASLR while enabling Windows' legacy ATL emulation. No DLL patching,
executable heap changes or registry mitigation changes. Host UI tests pass all
six configurable encoders through close/OK/cancel (18 opens); Wave stays
disabled, parent ownership/re-enabling and clean exit pass. Runtime DEP flags=1,
permanent=TRUE. All 30 Release CTest cases pass (37.67 s). Nero's separate
dependency loading/output-commit issues remain; see CONVERSION_RECOVERY.md.

2026-09-11: restored the playlist conversion pipeline against 0047D682,
004122CD/00412723 and 00412B48/00412E61: Wave-first encoder catalogue,
creator configuration, instance extension, double-PCM/resample/effects chain,
conditional metadata copying, modeless singleton progress, pause/cancel,
per-track completion import and overwrite policies. Added an explicitly separate
optional LAME DLL encoder from the supplied x86 ZIP (including libmpg123; no EXE
dependency). Wave/APE PCM integrity, native MP3/WMA and LAME CBR/VBR/ABR,
CUE, Unicode paths, cancellation and overwrite protection are host-tested.
Original/rebuilt host UI checks cover catalogue/capabilities and batch numbering
with completion import. Nero AAC and missing external CLI presets remain
unvalidated; cooperative pause and transactional replacement are intentional
safety differences. See [CONVERSION_RECOVERY.md](CONVERSION_RECOVERY.md).
Final Release build and all 30 host CTest cases pass (32.42 s); 30 pre-existing
runtime configuration files were restored and SHA-256 verified after staging/tests.

2026-09-11: completed Discord reconnect/seek and current-line lyric synchronization.
The sender retains every latest audio sample and re-reads state after blocking
READY handshakes; timeline revisions bypass distance-based seek filtering.
Pending seeks do not extrapolate before decoder acknowledgement. Timed LRC lines
share the player's offset/seek logic even with hidden lyric windows; ordinary
line changes coalesce at two-second intervals, while seeks, pause and clear take
priority. General/@DiscordSyncLyrics defaults on under the Discord master switch;
artwork remains deferred. Release and all 29 host CTest cases pass (30.21 s),
including actual private-pipe reconnection, delayed handshakes, one-millisecond
seeks and lyric coalescing. No real Discord account was used for rendering tests.
See [DISCORD_PRESENCE.md](DISCORD_PRESENCE.md).

2026-09-11: expanded Discord's music presentation: member-list song/program
titles, fixed paused positions, start-only unknown-duration/listening clocks,
station-aware radio labels, and bounded artist/album text layout. Metadata
comes from the playing item; URL-only titles are not published. Album artwork
is explicitly deferred. The sender now projects stale snapshots at reconnect;
native PCM clocks no longer clamp unknown durations to zero, and unknown-length
audio no longer triggers end-of-track fade. Release and all 28 host CTest cases
pass (23.13 s), including isolated Discord IPC and clock/fade regressions.
Real Discord profile rendering and live-reader metadata delivery are not
established by these tests. See [DISCORD_PRESENCE.md](DISCORD_PRESENCE.md).

2026-09-11: restored the optional mp3PRO Winamp input bridge following
004E6CBD/004E6FB6, including enhanced-rate selection, bounded PCM buffering,
single-reader ownership, pause/resume, asynchronous seek flushing and stop.
Keeps MPEG fallback and metadata; loads only EXE-local mp3PRO.dll without a
file-version check or another EXE dependency. Fixed paused waveOut seek
prefill briefly advancing the output clock. Mock ABI/PCM, CUE and silent
waveOut/DirectSound tests pass; the real DLL passes synthetic ordinary-MPEG
fallback/EOF/seek checks. A genuine mp3PRO sample is still needed for SBR
comparison against the original. Release build and all 28 CTest cases pass
on the host (24.28 s); 27 existing configuration/playlist/skin XML files
retain their SHA-256 hashes. See [MP3PRO_RECOVERY.md](MP3PRO_RECOVERY.md).

2026-09-11: removed the remaining sidecar EXE runtime dependencies. DSP scan,
per-DLL inspection/configuration and output-device catalogue/details now run
in private modes of the player itself; metadata callers no longer construct
old helper filenames. The player build target no longer depends on probe
executables. Release and all 27 CTest cases pass; host UI tests with a renamed
EXE and no helpers show one accepted DSP (a stalling fixture is timed out),
eight output devices and a FLAC cover, with all six properties close checks
passing. Process isolation remains an intentional difference from the original
in-process architecture. See [EMBEDDED_WORKERS.md](EMBEDDED_WORKERS.md).

2026-09-11: fixed portable File Properties when only the player EXE is copied
into an existing installation. Metadata/cover/tag jobs now run in an isolated
private mode of the same EXE, before normal startup/IPC, without requiring the
standalone helper. Fixed SC_CLOSE and completed-modeless-sheet teardown so the
owner cannot remain disabled behind an orphan properties shell. Host tests
with D:\Programs\TTPlayer DLLs, a renamed EXE, no helper and a different working
directory show the FLAC cover and pass six close-command checks. Release and
all 25 CTest cases pass. See [PORTABLE_FILE_PROPERTIES.md](PORTABLE_FILE_PROPERTIES.md).

2026-09-11: desktop lyrics now cache full-width lines and scroll the visible
slice following `00417342`, independently of karaoke highlighting. The head
stays still until playback passes the window midpoint; the tail stops at the
right edge. All paint layers share the offset, two-line previews stay at their
heads, and seeks/pauses derive directly from playback time. Added actual
UpdateLayeredWindow DIB comparisons and timing/boundary regressions to the local
desktop lyric tests. Release build and all 25 CTest cases pass on the host.
See [DESKTOP_LYRIC_SCROLL.md](DESKTOP_LYRIC_SCROLL.md).

2026-09-07: skin replacement now rebinds existing HWNDs/controls following
`0045D5FA -> 0045DDEE -> 00468363`. Removed the duplicate menu skin application,
unconditional shadow hide/show, and lyric/EQ/playlist-toolbar recreation.
Preserves live RichEdit content/selection and playlist selection; sidecars are
committed before presentation. Native/rebuilt host menu round trips for default,
LX-iPlay, TT2012 and Let's Vista retain all observed HWNDs. Release and all 25
CTest cases pass, including tagged-handle, hidden-profile and mini-exit tests.
See [SKIN_REBIND.md](SKIN_REBIND.md) for original addresses and test boundaries.

2026-09-07: added Windows-native taskbar thumbnail transport controls as a
requested modern extension: previous/play-pause/next, resource-backed labels,
live enabled/playback state, minimized-window operation and shell recreation
handling. Existing mini/tool-window and tray visibility rules are unchanged.
Release build and all 24 CTest cases pass. Real host thumbnail mouse clicks
verified play/pause/resume/next/previous without restoring the player; tests use
an isolated silent runtime. See [TASKBAR_PLAYBACK.md](TASKBAR_PLAYBACK.md).

2026-09-07: fixed the transient old-position flash after progress/lyric release.
The audio engine now exposes the accepted seek target until the output worker
acknowledges the matching request revision; consuming the mailbox no longer
releases the target, and stale/same-target ACKs cannot overwrite a newer seek.
Stop/error/worker exit discard pending targets. Lyric release publishes the
target before releasing capture. Extended `progress_seek_tests` covers the
handoff and samples the real host clock throughout the first 250 ms after seek.
Release build, all 23 CTest cases, and silent host waveOut/DirectSound playback
checks (normal/mini, forward/backward/paused seeks) pass.

2026-09-07: progress slider tracking now previews without seeking; release
commits its final position once without a seek fade, following the notification
split in `00460AB1` and tracking guard in `00428DCD`. Cancel/capture loss discards
the preview. Normal/mini and horizontal/vertical sliders share this behavior;
play/pause/stop fade settings remain independent. Added `progress_seek_tests`
for real mouse-message handlers and fade scheduling, plus explicit silent host
playback checks on waveOut/DirectSound. See [AUDIO_ENGINE.md](AUDIO_ENGINE.md).
Release build and all 23 CTest cases pass on the host. Explicit playback checks
also pass for playing and paused seeks in both normal and mini layouts.

2026-09-07: removed exact `ttpcomm.dll` version gates in both the API loader and
startup runtime. Version queries are optional and are never called at startup;
EXE-local loading and per-feature ABI/resource checks remain. Compatibility tests
cover a differing version, an absent version export and missing EXE-local DLLs.
Release build and all 22 CTest cases pass on the host; the existing DLL's UI
startup smoke test passes too. Other real DLL releases still require ABI testing.

2026-09-07: restored Files-owned playlist item infotips, child-coordinate mouse
hit testing and notification routing, native metadata-only placeholders and PCM
short codec name. See [PLAYLIST_ITEM_TIPS.md](PLAYLIST_ITEM_TIPS.md) for original
addresses, host comparison coverage and the repeatable probe.

| Module | Current state | Next compatibility target |
| --- | --- | --- |
| Core/build | recovered `004C0E8F` `wWinMain` lifecycle with intentionally version-independent EXE-local ttpcomm loading, five-second single-instance forwarding (including `/a`/`/e` payload mode), TLS/OLE/common-controls, validated EXE-local `ttpres.dll`, sound/CoolSB startup and reverse teardown. `/reg` opens the original reduced About+association sheet; `/unreg` removes owned per-user extension, AudioCD and Directory registrations. The `004B5470/004B54F2` idle-aware message pump and tooltip hook behavior are live. | private thread-diagnostic object, WTL command-bar/button wrapper and CBT shadow/theme wrapper cannot be reconstructed from the pseudo-C object layout alone |
| Audio | recovered reader and decoder registries plus synchronous x86 creator/`IStream`/six-slot-buffer chains; APE, TAK, VQF and the supplied FLAC decode/read/seek are real-sample validated. A shared decoded-source and PCM-output chain covers AddIn, Media Foundation URL/WAV/MPEG, AIFF/AIFC/AU, exact 75 Hz CUE and raw CD-DA; output bit depth, SSRC ordinal 102, dither, ReplayGain, EQ, Surround and Winamp DSP feed bounded waveOut, DirectSound, KS or ASIO streams. `AutoScanGain` analyzes the same pre-gain playback PCM and commits only at natural EOF; it does not launch a second decoder. Encoder creators execute Configure/Open/Start/Write/Finalize for playlist conversion. All five fade-mode transitions and four duration fields have runtime consumers where the original output supports them; `WM_CLOSE` now starts the bit-3 stop fade before window teardown and retains the original `FadeDuration[3] + 500 ms` bounded exit guard. | validate CD-DA on physical optical hardware and KS/ASIO on compatible devices; retain explicit device-specific KS topology and retired network-reader boundaries |
| Playlist | independent `TTPlayer_PlayListWnd`, all 23 supplied `playlist_window` skins, original `0x7D66` show/hide command, skin-relative switching, fixed/native and resizable/minimum-size window modes, client-edge/corner resizing, attached main-window group movement, nine-slice background painting, multi-list `%04d.ttbl` store with `%03d` migration and current-row persistence, M3U/M3U8 plus TTPL/version-4 XML reading and writing, native-style multi-selection, internal selected-row drag reorder plus Ctrl copy to another catalogue, 16-pixel owner-draw rows, skinned splitter/scrollbar/title/close, resource-backed toolbar/context menus, delayed atomic saving/slot compaction, Vista+ Common Item Dialog single/multi-file and folder intake, and real OLE `CF_HDROP` routing for main replace/play, playlist positional insertion/catalogue import, archive/CUE expansion and lyric-first-item loading are live; the equalizer intentionally rejects drops. The resource-backed file-information sheet reaches reader-QI metadata/thumbnail setters or the executable's built-in MP3 tag path through a bounded helper. | same-process private clipboard/data-object interoperability and retired service dialogs |
| Lyrics | normal/mini `TTPlayer_LyricWnd` popup plus detached full-screen `LyricCtrl` and independent layered `DeskLrcCtrl/Paint/Bar` subsystem recovered: independent normal/mini/full-screen scroll, font, alignment, spacing, fade, karaoke, colors and transparency; exact `AutoFontFS` width fitting; opaque, desktop-color-key and visual-overlay layouts; native full-screen/desktop menus; menu/physical-Escape restoration; `004A88D0` skin/Lyric.xml parsing, local LRC discovery, editing, metadata, smooth scrolling and pixel-to-time dragging are live. Lyric drag/line-step seeks preserve the supplied original's direct audible transition instead of applying the reconstruction's broader generic seek fade gate. | a provider-neutral replacement for the retired online service |
| Skin/UI | the DLL default and all valid EXE-local ZIP-based external skins load and switch successfully, including UTF-8-BOM metadata compatibility for `004941_088.skn`; DLL/Skin resolution is restricted to the EXE directory; playback-state controls, original context menus, hover paths, dragging, mini-player switching and separate auxiliary windows are live. `VisualCtrl(0x7DDC)` supplies PCM-driven dream/spectrum/scope, WIC-first/OLE-fallback covers with atomic off-screen presentation, per-skin `Visual.xml`, the exact native seven-item detached menu, split/overlay full-screen layouts and restored HWND style/parent transitions. The original 15-page modeless Options sheet, targeted entry routes, nested lyric/network pages, ttpres-backed controls, device/DSP/association pages, lifecycle messages and modeless 48-color `ColorSelectCtrl` are present. The EXE-local `Music.library` catalogue, tree/query/playback/rating/edit routes and monitored-directory refresh are active | retired online-service backends |
| Plugins | loads EXE-local `AddIn\ttp_*.dll` and enumerates the four real categories: reader, decoder, encoder and lyric-search provider. Retained factories and sessions implement reader/decoder PCM, metadata slots 3–6, thumbnail slots 3/4/5/8/9 and encoder Configure/Open/Start/Write/Finalize with SEH and module-lifetime barriers. There is no separate Sound AddIn writer/processor category: metadata/thumbnail writing belongs to a reader QI and PCM processors come from ttpcomm/Winamp DSP. | recover the retired lyric-search provider request/result/cancel protocol and validate ABI variants not represented by the supplied DLL set |
| Settings | imports and saves the Player/Playback/Device/Convert/Library/History/lyrics/visual/hotkey schemas and the modeless 15-page Options sheet preserves Apply/close notification semantics. Runtime consumers now cover output backend/buffer/width/resampling/dither, AutoGain/live AutoScanGain/manual SkipScanGain, all fade durations, EQ/DSP, local lyric discovery/visibility/trimming, media-library monitoring, modern-dialog history, menu/control hotkey tips, playback-open tips, window snap/title/auto-shutdown and the normal/mini/full-screen/desktop UI profiles. MP3 policy crosses the helper boundary and affects actual tag I/O; per-skin profiles remain separate from root globals. The retired MSN/Baidu Hi checkbox is migrated to asynchronous Discord Rich Presence with built-in application ID `1546275976676376716`; only a non-empty `DiscordApplicationId` in `TTPlayer.xml` can override it, and the ID is not exposed in Options. | retired update/FreeDB/online-lyric/cache/download services and their settings remain explicit nonfunctional compatibility data rather than simulated successes |

Verified on the supplied installation:

- The final Win32 Release build and all 21 CTest targets pass on the host.
  The supplied `陈慧娴 - 千千阙歌.flac` opens through the original
  `ttp_flac.dll` reader as 44.1 kHz/16-bit/stereo, reports 299106 ms, decodes
  52,762,340 PCM bytes to EOF with FNV-1a
  `bb35ad7ed2874bb0`, and advances through the selected DirectSound device.
  A loopback HTTP WAV also opens through the shared URL source and advances
  for more than three seconds. This host exposes no optical drive, accepted
  KS endpoint or ASIO registry entry, so their physical-hardware behavior is
  not claimed by the deterministic sink tests.
- Media-library host-copy probes preserve `Music.library` without modifying
  numbered playlists and observe monitored-file add, rename and delete
  transitions (6, 6 and 5 live entries respectively); every player process
  exits normally.
- Full-screen recovery follows `FUN_0046228D`, `FUN_004427B1`,
  `FUN_00458020`, `FUN_0044B4A1` and `FUN_004499AD`. Original/rebuild host
  probes match all three mode rectangles and detached HWND contracts; both
  expose the same 12-item lyric and seven-item visual menus, physical menu
  exit and physical Escape restore the main/lyric windows, and full-screen
  lyrics use the same black background, 60-pixel font and blue/green colors.
  The isolated rerun harness and its limitations are documented in
  `FULLSCREEN_RECOVERY.md` and `tools/windows_sandbox/README.md`.

- `TTPlayer.xml`: volume 100, output 16-bit, file buffer 16384;
- Built-in file information now follows the MP3 paths at `004D9AC8` and
  `004D9B56`, which exist in the executable and therefore must not depend on
  a nonexistent `AddIn/ttp_mp3.dll`. `004D9B0B` read-priority merging,
  `004D9C09` write-type selection, `004D9C7E` padding and `004D9C87 ->
  004DC59D` encoding selection cross the helper request and affect real tag
  I/O. ID3v1, ID3v2 and APEv2 combinations are written through a sibling
  temporary file and atomic replace; unmodified private ID3 frames and APE
  items survive. The conservative writer refuses globally-unsynchronised and
  ID3v2.2 tag rewrites with `ERROR_NOT_SUPPORTED` rather than dropping opaque
  frames. WAV RIFF/INFO and Shell-backed non-AddIn file information are
  available read-only.
- Media-library startup follows `004C03FD -> 004AF271`: `MaxItemCount` selects
  initial hash capacity and never caps scan/query results. Shutdown follows
  `00461950` by writing the non-tombstoned live count back as the next-run
  hint. `004C038B -> 004AF7F4` EXE-local `Music.library` loading and
  `004616BD -> 004AF838` ordinary TTBL persistence remain active alongside
  monitor, category/query, playback, rating and property-edit routes.
- `AddIn`: 15 个 `ttp_*.dll` 暴露 `ttpGetSoundAddIn`，另有 6 个依赖/资源 DLL；
- `Skin/TT2012.skn`: 74 ZIP entries and required legacy assets present;
- unit tests: playlist navigation and LRC parsing/time lookup pass.
- Normal lyric recovery follows `0046AACA`, `004A88D0`, `00442576`,
  `004425F7`, `00442671`, `00442A97`, `0043FA1D` and `0043EF4D`.
  Original/rebuild LX-iPlay probes match the `500x70` popup, exact
  `35,5,465,65` lyric child, popup style/ex-style/owner, the three zero-size
  skin commands, and hidden `0x82DC` base control.  Detailed evidence and the
  separate desktop-lyric recovery is in `LYRIC_WINDOW_RECOVERY.md`.
- Sound AddIn registration now follows `004C8421`, `004CABC0`, `004C88D9`,
  `004C8954` and `004CB1DB`: the EXE-local AddIn path is appended to `PATH`,
  only `ttp_*.dll` is code-loaded, the factory and interface enumerator must
  succeed, and only reader category `476D15A5-D863-416A-8A59-A9C7D72CE04E`
  contributes a format. The UI no longer uses `LOAD_LIBRARY_AS_DATAFILE` or
  hard-coded DLL/resource IDs to make a failed codec appear installed.
- Playlist recovery: `0046BE24` now creates the owned popup class
  `TTPlayer_PlayListWnd` with the original `WS_POPUP`/`WS_EX_TOOLWINDOW`
  combination. `004A3BBC` command `0x7D66` toggles rather than opening the file
  dialog, `PlayListWnd`/`PlayListVisible` are restored and saved, and a close
  hides the window without ending the process. `004A8DC6` fields for the root
  image, toolbar/hot image, list bounds, selected row, splitter and scrollbar
  bitmaps are parsed for every supplied skin. LX-iPlay comparison confirms the
  original 500x255 window and 455x20 bottom toolbar. Captures are under
  `build/comparison/playlist-original-lx-visible/` and
  `build/comparison/playlist-rebuild-lx-visible/`.
- The internal playlist format is corrected from the earlier XML assumption:
  current `PlayList/0000.ttbl` is binary `TTBL`, version 5. The rebuild reads
  the supplied original file, writes the recovered flag-based record layout,
  and commits through `0000.ttbl.tmp` plus replace/write-through rename. A
  round-trip test checks title, paths, display title and duration.
- `004783FA`, `004786E7` and `00480DE3` are now represented by a dedicated
  multi-list store. Startup scans 100--1000 EXE-local numbered slots, retains
  `ActiveList`, recovers an orphan `.tmp`, accepts the `%03d.ttbl` generation,
  and saves each dirty list through `%04d.ttbl.tmp`. The original debounce is
  preserved: commit after 30 seconds or after the fifth edit, with a forced
  flush during orderly shutdown. `PlayLists`, `ActiveList`,
  `CreateNewVerPlayList` and `Histroy/SplitOnLists` are imported; list count,
  active slot and splitter position are saved.
- Original LX-iPlay runtime probes exposed `SplitterCtrl`, `TreeCtrl`,
  `ListCtrl(10241)` and `ListCtrl(10242)` beneath `TTPlayer_PlayListWnd`.
  Their observable state is separated in the rebuild: the left list pane uses
  `SplitOnLists=55`, the track caret is independent from the playing item,
  Ctrl/Shift multi-selection and keyboard paging are supported, and switching
  the active list no longer redirects an already playing track. Pixel probes
  established a 16-pixel ListCtrl item height; the 19-pixel selected bitmap is
  clipped/restarted per row. `00487C0D` draws the playback triangle only for
  the current playing row and draws the focused-row frame independently from
  `LVIS_SELECTED`; selecting an arbitrary row therefore does not synthesize a
  playback marker. When a skin has no selected bitmap, `0045032C` supplies the
  exact `Color_Select` to `Color_Bkgnd` per-scanline gradient. Odd unselected
  rows use `Color_Bkgnd2`, while number/title/duration retain their separate
  normal, playing and selected text colors. The vertical scrollbar strip is
  reserved only after the item count exceeds the visible page, matching the
  native ListCtrl selection width. Skins without a splitter bitmap retain the
  original five-pixel `SplitterCtrl` gradient.
- `FUN_00482BAF`/`FUN_004897C1` left-catalogue parity is now explicit rather
  than simulated by parent labels: `TreeCtrl(0x2800)`, `ListCtrl(0x2801)` and
  `ListCtrl(0x2802)` match the original class, style and normalized rectangles.
  Active-list color is independent of focused selection; selection bitmap or
  gradient, focus frame, system highlight-text fallback, alternating rows and
  the two-pixel text inset follow the custom-draw branch.  Physical hover
  probes also match the original capture/no-capture split for playlist toolbar
  and close controls, equalizer `SkinButton`/`SkinSlider`, and lyric buttons;
  all release their hot state and tooltip on pointer exit.
- Playlist menu/input recovery now follows `FUN_00482BAF`, `FUN_00483AF8`,
  `FUN_00488FEF` and `FUN_004894AF`. Menu resources `0x8B`, `0x98`, `0x99`
  and `0x9C` are loaded from the EXE-local `ttpres.dll`; the local-file branch
  performs the original command/position deletions instead of showing a static
  superset. Physical probes match the complete command-ID sequences for the
  19-item single-track, 15-item multi-track, seven-category blank-area and
  12-item playlist-title menus. All 23 external skins expose matching seven
  toolbar popups; `173 Keenwood` uses XML rather than bitmap hit width, and
  the 47-pixel tall toolbar in `一听音乐` keeps quick-find in its lower row.
  Physical selected-row moves in embedded LX-iPlay and separate TT2012 lists
  produce the same final TTBL order in the original and rebuild. Detailed
  evidence is in `PLAYLIST_MENU_INPUT.md`.
- Playlist toolbar composition now follows the native color-keyed image-list
  path instead of opaque `SRCCOPY`. Across all 23 supplied skins the base
  toolbar crop is pixel-identical to the original and no rebuilt playlist
  capture contains a pure `#ff00ff` pixel. Integer ImageList frame widths
  recover the one-pixel-per-button placement in TT-07, PurpleMyth and
  Qingping; proportional narrow-frame cropping preserves 173 Keenwood, and
  AIPOTU's overlapping 31-pixel frames are also exact. The toolbar opens its
  resource popup on button-down like `TBN_DROPDOWN`; Media Player 10's base,
  hot and popup-open states are pixel-identical. Four physical right-click
  targets still match the complete resource command sequence, and the new
  two-row Ctrl-selection drag probe gives the same retained order in both
  programs.
- Playlist catalogue creation/rename now follows `0048468A`, `0044DEF5`,
  `0048516E` and `00489B06`. New lists use EXE-local `ttpres.dll` string
  `0x8192` (`新列表%d`), become active, and immediately enter the same in-place
  `Edit` used by right-click command `0x7F04`; Rename is no longer disabled.
  Enter commits without trimming, while Escape, empty Enter and focus loss
  retain the old title. Dirty tracking is applied to the renamed numbered
  TTBL rather than accidentally to the active entry. Physical right-click,
  create, commit and cancellation probes match original editor text, exact
  `0x54000080/0` styles and rectangles in LX-iPlay, TT2012 and Let's Vista.
- `004A8DC6` playlist `title` and four-frame `close` elements are parsed in
  addition to the previously recovered root/toolbar/list/scrollbar assets.
  The seven measured toolbar commands are live: 添加, 删除, 列表, 排序, 查找,
  编辑 and 模式. The list/sort/mode popup contents were recovered by clicking
  the original LX-iPlay toolbar. Skinned scrollbar button, bar and thumb frames
  now support line/page scrolling and thumb dragging; the splitter is also
  draggable and persisted.
- UI smoke test: creates the real skinned main window, enters the message loop,
  and closes cleanly; captures are `build/default-skin-window.png` and
  `build/player-skin-window.png` (TT2012).
- Main-window right click: original menu 0x8a and its resource-ID submenus are
  loaded from the recovered `ttpres.dll`; dynamic track/skin/alpha entries and
  playback, mute, mode, top-most, minimize and exit commands are live. The
  normal-player `0045DFBA` branch removes resource 0x94 and its DAT_00547858
  re-entry guard is reproduced. The full shaped client surface now routes
  right-click through `00460D88` semantics. Blank-area left drag now follows
  `0046EAAC/0046EB64/0046EB28`: client anchors, `SetCapture`, current-rectangle
  delta movement and release cleanup, without a synthetic `HTCAPTION` message.
  The 10-pixel `0044F804/0041094D` work-area/window snapping rule, the
  `0047075B` transitive attached-window set and `004708B4` deferred group move
  are also restored. `0040BFD7` recognizes same-side aligned overlapping
  boundaries in addition to opposite-edge contact, so LX-iPlay's 500-pixel
  aligned player/playlist pair follows the main window as one group. The
  playlist remains independent when it is the source window, but its
  `FUN_0048AC21 -> FUN_0044F3CF` movable block uses the same 10-pixel magnetic
  threshold. Reference/rebuild physical-input probes agree at the 10/11-pixel
  boundary, on a 9-pixel playlist-to-player gap, and for attached/group movement;
  popup selection is delivered by
  synchronous `WM_COMMAND` as in `0046A27D`. Capture:
  `build/context-menu-window.png`.
- Popup-menu drawing now follows the WTL command-bar owner-draw path rather
  than the host OS default menu renderer. `00469D09/00470482` row measurement,
  `00469CD8/0046D5E7` flat gradient/text/selection drawing, the `0x82/0x83/0x94`
  toolbar image lists and command aliases are active for main and playlist
  popups. The main menu also restores `00465CB8`'s `0xFFFA` zero-height side
  item and `004700D9`'s vertical `千千静听--尽听精彩` strip. Live original and
  rebuild menu handles both report 130x20 playlist-title rows, 10-pixel
  separators, a 17x0 main side item, and 121x20 main rows; normal, hover,
  icon, disabled and checked-menu captures are under
  `build/comparison/menu-style/`. See `MENU_STYLE.md`.
  The remaining `004700D9` ambiguity has also been resolved: its localized
  brand flag selects one `DT_WORDBREAK` draw rather than a manual per-character
  loop. The measured main-menu side region is now pixel-identical, including
  the inherited menu-font fields, 14-pixel SimSun metrics and the two embossed
  text offsets. The executable now carries original group icon resource
  `0x80` and the original Common Controls 6 activation dependency, matching
  `0045FAD8`'s module-resource load and alpha-compositing path.
- The initial pair scan in `0047075B` performs `0040BFD7` attachment testing
  before the later visibility-filtered transitive expansion. The rebuild now
  preserves that ordering: an already attached but hidden LX-iPlay playlist
  follows a `(40,30)` main-window drag from `(300,250)` to `(340,280)` and
  reappears there, while a hidden detached playlist at `(100,500)` remains
  fixed in both programs. Skin switching also reloads the package sidecar
  `Skin/<package>.xml` playlist font/colors, preserves global `SplitOnLists`,
  and falls back to the package `Playlist.xml` values parsed by `0048E696`.
  TT2012 pixel probes match the original 16 selected-row scanlines and the
  five splitter scanlines exactly. The physical list-surface drag matrix gives
  the same no-move result for all 23 supplied skins.
- Playlist window dragging now includes the omitted resizable-window branch
  from `00410832/0046EB64/0048B8C7`: skins with a non-empty `resize_rect`
  expose four client edges and four corners, keep the background bitmap's
  native size as their minimum, use the work area as their maximum, and snap
  the active edge through `004709A2` semantics. Fixed skins ignore invalid
  persisted sizes. TT2012 reference/rebuild probes both resize `500x255` to
  `540x285` after a `(40,30)` bottom-right drag. Runtime skin replacement now
  translates `playlist_window.position` by the new main-window origin as in
  `00468363 -> 0046BCBE`; selecting LX-iPlay therefore places the playlist at
  `(player.left, player.top + 50)`. Resized backgrounds use the parsed
  `resize_rect`/`resize_tile` nine-slice path instead of stretching the entire
  bitmap, and `SetWindowRgn` is regenerated from that rendered color-key
  surface so transparent rounded corners do not leak the key color. An
  isolated startup matrix with the same deliberately undersized
  saved rectangle matched the original for all 23 supplied skins; every
  playlist HWND was visible and every resulting width/height pair was equal.
  The original's playlist body is a child `ListCtrl`, so its entire rectangle
  consumes button messages even below the final row. The rebuild now blocks
  the parent drag path for the complete parsed list rectangle rather than only
  for populated rows. Physical `WindowFromPoint` probes match the original
  no-move result at the list center for all 23 skins, while a separately found
  exposed parent-skin point moves `(24,16)` in both programs for all 23. For
  LX-iPlay, dragging the main window moves its aligned embedded playlist by the
  same delta, while the visible list body itself cannot start a separate drag.
- Main-window double click now follows `00460CF5` and command `0x7DD4`, rather
  than opening the file dialog. `FUN_004530FD` support detection is reproduced
  against the successfully loaded `mini_window` background; missing or broken
  mini backgrounds keep the command disabled. Valid mini backgrounds,
  elements, text and color-key regions are parsed, normal/mini player and lyric
  rectangles are saved independently, playlist/equalizer are hidden/restored,
  and the skin's `minimode` button shares the same transition. The original DLL
  default was measured at 327x141 -> 383x25 -> exact normal-rectangle restore.
  LX-iPlay produces 500x350 -> 200x33 -> exact restore, followed by an exact
  `(20,10)` captured background drag. Detailed evidence is in
  `../reverse/semantic/MAIN_WINDOW_MOUSE.md` and `LYRIC_WINDOW_RECOVERY.md`.
- Runtime skin switching keeps the old bitmap/icon ownership alive until the
  new window size, region and icons have been committed. Failed/invalid skin
  loads no longer clear the current live skin; empty regions are rejected and
  the original skin is restored. As in `0045D5FA`, `WS_EX_LAYERED` remains set
  throughout the switch instead of being removed inside the active popup-menu
  loop. The `0x940A0000` main-window style now preserves `WS_VISIBLE` instead
  of accidentally rewriting it as hidden `0x840A0000`. Automated message verification
  switched Default -> Let's Vista (322x165) -> TT2012 (327x157), waited three
  seconds after each switch, and observed the process remain alive.
- Entrypoint smoke test now traverses the recovered `ttpcomm.dll` 5.7.0,
  resource, sound-discovery and CoolSB initialization/teardown sequence. A
  two-process check also verifies secondary exit code 1 while the primary
  remains alive and receives the foreground/`WM_COPYDATA` path.
- A visible-string audit leaves no Han text literals in `rebuild/src` or
  `rebuild/include`: playlist/menu/status/channel/filter/error labels come
  from the EXE-local `ttpres.dll`. The ttpcomm, ttpres and sound-library
  startup failure sentences remain literals because `004C0E8F/004C01CD`
  contain those exact strings; the sound caption resource `0x80` is captured
  before module shutdown, and ordinary decoder-open failures use `0x828E`
  rather than exposing reconstruction-only English diagnostics.
- Startup-to-first-show now follows `004C0E8F`, `004C0900`, `004C01CD` and
  `0045FAD8`: the hidden host is created at 100,100 with a 300x200 rectangle,
  then the selected skin/region and `PlayerWnd` or `PlayerWnd2` geometry are
  committed before the hard-coded `SW_SHOW`. The main class is the original
  `TTPlayer_PlayerWnd` with class style `CS_DBLCLKS`; owned playlist visibility
  is restored only after the main window is visible. Runtime probes match the
  original normal `0x940A0000/0x00080000` and mini
  `0x940A0000/0x00080088` style pairs, and an exit/relaunch round trip retains
  both independent rectangles and `MiniMode`.
- Full ZIP skin collection regression opens 172 `.skn/.zip` packages, follows the
  `player_window/image` attribute (including `player.bmp` and
  `Player-Window.bmp`), accepts the three recovered missing-whitespace XML
  forms plus bare URL ampersands/trailing root text/duplicate attributes,
  and builds a non-empty region.  The RAR-based historical package is assigned
  to the separate archive backend rather than being reported as a ZIP failure.
- Media Player 10's 78x14 `volume_fill.bmp` contains 768 `#ff00ff` mask pixels.
  Its fill now follows `00451E07`/`00450A1C`: original-size centered clipping,
  color-key `TransparentBlt`, one-pixel slider insets and a thumb-center fill
  endpoint. Original/rebuild 270x168 window captures have zero magenta pixels
  and a byte-identical 72x13 volume crop.
- Main-skin text now follows `004A954D`, `004AA061`, `004AA150`, `00408FC9`
  and `00409023`: caption-font-derived positive-height antialiased fonts,
  12-pixel defaults, XML foreground/background/alignment, transparent sentinel
  handling, no forced ellipsis, centered info and top-aligned stereo/status.
  The separate info composition and final-DC status paths produce pixel-identical
  text crops against the original for Default, Media Player 10, Let's Vista,
  Claymore, ElegantLife and LX-iPlay.
- Playback controls now follow the timer-10 path in `0046010E` and its
  `0045CE05` fan-out: the engine's millisecond position updates the progress
  value and native-size LED glyphs; pause freezes both, stop resets both, and
  dragging the progress thumb performs a block-aligned seek. `0045C198`,
  `0045C0C6`, `0045B69B` and `0046534B` supply the exact `状态: 播放`,
  `状态: 暂停`, `状态: 停止`, `状态: 无效`, `声道`, `静音`, `单声道` and
  `立体声` semantics. The one-based `1.` playlist prefix and the original
  single-HWND play/pause bitmap replacement are also restored.
- The main-skin `info` control now follows the complete scrolling-static path
  from `004091B4`, `004092FC`, the omitted paint body at `00409476`,
  `0040989E` and the queue builder `0045CA57`. Timer 9 scrolls overflow one
  pixel every 40 ms with 25-tick endpoint holds; timer 7 uses the player-set
  `DAT_005478AC = 5` second item delay; timer 8 performs a one-pixel/40 ms
  vertical cross-slide. The queue is primary title, non-empty 0x81CA metadata
  lines, format and duration. The original 45-second probe renders
  `格式: PCM 8kHz 256K` and `长度: 0:45`, now reproduced exactly. In the new
  30-second LX-iPlay trace, stable title start/end, format start/end and
  duration frames are byte-identical; independently scheduled 40 ms motion
  frames can still be sampled one tick apart.
- `tools/compare_playback_skins.ps1` starts the original and rebuild from
  fresh per-run EXE-local runtimes, applies identical WAV/mute/volume settings, and
  captures playing, paused and stopped states for the DLL default plus all 23
  external skins. Results and 144 screenshots are written below
  `build/comparison/playback-all/`. The original probe's playlist/configuration
  are no longer mutated between runs. Default still differs by only five pixels;
  LX-iPlay playing differs by five pixels and paused is byte-for-byte identical.
  Raw stopped snapshots can differ when independently scheduled 40 ms title or
  skin-animation timers are captured one tick apart, so those values are not a
  stable measure of layout parity. The tool also accepts `-TimelineSeconds`,
  `-TimelineIntervalMs`, `-ProbeDurationSeconds` and `-SkinFilter`; the focused
  LX-iPlay evidence is under `build/comparison/lx-info-30s-fixed/`. A fresh
  post-change Default + 23-package playing/paused/stopped run completed without
  a load failure, vanished window or process exit and is stored under
  `build/comparison/playback-all-info-fixed/`.

The first reviewed pseudo-code recovery map is documented in
`../reverse/semantic/RECOVERED_LOGIC.md`. It confirms the Win32 message loop,
TTBL/M3U dispatch, delayed atomic playlist saving, skin XML set, add-in factory,
sound-library lifecycle, and the bidirectional settings serializer.
The context-menu call graph and original command map are documented in
`../reverse/semantic/MAIN_WINDOW_CONTEXT_MENU.md`.
The corrected `wWinMain`/application-session boundary is documented in
`../reverse/semantic/WINMAIN_RECOVERY.md`.
The recovered startup-to-first-show ordering, settings offsets and runtime
window-style comparison are documented in
`../reverse/semantic/STARTUP_WINDOW_RECOVERY.md`.
The default-skin DLL resource path and its dual-source ZIP-reader evidence are
documented in `../reverse/semantic/DEFAULT_SKIN_RESOURCE_RECOVERY.md`.
The original-program three-skin comparison, four-frame control evidence and
runtime context-menu switching test are documented in
`../reverse/semantic/MAIN_SKIN_PARITY.md`.

- Popup-menu focus icons now follow `FUN_0046D5E7`: enabled unchecked command
  images receive the original `DrawStateW(GRAY_BRUSH, DST_ICON | DSS_MONO)`
  stamp at `(+1,+1)`, then the colored image is raised to `(-1,-1)` with
  `ILD_TRANSPARENT`; checked and disabled branches remain separate. Focused
  `千千选项` and `显示桌面歌词` icon regions are pixel-identical to the original,
  while submenu headers preserve the original no-stamp behavior. The legacy
  `0x8023 -> 0x7EFD` playlist alias retains its source bitmap's color-key mask
  so modern `comctl32` cannot turn its focused icon into a gray rectangle.
- Playlist blank-area clicks now retain native common-control state rather than
  treating selection and caret as one value. Physical original/rebuild probes
  for both `PlayLists` (`0x2801`) and `Files` (`0x2802`) match exactly:
  `LVIS_SELECTED 0 -> -1`, `LVIS_FOCUSED 0 -> 0`, and keyboard focus remains
  on the clicked child. Active-list and playing-track state remain unchanged.
- Lyric dragging now follows `004425F7`, `00442A13`, `00442671`, `0043FA1D`,
  `0043EF4D`, and `00441330`: capture begins only on non-text space; XML
  `ScrollMode=0` is vertical/Y-axis with a horizontal guide, while mode 1 is
  the variable-width horizontal/X-axis stream with a vertical guide. A physical
  original/rebuild fixture produces the same `0:06` target for vertical -20 px
  and `0:04` for horizontal -20 px. The `Lyric` node's full `LOGFONT`, text,
  highlight, and background colors now override skin defaults as in the original.
  Detailed evidence is in
  `../reverse/semantic/PLAYLIST_BLANK_LYRIC_DRAG.md`.
- Runtime skin profiles are no longer treated as playlist-only files. The
  `<skin>.skn.xml`/`Default.xml` Player, Lyric and PlayList groups are committed
  transactionally: lyric font/colors, all playlist colors, main/auxiliary
  rectangles and visibility change together, while global lyric interaction
  options remain intact. TT2012 now switches to main `342,669,669,826`, hidden
  lyric `379,1218,706,1340`, and hidden playlist `342,826,669,1050`; LX-iPlay
  restores its visible embedded playlist at `369,611,869,866`. Playlist and
  lyric regions are regenerated after their final profile sizes. Evidence is
  recorded in `../reverse/semantic/SKIN_PROFILE_SWITCH_RECOVERY.md`.
- Lyric animation now follows the control-private clock at `0043EB87`,
  `0043EDE6`, `0043F014`, `0044254A` and `0043F876`: the horizontal stream
  repaints at 20 ms, vertical rows at 50 ms, while the main timer-10 path keeps
  its original 250 ms period. A paint samples the decoder clock once, applies
  the current-line two-pass highlight, the 500 ms adjacent-line color blend,
  and `FadeIndex`'s axis-dependent per-pixel edge fade. Progress fill now uses
  the exact centre of the `00428F41` rounded thumb rectangle as required by
  `00451E07`; mouse mapping uses the same inset/centre span from `00452045`,
  eliminating the intermittent one-pixel split. `Fade_Windows`,
  `OpaqueWhenActive` and `AlphaPercent` now drive layered main/auxiliary skin
  alpha transitions through the recovered `0044EFBE` step model. Detailed
  evidence is in `../reverse/semantic/LYRIC_TIMING_PROGRESS_FADE.md`.
- The lyric display submenu and its four drawing modes now follow `0044D2EA`,
  `0044D327`, `0044D35E`, `0044D39D` and `0044B4A1`. Command `0x409` displays
  the opposite scroll action from DLL string `垂直滚动|水平滚动`; karaoke mode
  now enables (rather than disables) the clipped playback-boundary pass.
  `Transparent` uses `BkgndColor` as the layered color key, while `TransSkin`
  parks lyric chrome and expands the control to the complete popup. Original
  and rebuild menu IDs/text/check states/row sizes and both transparent control
  rectangles match in physical probes. Details are in `LYRIC_DISPLAY.md`.
- LX-iPlay's `mini_border="#1e1e1e"` is no longer drawn around the normal
  `LyricCtrl`. `004A88D0` stores those colors at skin offsets `+0xA2C/+0xA30`,
  but their sole paint consumer, `00449313`, reads them only in the
  `DAT_0054775C != 0` mini-player branch and applies the frame to the lyric
  popup client. Original/rebuild physical HWND captures now both have no
  rectangular outline around the normal `35,5,465,65` lyric child. Detailed
  evidence is in `LYRIC_WINDOW_RECOVERY.md`.
- Win32 Release playback no longer loses every decoded block when EQ is
  enabled. The ordinal-103 setup now preserves the original standalone
  `FUN_004B182C` call boundary, preventing VS 18 `/O2 /Ob2` from producing a
  zero-frame EQ stream. The end-to-end probe reads the deployed XML and checks
  decoder, processor, waveOut and advancing position; the clean Sandbox run is
  recorded under `build/sandbox-release-playback/`.
- The playlist-import Sandbox regression no longer appears indefinitely
  unresponsive. `ttplayer_tests` scans 173 skins and normally needs about 67
  seconds in a clean VM, so its former 60-second false timeout is now 180
  seconds with five-second progress heartbeats. OLE execution is bounded by a
  seven-second drop deadline plus one-second cancellation and a 15-second
  helper-process deadline; all cross-process messages use a 2.5-second
  `SendMessageTimeoutW` guard. Guest progress and completion files are
  atomically renamed, a 60-second stale heartbeat aborts the run, and Sandbox
  environment/session cleanup uses shared finite budgets.
- Each import case now removes only its validated generated `%TEMP%` runtime
  after copying evidence. This prevents the former roughly 46 MB per-scenario
  leak from approaching 1 GB over repeated matrices and slowing later original
  launches. Completed isolated evidence is Base `20260904-212933` (9
  scenarios/18 runs), Dialog `20260904-213147` (4/8), and native-test report
  `20260904-210505` (all four exits 0 without timeout). Focused Extended reports
  `20260904-220956`, `20260904-221045`, `20260904-221519` and
  `20260904-221556` all have `Success=true` for embedded CUE and catalogue
  ordering/selection. They resolved a stale oracle found during expansion;
  complete Extended report `20260904-222201` then passed 17 scenarios/34
  original-and-rebuild runs, with its final marker also recording success.
- The remaining Base flake was a fixture race: a two-second command-line WAV
  was followed by OLE input after only 1.2 seconds. The probe now waits three
  seconds, requires three consecutive bounded UI replies, and disables the
  unrelated online lyric lookup in the network-disabled VM. Current Base
  report `20260904-225446` passes 9 scenarios/18 runs with no forced close.
- Audio open and UI-issued stop no longer wait indefinitely for a private
  reader: their budgets are four seconds and 1.5 seconds respectively. Worker,
  plug-in and module ownership remains intact until safe teardown. A deliberate
  seven-second reader stall is covered by `audio_recovery_tests`; current
  Sandbox report `20260904-225758` records all four native executables at exit
  code 0 with no timeout.
- Skin-menu catalogue scanning starts asynchronously with the root context
  menu. Expanding the Skin submenu now publishes only a completed snapshot and
  never waits on the UI thread; a still-running or stale generation is retried
  by the existing finite poll path. Stale or shutting-down scans receive a
  cooperative cancellation token and stop at the next package boundary without
  detached threads or borrowed-module lifetime violations.
- The original 15-page modeless Options shell accepts all 13 routes exercised by
  the probe: one physical main-window context-menu route and 12 command/private-
  message routes. That historical probe injected the media-library and independent
  desktop-lyric routes directly, so it proves route acceptance rather than the
  source runtimes; both source runtimes are now covered by later implementation and
  focused tests. The initial 48-cell `ColorSelectCtrl` popup
  is modeless and has finite cancel/capture/owner-destruction paths without a
  nested message loop; its `Custom...` route still opens the modal system
  `ChooseColor` dialog. The color probe verifies structure and routing, not
  pixel-by-pixel palette or system-dialog parity. Current color evidence is
  `rebuild/build/host-settings/20260905-170304-color-final-current`.
- Final direct-host `Both / All` evidence is
  `rebuild/build/host-settings/20260905-165607-all-final-current`: 20 original and 20 rebuild cases,
  390 page visits, no execution/behavior/parity failures, no error and no forced
  termination. The report does not record executable hashes. The device page
  now matches all 13 original visits at eight
  devices with selection index four by applying the original KS render-alias,
  exclusive-overlapped-open and sink/input/interface/medium pin filters.
- Output-device discovery and the currently recovered detail fields run in a
  same-bitness worker (the player's private mode since 2026-09-11, formerly
  `ttplayer_output_device_probe`). First page creation waits
  synchronously for up to four seconds and a failed helper gets up to one further
  second for Job-backed termination; this is finite isolation, not asynchronous
  refresh. Successful catalogue snapshots preserve device order and keys.
- ASIO registry discovery and `004E207B`'s four capability values are present:
  output channels, the fixed 13-rate `canSampleRate` table, first-output valid
  bits and preferred buffer frames. The same-bitness helper formats the exact
  four rows before publishing its bounded snapshot; this intentionally moves
  the original selection-time lazy call out of the UI process. Native ASIO
  playback opens the selected CLSID and fails explicitly rather than falling
  back. `waveOut dwSupport == 0` now preserves the
  original three empty detail rows, although the final matrix did not capture
  those strings directly. This host exposed no accepted KS/ASIO item and did not
  inject missing, timed-out, crashed or malformed helpers, so the report is not
  evidence for those physical-device or fault branches.
- All generic file, multi-file, folder and save selectors now use the shared
  Vista+ `IFileOpenDialog`/`IFileSaveDialog` path. This includes main/playlist
  intake, playlist import/export, lyric association and editor Save As, EQ and
  visual profiles, Options profiles/folders/icon-file selection and skin-preview
  export. The original post-selection transactions remain separate; playlist
  export supplies the recovered `ttpl` default extension and writes the native
  version-4 XML form. The direct-host main/playlist multi-file probes under
  `build/host-modern-dialog-review-20260905-203334/` and
  `build/host-modern-dialog-review-20260905-playlist/`
  record the modern DirectUI/Shell view classes, expected rows, responsive
  owners and no forced termination. Lyric editor Save As and association are
  covered by direct-host `build/host-modern-final/lyrics-modern-dialogs.json`;
  both expose the modern Shell view, cancel physically with Escape, leave the
  main window responsive and exit without forced termination.
- Options `SC_CLOSE`, Cancel and OK now converge on the recovered modeless apply
  and destroy path. Direct-host reports
  `build/host-options-modern/20260905-193322-close/` and
  `build/host-modern-icon-final/options-modern-dialog-host-report.json` cover all
  four close routes and the modern folder, recursive-folder, profile and icon-file
  selectors without a forced process stop.
- Internal track dragging onto the catalogue now preserves the pressed Ctrl
  modifier and copies the full selected set into the target list; releasing on
  toolbar/chrome/outside cancels instead of becoming a first/last-row reorder.
  The direct-host original/rebuild comparison at
  `build/host-final/20260905-201021-catalogue-ctrl/` passes with matching target
  selection and list fingerprints. Main playlist/EQ/lyric skin buttons derive
  their persistent checked frame from actual auxiliary-window visibility and
  return to the exact normal frame after each skinned close; rebuilt-host
  evidence is `build/host-final/auxiliary-button-state-rebuild.json`.
- Desktop lyrics now owns the original `DeskLrcCtrlClass`,
  `DeskLrcPaintClass` and `DeskLrcBarClass` topology. Commands `0x8039` and
  `0x8038`, layered paint, scroll, move/resize, lock/click-through, skin data,
  playback time, settings and the 12 toolbar controls are connected to the
  player lifecycle. Direct-host `build/host-desktop-lyrics-rebuild.json` matches
  the supplied original's owner chain, rectangles, styles and control ordering.
  The root context-menu command changes from `0x8040` (lock) to `0x8041`
  (unlock) even after click-through is enabled; direct-host evidence is in
  `build/host-final/desktop-lyrics-main-menu.json`. A physical host click of
  `0x8040` changes `DeskLrcCtrlClass` from ex-style `0x80088` to `0x800A8`
  (`WS_EX_TRANSPARENT`) in
  `build/desktop_lyrics_lock_host_probe_20260905.json`.
- The desktop-lyric input path now follows `004167E5`/`00419D6D`/
  `00419E3E`/`00419E1B`: the alpha-zero control is primed by the 100 ms
  `0041A341` hover timer, `0041A0D1` restores the configured background alpha
  and toolbar on `WM_SETCURSOR`, and `DeskLrcCtrlClass` captures client mouse
  input for grouped move and custom 4-pixel edge resizing. The recovered
  `004196D5` ten-pixel work-area snap, `00419983` toolbar-reachability rules,
  toolbar drag-anchor correction, `IDC_HAND` interior cursor and
  `004B2578` vertical-resize font correction are also live. Drag repaint is
  posted to the Paint surface as in `004164CE`, and the original `+0x104`
  manual-resize gate prevents an ApplySettings height recalculation. Physical
  host comparisons of the supplied original and rebuilt Release each pass 13/13
  movement, resize, capture, lock/pass-through, responsiveness and clean-exit
  assertions in `build/host-desktop-lyrics-original-mouse.json` and
  `build/host-desktop-lyrics-rebuild-mouse.json`; Windows Sandbox was not used.
