# TTPlayer clean reconstruction

<img width="654" height="463" alt="PixPin_2026-09-11_04-50-19" src="https://github.com/user-attachments/assets/c38933c0-aa58-458b-a0b2-929593e1669e" />

This is a clean, buildable rewrite around the recovered behaviour and file/ABI
boundaries. It intentionally does not compile Ghidra pseudo-C directly.
The current address-by-address status for AddIn, native outputs, PCM, fades,
media library, tags, CD/VCD, URL, associations, settings and startup is in
[`MAJOR_FEATURE_RECOVERY.md`](MAJOR_FEATURE_RECOVERY.md).

Current milestone (0.1):

- native Unicode `wWinMain` reconstructed at the original `004C0E8F` boundary:
  EXE-local `ttpcomm.dll` loading without version pinning, single-instance event/mapping and
  `WM_COPYDATA` forwarding, TLS/OLE/resource/sound/CoolSB lifecycle, followed
  by a separate application-session message loop;
- exact `<DEFAULT_SKIN>` ZIP read at runtime from the loaded `ttpres.dll` with
  the recovered `FindResourceW`/`LoadResource`/`LockResource` path, plus the
  original file-backed branch for external `.skn` packages, tolerant GBK/UTF-8
  `Skin.xml` parsing, bitmap state strips, color-key window regions and
  XML-positioned controls;
- file picker/drag-and-drop, playlist selection, previous/play-pause/next/stop,
  volume/balance control, playback status, and command-line audio/playlist opening;
- Windows-native taskbar thumbnail previous/play-pause/next controls with live
  playback state, without restoring the minimized player; this requested modern
  extension is documented in [TASKBAR_PLAYBACK.md](TASKBAR_PLAYBACK.md);
- main-window `WM_CONTEXTMENU` behaviour rebuilt from `CPlayerWnd`: original
  `ttpres.dll` menu hierarchy/text, dynamic current-playlist and installed-skin
  submenus, transparency levels, playback-mode checks and command dispatch;
- original-program-verified default, Let's Vista and TT2012 main-window
  rendering: exact dimensions/styles, four-state bitmap slicing, package icon,
  stopped-state LED/text and live right-click skin replacement;
- original-style bounded output pipeline with selected waveOut, DirectSound,
  native KS and ASIO backends; the recovered x86 reader/decoder/buffer adapter
  is validated with APE, TAK, VQF and the supplied FLAC. AddIn, Media
  Foundation URL/WAV/MPEG, native AIFF/AIFC/AU, exact CUE and raw CD-DA feed
  one ReplayGain/EQ/Surround/Winamp-DSP/SSRC/output-bit/dither chain with the
  recovered play/pause/seek/stop/end fade state machine;
- UTF-8 M3U8 playlist load/save and sequential/repeat/shuffle navigation;
- recovered playlist catalogue HWND/custom-draw contract: hidden
  `TreeCtrl(0x2800)`, visible `ListCtrl(0x2801/0x2802)`, active-list versus
  focused-selection state, selected image/gradient/focus frame and original
  enter/leave behaviour for toolbar and close controls;
- recovered normal/mini `TTPlayer_LyricWnd`: package `lyric_window`/`Lyric.xml`
  layout, independent mode geometry/visibility/top-most, mini solid-frame and
  horizontal-resize contract, fixed and nine-slice normal popup, original child HWND/command set,
  resource-backed menu/tooltips, UTF-8/UTF-16/ANSI local LRC loading,
  association/reload/copy, TOM-backed RichEdit editor with protected live tag
  coloring, resource toolbar and focus-scoped Ctrl+X/C/V/Z/Y accelerators,
  timed multi-row paint, smooth motion, captured row dragging, arrow seek and
  500 ms lyric-timeline wheel adjustment; runtime parity evidence is in
  `LYRIC_WINDOW_RECOVERY.md`;
- recovered full-screen state machine: the existing `VisualCtrl` and
  `LyricCtrl` are detached/reparented with their original style transitions;
  opaque lyrics, desktop-transparent lyrics, split and overlay combined
  layouts use the independent `*FS` settings and native resource 143/145/390
  context menus. See `FULLSCREEN_RECOVERY.md` and the reproducible
  `tools/windows_sandbox/` comparison harness;
- recovered x86 `ttpGetSoundAddIn` adapter: the four real reader, decoder,
  encoder and lyric-search-provider categories, retained module/factory/session
  lifetimes, `IStream`, complete extensible formats, six-slot PCM buffers,
  read/EOF/seek/reset and executable encoder Configure/Open/Start/Write/Finalize;
  the file picker and converter expose a format only after its creator succeeds;
- recovered reader metadata and thumbnail read/write slots, isolated file-info
  transactions, built-in MP3 ID3v1/ID3v2/APEv2 preservation, ReplayGain,
  one-based CUE sub-tracks, CDA conversion/grabbing source, URL dialog/source,
  and EXE-local monitored `Music.library`; detailed evidence is in
  `AUDIO_RECOVERY.md` and `MAJOR_FEATURE_RECOVERY.md`;
