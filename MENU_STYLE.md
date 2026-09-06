# Popup menu style recovery

This reconstruction targets the observable Win32 behavior of the supplied
TTPlayer binary. Decompilation cannot prove source-level or binary identity
with the lost private implementation.

## Recovered call path

- `FUN_0046EE5B` enters the command-bar popup path and calls `FUN_0046A27D`.
- `FUN_0046A27D` installs the menu hook, calls `TrackPopupMenuEx`, and restores
  the temporarily changed menu records after the modal loop.
- `FUN_00469D09 -> FUN_0046EE2F/FUN_00470482` handles
  `WM_MEASUREITEM`. Normal rows use the popup menu font, a 20-pixel icon
  column and a 20-pixel minimum height; separators use half `SM_CYMENU`.
- `FUN_00469CD8 -> FUN_0046D5E7` handles `WM_DRAWITEM`. The flat-menu path
  paints a three-stop icon-column gradient, `RGB(252,252,249)` text area,
  `RGB(193,210,238)` selected fill, and `RGB(49,106,197)` selected frame.
- `FUN_004703E5` draws the label before a tab on the left and the accelerator
  after it on the right, while honoring keyboard-cue state.
- `FUN_0048B5AB` loads 16x16 command images from toolbar/bitmap resources
  `0x82`, `0x83`, and `0x94`. The rebuild parses the same `RT_TOOLBAR`
  command sequence and adds the original command aliases.
- `FUN_00465CB8` inserts special item `0xFFFA` before the main popup.
  `FUN_0046EE2F` measures it as a zero-height side-column item and
  `FUN_004700D9` paints the vertical `千千静听--尽听精彩` brand strip.

The main player and owned playlist window both route `WM_MEASUREITEM` and
`WM_DRAWITEM` through this shared implementation. All nested submenus are
converted for the lifetime of the popup and destroyed only after the modal
menu call returns.

## Skin-list population (`0045E518`)

The native skin submenu is populated lazily from `WM_INITMENUPOPUP`, identified
by its fixed first command `0x7919`. The rebuild keeps that HMENU publication
point, but intentionally starts the package scan asynchronously when the root
right-click menu opens. Consequently the root popup remains immediate and the
time spent navigating to Skin overlaps the EXE-local `Skin` directory scan. If
the submenu is expanded first, `WM_INITMENUPOPUP` waits for the unfinished tail
and then publishes the complete snapshot; no worker thread inserts menu items
or accesses a window handle.

Each scan enumerates only `.skn`/`.zip` files below the executable-local `Skin`
directory and reads that package's `Skin.xml`. Candidates for which ZIP
loading, XML parsing, or root `version=2` validation fails are released without
receiving a command.

The menu title is the root `name` attribute, not the package stem. External
objects are sorted by `FUN_004C54D1`, which dynamically resolves
`shlwapi!StrCmpLogicalW` and falls back to `lstrcmpiW`; duplicate names are
retained and no filename tie-breaker is added. Commands are then assigned
contiguously from `0x791A`. The embedded resource object is inserted at vector
index zero for command `0x7919`, although its visible label remains the fixed
resource text `<默认皮肤>`.

`CSkinParser_ParseMetadata` also retains `author`, `url`, and `email`, stripping
a case-insensitive leading `mailto:` from the last field. The popup tracking
tooltip formats resource `0x81C3` exactly as
`作者: %s\n主页: %s\n邮箱: %s`, uses `GetMenuItemRect` for the selected row,
and observes the common-control initial delay. Before `TrackPopupMenuEx`, the
system submenu delay is temporarily set to the native 400 ms and restored
after the modal loop. Long lists remain a single native `#32768` menu, so
Windows supplies the top/bottom scroll arrows.

The metadata decoder deliberately extends the native lexer by accepting an
optional UTF-8 BOM. This makes the otherwise valid `004941_088.skn` appear as
its XML name `M&G-Kaddish` and lets the normal dynamic command path switch to
it. The supplied original binary rejects that package, so this one behavior is
an explicit compatibility improvement rather than a claim of binary parity.
Duplicate internal names and their independent command entries remain intact;
hovering `ayu3` still yields the same three-line author, home-page, and email
tooltip.

## Physical comparison

`tools/probe_playlist_context.ps1` can now capture the main popup, playlist
item/title popups, selected rows, and opened submenus. It reads the live menu
handle with `MN_GETHMENU`, records every item rectangle/state, and captures
the compositor-visible pixels.

Measured original/rebuild results on the same Windows session:

| Surface | Original | Rebuild |
| --- | --- | --- |
| Playlist-title normal row | `130 x 20` | `130 x 20` |
| Playlist-title separator | `130 x 10` | `130 x 10` |
| Main side item | `17 x 0`, ID `0xFFFA` | `17 x 0`, ID `0xFFFA` |
| Main normal row | `121 x 20` | `121 x 20` |

The playlist-title item IDs, owner-draw/default/disabled states, row sizes,
hover fill and separator positions match. Main-menu root differences in the
high byte of `GetMenuState` reflect different dynamic submenu item counts
(installed skin and playlist contents), not drawing flags. Reference and
rebuild captures are stored under `build/comparison/menu-style/`.

## Final `004700D9` correction

The side caption is not painted one character at a time. The command-bar
branding flag selects the upright `DT_WORDBREAK` branch for the localized
caption. The implementation now copies `lfMenuFont` from the 500-byte
`NONCLIENTMETRICS` layout, overrides only the fields seen in the pseudo-code
(SimSun, height 14, weight 400, antialias quality), performs the original
`DT_CALCRECT` pass, and repeats the exact dark/highlight rectangle offsets.
The complete 25-by-376 side region is pixel-identical to the original capture.

The resource image path also follows `0048B5AB/0045FAD8`: 24-bit toolbar
strips are loaded as DDBs, the executable supplies group icon `0x80`, and
enabled images use `ILD_TRANSPARENT`. The executable manifest now activates
Common Controls 6 just like the supplied binary, so the icon's alpha channel
is composited instead of exposing premultiplied black pixels. Mouse-opened
main menus and the separate playlist command bar retain their independent
keyboard-cue state.

## Focused image branch (`0046D5E7`)

The selected icon path is separate from the selected-row fill. For an enabled,
unchecked command image the original first obtains an `HICON` with
`ImageList_GetIcon`, calls `DrawStateW` with stock object `2` (`GRAY_BRUSH`) and
flags `0x83` (`DST_ICON | DSS_MONO`) at `(x + 1, y + 1)`, then moves the colored
image to `(x - 1, y - 1)` and draws it with `ILD_TRANSPARENT`. Checked images
retain the inset blue frame; disabled images continue through
`DST_ICON | DSS_DISABLED`. Resource-submenu headers and the legacy playlist
play/properties aliases retain the observed no-stamp result while keeping the
one-pixel raised color-image offset.

The old `0x8023 -> 0x7EFD` playlist alias needs the toolbar bitmap's original
`RGB(192,192,192)` color-key mask. Current `comctl32` can expose that image as
an all-opaque `HICON` to `DSS_MONO`; the rebuild therefore preserves the
one-bit mask while loading the 24-bit strip and applies the same gray stamp
through that mask.

Same-session captures now give zero changed pixels in the complete 24x20 icon
region for focused `千千选项` and `显示桌面歌词` rows. The focused `播放控制`
submenu header differs only in the already-known 28 low-order color-rounding
pixels (total RGB delta 49, maximum per-pixel delta 3); no gray rectangle or
incorrect focus stamp remains.
