# Preserve "Always on top" across window transitions

## Cause and recovered behavior

The old rebuild preserved the `TopMost` setting and menu check but lost the
actual Win32 window band. `UpdateLyricWindowSkin` and
`ApplyActiveLyricWindowState` combined geometry with
`SetWindowPos(lyric, HWND_NOTOPMOST, ...)` whenever the lyric's independent pin
was off. Since LyricWnd is owned by the main window, that operation also
demoted the pinned main window. Both skin replacement and mini-mode switching
used this path.

The same problem existed one level deeper: changing desktop lyric settings
could demote DeskLrcCtrl, its LyricWnd owner and then the main player. Conversely,
unpinning the main window could remove the independent lyric/desktop pin.
This is native owner/owned-window propagation, not a missing XML save.
See [Microsoft's SetWindowPos remarks](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos).

Evidence from the 5.7.9 `reverse/decompiled/TTPlayer.exe.pseudo.c`:

| Address | Relevant behavior |
| --- | --- |
| `00449451` → `0044E5CE` | Rebind the existing lyric window; geometry uses flags `0x16` and `0x14`, both containing `SWP_NOZORDER`. |
| `0046ACE2` | Equalizer rebind likewise preserves Z order while resizing/repositioning. |
| `00464B6C` | Mini transition uses `0x14` for geometry, switches independent normal/mini state, then separately updates topmost using `0x13`. |
| `004A3A66` | Main menu chooses `DAT_00547764` or `DAT_005477E0`; pin/unpin is an explicit topmost operation. It also refreshes desktop lyrics. |
| `0041964D` / `00453CC4` | Apply the effective desktop topmost state to all three desktop lyric HWNDs. |

The source retains `TopMost`/`TopMost2` and `LyricTopMost`/`LyricTopMost2` as
independent preferences. An intentionally unpinned mini mode is not changed
into a pinned mode merely because normal mode is pinned. No user XML or skin
sidecar needs migration. TTPlayer6120 is not used as a reference here.

## Correction

- Skin/mode geometry uses `SWP_NOZORDER`; extended-style replacement preserves
  the current native `WS_EX_TOPMOST` bit. `SetWindowPos`, not a style write,
  performs deliberate band changes.
- `ApplySkinWindowTopMost` reconciles the owner first, then playlist/EQ,
  then lyrics, then all three desktop lyric windows. Effective lyric topmost
  is the active main pin OR active lyric pin; desktop topmost is its own pin
  OR its owner's effective pin. Stored preferences are not overwritten.
- Hidden playlist/EQ HWNDs are reconciled explicitly. The native test exposed
  stale topmost bits there after a mini transition even with the main corrected.
- Reconciliation occurs after mini/lyric visibility restoration as well as
  after skin binding, main/lyric pin commands, options-wide application and
  fullscreen return. A test also caught a lyric band change after the mini
  show sequence; applying the policy only before `ShowWindow` was insufficient.
- Desktop settings, visibility and toolbar pin operations respect their
  owner's native band. A narrow `RefreshTopmost` avoids rebuilding fonts or
  repainting the desktop toolbar just to update Z order.
- Unchanged bands are not reordered. There is no timer repeatedly forcing
  the player above other applications, and no window recreation.

## Host verification — 2026-09-13

The existing native skin-rebind regression failed before the fix with
`skin switch lost the main window's configured topmost state`; its expanded
four main/lyric pin combinations now pass. The new
`src/ui/window_topmost_tests.cpp` covers the new contract independently of
the locally ignored `tests/` directory.

The new regression launches a copy of itself in a unique temporary EXE-local
runtime containing only fixture DLLs and skins. No user settings, playlists,
media, Discord connection or audio output is used. It asserts native
`WS_EX_TOPMOST` on seven real HWNDs, not just settings booleans, and covers:

- Normal and mini startup; normal/mini pins on/off independently.
- Default, LX-iPlay, TT2012 and Let's Vista skin commands, repeated sidecars,
  and a skin replacement that exits mini mode without replacing the main HWND.
- Main, lyric and desktop pin combinations; settings and hidden/shown desktop
  surfaces; normal/desktop lyric mode changes while entering/leaving mini.
- Rapid queued mini toggles during the asynchronous window fade.
- Hidden/shown windows with window fades enabled/disabled, minimize/restore,
  actual shadow class changes and the `0xFFFF` options application path.
- Lyric, visual and combined fullscreen control detach/reparent/restore with
  small drawing surfaces. This checks the real restoration path but does not
  claim a music-playing, full-monitor visual comparison.

Release builds successfully. All eight targeted host CTest cases pass:
`window_topmost_tests`, `skin_rebind_tests`, `desktop_lyrics_menu_tests`,
`taskbar_playback_tests`, `skin_profile_tests`, `fullscreen_lyric_drag_tests`,
`fullscreen_album_tests`, `skin_png_tests`. The expanded topmost regression
also passed independently before the combined run.

The verified executable was copied to `build/Release/ttplayer_rebuild.exe`
(SHA-256 `37D4FC447D2FC50291EFE7CD93107CBF65AC572B4F27AE638F988B675E089E39`).
The previous EXE is retained as
`out/png-6120/ttplayer_rebuild.before-topmost-20260913-151627-957.exe`.
Deployment verified that `build/Release/TTPlayer.xml` and
`build/Release/Skin/Default.xml` remained unchanged; no configuration or skin
files were copied into the user's runtime.

This restores the verified state/ownership boundary, not binary-identical
private classes or every possible third-party skin and Windows shell variant.