- recovered `CSkinParser_ParseEqualizerWindow` layout and the independent
  `WS_POPUP/WS_EX_TOOLWINDOW` equalizer: enable/profile/reset/close buttons,
  balance, surround, preamp, ten EQ bands, presets, profile files, capture
  drag/track-click/hover/Esc/arrow input, native-size clipped slider fills,
  owner-drawn profile menu, window snapping, skin switching and runtime ordinal
  103/104 DSP refresh; real `SkinButton`/`SkinSlider` child HWNDs restore the
  original per-control capture path. Live slider tracking temporarily writes
  the resource-backed balance/surround/EQ value into the main skin `status`
  field and restores playback status on release. Resource-backed hover text now
  follows the original split registration: main/equalizer callback tools, the playlist's
  native eight-tool companion, and its shared explicit-string hover tools,
  with the recovered pre-translate `MSG` relay and no duplicate tooltip
  subclass/periodic-update path;
  evidence is in `EQUALIZER_RECOVERY.md`;
- legacy `TTPlayer.xml` import/save and runtime consumers for player, playback,
  device, conversion, media-library, history, hotkey, visual, lyric and desktop-
  lyric settings; `/reg` and `/unreg` use safe per-user association transactions;
- `.skn` ZIP central-directory inspection and compatibility validation.
- recovered resource-only `ttpres.dll` targets for both the application root
  and `AddIn`, preserving every original resource payload and language ID.

Build from a Visual Studio developer shell:

```powershell
cmake -S rebuild -B rebuild/build -A Win32
cmake --build rebuild/build --config Debug
ctest --test-dir rebuild/build -C Debug --output-on-failure
```

Run the reconstructed player UI from the repository root so it can import the
legacy `TTPlayer.xml` settings automatically:

```powershell
.\rebuild\build\Debug\ttplayer_rebuild.exe
```

A supported audio or playlist path may also be passed directly. The program first renders the
original 327x141 default player skin; the native dark window is retained only
as a safe fallback when a skin package cannot be loaded.

Recovered resource DLLs are written to:

```text
rebuild/build/resources/root/ttpres.dll
rebuild/build/resources/AddIn/ttpres.dll
```

The Win32 target is deliberate: all legacy add-ins and `ttpcomm.dll` are x86.

## UI source layout

`PlayerWindow` remains one coordinator class so its HWND ownership, message
ordering and reconstructed private state stay ABI-neutral, while each native
window now has a conventional implementation unit:

- `src/ui/player_window.cpp`: main player, skin switching, shared menu support,
  persistence and playback coordination;
- `src/ui/player_window_lyrics.cpp`: `TTPlayer_LyricWnd` and `LyricCtrl`;
- `src/ui/player_window_playlist.cpp`: `TTPlayer_PlayListWnd`, list controls and
  their tooltip/edit handling;
- `src/ui/player_window_equalizer.cpp`: `TTPlayer_EqualizerWnd`, `SkinButton`
  and `SkinSlider`;
- `src/ui/player_window_visual.cpp`: `VisualCtrl`, PCM-driven dream/spectrum/
  scope renderers, reader-owned album covers and the full-screen state machine;
- `src/ui/player_window_visual_options.cpp`: resource dialog 253, visual
  settings/profile commands and their immediate renderer refresh path;
- `src/ui/player_window_internal.h`: private command IDs and shared drawing,
  menu and geometry declarations; it is not part of the public API.

The split is structural only: window class names, control IDs, resource IDs,
message dispatch order and recovered method bodies are unchanged.

## Compatibility policy

- New modules expose typed C++ interfaces.
- Legacy DLLs remain behind the x86 sound-library adapter; private object and
  buffer vtables never escape the plugin/audio boundary.
- `.skn`, M3U8/LRC, and selected `TTPlayer.xml` settings are preserved first.
- Private metadata and processing slots stay behind guarded x86 adapters; a
  format or processor is exposed only after its creator and initialization
  actually succeed.
- Accepted album-art payloads use WIC first and call `OleLoadPicture` only
  when WIC decoding fails; reader selection and legacy MIME admission remain
  unchanged. Cover frames are composed off-screen and transferred atomically;
  the parent skin painter excludes the live `VisualCtrl`, preventing its
  250-ms UI refresh from exposing a background-only frame. Analysis and the
  Windows Sandbox regression are recorded in `COVER_FLICKER_RECOVERY.md`.
- Visualization recovery evidence and the exact command/configuration mapping
  are recorded in `VISUALIZATION_RECOVERY.md`.

## Remaining validation/recovery boundaries

1. validate CD-DA, KS and ASIO on compatible physical hardware;
2. extend real-sample validation across TTA/MPC/Real/MOD and unusual
   multichannel/valid-bit layouts;
3. recover the lyric-provider request/result/cancel ABI or replace the retired
   service with an explicitly non-original provider-neutral client;
4. retain explicit boundaries for retired MSN/update/FreeDB/recommendation/
   cache/download services instead of reporting simulated success;
5. the old private WTL command-bar/button/CBT wrapper object layouts and exact
   C++ source cannot be inferred from pseudo-C alone; preserve their observable
   duties through the owned Win32 implementation.
