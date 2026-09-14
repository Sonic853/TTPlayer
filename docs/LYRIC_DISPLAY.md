# Lyric display-mode recovery

This note records the normal and mini lyric-window display modes recovered from
`TTPlayer.exe.pseudo.c` and verified against the original executable. The
commands come from `ttpres.dll` menu resource `143` (`0x8f`); the rebuild does
not duplicate their Chinese labels in source.

## Command dispatch

The original lyric-window dispatcher routes these menu commands through
`FUN_0044D2EA`, `FUN_0044D327`, `FUN_0044D35E` and `FUN_0044D39D`, followed by
the `0x7f0/0x10` refresh branch in `FUN_0044B4A1`:

| Command | Resource text | Recovered state change |
| --- | --- | --- |
| `0x409` | `垂直滚动|水平滚动` | Toggle the active normal/mini lyric scroll layout and reinstall timer `0x7b`. |
| `0x866` | `卡拉OK方式` | Toggle per-line playback-boundary coloring. |
| `0x867` | `以背景色透明` | Toggle the lyric background color key. |
| `0x861` | `淡入淡出当前行` | Toggle the 500 ms adjacent-line color transition. |

`0045Axxx` does not check `0x409`. It splits string resource `0x409` and
renames the item to the *other* layout: XML `ScrollMode=0` displays vertical
rows and therefore offers `水平滚动`; mode `1` displays one horizontal stream
and offers `垂直滚动`. Commands `0x866`, `0x867` and `0x861` are checked from
their current Boolean settings. `0x8036` similarly reflects the mouse-wheel
timestamp-adjust option.

`FUN_0043EDE6` selects a 50 ms animation timer for vertical rows and 20 ms for
the horizontal stream. Changing mode cancels lyric dragging, replaces the
timer, recomputes placement from the decoder clock and repaints `LyricCtrl`.
The same preparation and command-dispatch path is used for both menu `0x8f`
opened directly on the lyric window and the copy grafted into the main-player
context menu by menu `0x8a`.

## Independent normal and mini settings

The settings constructor initializes the integer at `+0x2B4` to `0` and the
next integer at `+0x2B8` to `1`. The XML serializer names them `ScrollMode`
and `ScrollMode2`; `ScrollModeFS` lives separately at `+0x344`.
`FUN_00401CA7` is the selector used by command `0x409`: it returns `+0x2B8`
when the mini-mode flag `DAT_0054775C` is set, `+0x344` for full-screen mode,
and `+0x2B4` otherwise. Therefore the normal default is vertical rows while
the mini default is the single horizontal stream.

The rebuild mirrors that selector for painting, hit testing, drag axis,
cursor, dynamic menu label and the 50/20 ms animation timer. Entering or
leaving mini mode cancels an in-progress lyric drag and immediately installs
the selected mode's timer. Shutdown writes all three XML attributes, so a
mini-window menu change no longer mutates or overwrites normal mode.

Per-skin `.skn.xml` files intentionally do not participate in this selection.
The original `DAT_00547744` profile branch stores window rectangles,
visibility, lyric font/colours and playlist appearance only. All installed
sidecars are loaded in the compatibility test with sentinel values for
`ScrollMode`, `ScrollMode2` and `ScrollModeFS`; the test requires all three to
survive every profile load unchanged.

## Painting semantics

`FUN_0043FC10` tests the control's `KaraokeMode` field at offset `+0x80`.
When it is zero, the current line is drawn completely in `HilightColor`. When
it is one, the line is first drawn in `TextColor` and then redrawn through a
playback-dependent clip in `HilightColor`. The earlier rebuild had this test
reversed. In horizontal mode the clip boundary remains the stationary client
centre while the stream moves underneath it; in vertical mode the boundary is
the measured glyph width multiplied by elapsed/line duration.

`FadeHilight` is the independent field at `+0x7c`. `FUN_0043FC10` blends the
previous line from highlight to normal during the first 500 ms of a new line.
Only non-karaoke mode pre-blends the upcoming line in the final 500 ms. This is
separate from `FadeIndex`: `FUN_004416E0` performs the axis-dependent bitmap
edge gradient and `FUN_0044B4A1` forces that effective index to zero while
background color-key transparency is active.

## Transparent and TransSkin modes

The global `Lyric` attributes `Transparent` and `TransSkin` are now loaded and
saved. `FUN_004494E5` applies `WS_EX_LAYERED` with `BkgndColor` as
`LWA_COLORKEY`, combined with the current skin-window alpha. During initial
configuration `FUN_0044B4A1` also forces `LOGFONT::lfQuality` to
`NONANTIALIASED_QUALITY`, preventing color-key fringe pixels around glyphs.

`FUN_00449313` and `FUN_004495C8` distinguish two transparent layouts:

- `Transparent=1, TransSkin=0` retains the skin chrome and its normal lyric
  rectangle; only pixels matching `BkgndColor` disappear.
- `Transparent=1, TransSkin=1` parks the close/on-top/desktop-lyric children,
  omits both normal skin chrome and mini borders, and expands `LyricCtrl` over
  the complete client so the resulting popup contains lyric glyphs only.

Disabling transparency restores alpha-only layered composition, the original
skin chrome, button positions, lyric rectangle and `FadeIndex` edge gradient.

## Runtime verification

Physical context-menu probes against both executables produce the same display
submenu: five items with IDs `0x409`, separator, `0x866`, `0x867`, `0x861`;
normal rows are `133x20` and the separator is `133x10`. With every Boolean on,
the three toggle rows report the same owner-draw/check state `0x108`, and the
scroll command reads `垂直滚动` in both executables.

Using the same LX-iPlay fixture and a `500x70` lyric popup, original and rebuild
both use `135,305..565,365` (`430x60`) for transparent skin-visible mode and
`100,300..600,370` (`500x70`) for transparent text-only mode. In a ten-second
line fixture, non-karaoke rendering contains 77 exact highlight-color pixels
in both executables; karaoke rendering contains 27 in the original and 28 in
the rebuild, the one-pixel difference coming from the independently sampled
decoder clock at pause. The progressive/whole-line branch selection is the
same.

The normal/mini probe now accepts separate initial scroll modes. Starting both
executables with `ScrollMode=0, ScrollMode2=1`, entering mini mode, dispatching
command `0x409`, returning to normal mode and exiting produces
`ScrollMode=0, ScrollMode2=0` in both files. This verifies that the command
changed only the active mini field; normal mode remained vertical.
