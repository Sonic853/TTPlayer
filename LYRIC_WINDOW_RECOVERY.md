# Lyric window recovery

This note records the reconstruction of the normal and mini, skinned lyric popup
(`TTPlayer_LyricWnd`).  The desktop overlay (`DeskLrcCtrlClass`,
`DeskLrcPaintClass`, and `DeskLrcBarClass`) is a separate subsystem in the
original executable and is not treated as an alias for this window.

## Recovered object and skin path

- `CPlayerWnd` constructs the lyric popup object at player offset `+0x1C24`
  through `0046AACA`.  Offset `+0x2944` belongs to the equalizer and must not
  be reused for lyric state.
- `004A7929` dispatches the `<lyric_window>` node and `004A88D0` parses its
  `position`, `resize_rect`, `resize_tile`, root `image`, `title`, `close`,
  `ontop`, `desklrc`, child `lyric` rectangle, and `mini_border` colors.
- The package-local `Lyric.xml` supplies the exact `LOGFONTW`, normal text,
  highlighted text, and background colors.  Font descriptors retain the
  original fourteen-field serialized `LOGFONTW` form.
- Fixed-size packages keep their bitmap dimensions.  Resizable packages use
  the parsed nine-slice rectangle/tile flag, regenerate their color-key region,
  and expand the lyric child by the popup size delta.

The legacy reader in the executable accepts several defects found in the
historical skin collection.  The rebuild now normalizes only markup positions
outside quoted values, so URL query strings are not damaged.  It also accepts
literal ampersands in old metadata URLs, a trailing line after `</skin>`, a
stray quote after an unquoted scalar, and an immediately duplicated attribute.
The regression pass currently opens all 172 ZIP-based `.skn` packages in the
workspace.  The one RAR-based package belongs to the still-separate archive
backend.

## HWND and command reconstruction

The popup is created as an owned, captionless `WS_POPUP` with
`WS_EX_TOOLWINDOW` and optional `WS_EX_TOPMOST`.  Its observable title is
`Lyric`.  `0044AD55` materializes the same child set even when a skin supplies
no image or a zero rectangle:

| Child | Command/ID | Behaviour |
| --- | ---: | --- |
| `SkinButton` | `0x8039` | desktop-lyric command |
| `SkinButton` | `0x8033` | lyric top-most toggle |
| `LyricCtrl` | object-local ID | timed lyric paint and input |
| `SkinButton` | `0x7D64` | hide normal lyric window |
| hidden `SkinButton` | `0x82DC` | framework base control at `-16,-16,0,0` |

Skin buttons use four-state bitmap frames and background-patch composition.
As in `0040A5E3`, entering a button gives that child HWND mouse capture and
selects its hot frame; a captured move outside releases capture and restores
the normal/checked frame, unless a left-button press is still active.  Button
up activates only when the original pressed control still contains the point,
and capture loss cancels the press.  The controls use the shared
resource-backed single-relay tooltip path.  `0x7D64` hides instead of
destroying the popup, while `LyricWnd`, `LyricVisible`, and `LyricTopMost` are
restored and saved in `TTPlayer.xml`.

The right-click popup comes from menu resource `0x8F` in the EXE-local
`ttpres.dll`.  Reload (`0x8024`), associate (`0x802D`), unassociate (`0x802E`),
copy (`0x8034`), download (`0x802F`), top-most (`0x8033`), and desktop lyrics
(`0x8039`) retain their original IDs so the common owner-draw menu code and
localized strings are reused.

## Lyric control timing and input

- `00442576 -> 00441C7E` paints rows around the current timestamp with the
  package font/colors, `TextAlign`, `RowInterval`, smooth between-line motion,
  and the highlighted current row.  This `LyricCtrl` paint chain does not read
  the skin's two `mini_border` colors.
- Local LRC loading supports UTF-8, UTF-8 BOM, UTF-16 LE/BE BOM, and the
  ANSI/GBK fallback used by 5.7-era lyric collections.  Adjacent `.lrc`/`.txt`
  files and EXE-local `Lyrics` candidates are searched without using a current
  working-directory fallback.
- `004425F7`, `00442671`, `0043FA1D`, and `0043EF4D` implement captured row
  dragging.  Release converts the dragged pixel phase to an interpolated
  timestamp between the selected line and its successor; the last line uses
  the original synthetic `+60000 ms` endpoint.  Escape/capture loss cancels.
- Arrow keys seek to the previous/next timed line.
- `00442A97 -> 0043E92C -> 0043D7D0` makes the wheel adjust every lyric
  timestamp by 500 ms when `MouseWheelAdjust` is enabled.  It does **not** seek
  the audio engine: wheel-up adds `-500 ms`, wheel-down adds `+500 ms`.

## Runtime comparison

An isolated LX-iPlay run of the original and rebuild produced the same normal
lyric popup geometry and window contract:

| Property | Original | Rebuild |
| --- | --- | --- |
| class | `TTPlayer_LyricWnd` | `TTPlayer_LyricWnd` |
| rectangle | `154,740,654,810` | `154,740,654,810` |
| style / ex-style | `0x94000000 / 0x80` | `0x94000000 / 0x80` |
| owner | `TTPlayer_PlayerWnd` | `TTPlayer_PlayerWnd` |
| `LyricCtrl` rectangle | `35,5,465,65` | `35,5,465,65` |
| zero-size buttons | `0x8039`, `0x8033`, `0x7D64` | same |
| hidden base button | `0x82DC @ -16,-16,0,0` | same |

The corresponding child styles also match: `0x8039` is `0x54000000`,
`0x8033`/`0x7D64`/`0x82DC` are `0x54010000`, and `LyricCtrl` is
`0x50010000`, all with zero extended style.

The native `LyricCtrl` ID is an object-address-derived value and therefore
changes between processes; the rebuild uses a stable private ID.  This has no
command or visual meaning.

A physical-pointer comparison over the visible lyric `SkinButton(0x8039)`
gives that same child capture in both binaries, displays an 85x22 tooltip, and
leaves neither capture nor a visible tooltip after the pointer exits.

## `mini_border` ownership and the LX-iPlay black frame

`CSkinParser_ParseLyricWindow` (`004A88D0`) initializes skin offsets `+0xA2C`
and `+0xA30` to the `0xFF000000` sentinel, then stores
`mini_border.left_top_color` and `right_bottom_color` when present.  Searching
all accesses in the decompiled executable gives one consumer only:
`FUN_00449313`, the lyric **popup-window** background painter.

That function has two distinct branches:

1. With `DAT_0054775C == 0` (normal player mode), it calls `FUN_0044E8CC` to
   paint the normal lyric-window skin and never reads `+0xA2C/+0xA30`.
2. With `DAT_0054775C != 0` (mini-player mode), it fills the lyric popup and
   calls `FUN_00413320` to draw a one-pixel client-edge frame.  Only this branch
   substitutes the package's `mini_border` colors; missing colors fall back to
   the active lyric text color.

LX-iPlay declares both mini colors as `#1e1e1e`, while its normal `LyricCtrl`
occupies `35,5,465,65`.  The previous rebuild drew those colors unconditionally
around that child, producing the reported black `430 x 60` rectangle.  The
border is now emitted by the popup painter only for mini mode, after its solid
background fill; normal `LyricCtrl` painting ends after text, drag-guide, and
`FadeIndex` processing, matching the native call graph.

The physical regression probe captures the original and rebuild
`TTPlayer_LyricWnd` with the same `500 x 70` popup and `35,5,465,65` child.  The
pre-fix child edge is `#1E1E1E`; after the fix every sampled child-edge pixel is
the active lyric background and no rectangular outline remains.  Captures are
stored under `rebuild/build/comparison/lx-lyric-progress-fade-30s/` as
`original-lyric-window.png`, `before-fix-rebuild-lyric-window.png`, and
`after-fix-rebuild-lyric-window.png`.

## Mini-player lyric state and layout

`FUN_00464B6C` does not destroy or hide the lyric popup unconditionally when
entering mini mode. It keeps the same `TTPlayer_LyricWnd` and switches between
two independent settings groups:

| Normal player | Mini player |
| --- | --- |
| `LyricWnd` | `LyricWnd2` |
| `LyricVisible` | `LyricVisible2` |
| `LyricTopMost` | `LyricTopMost2` |

On first entry, an empty `LyricWnd2` is created immediately to the right of the
mini player with a width of 200 pixels and exactly the mini-player height.
`FUN_004495C8` moves the three normal lyric buttons to `(-1000,-1000)`, then
lays out `LyricCtrl` at `(2,2)-(width-4,height-2)`. `FUN_00449577` removes the
normal shaped region. `FUN_004495A0` reduces every resize hit except the right
edge (`0x10`) to move, so the mini lyric is horizontally resizable but retains
the mini-player height.

`tools/probe_mini_mode.ps1` exercises the physical double-click path in an
isolated runtime. Original/rebuild LX-iPlay probes both produce normal player
`100,100,600,450`, normal lyric `100,500,600,570`, mini player
`700,100,900,133`, and mini lyric `700,150,900,183`. Normal-visible/mini-hidden
and normal-hidden/mini-visible round trips save `1/0` and `0/1` respectively.
With an empty mini lyric rectangle, both materialize `900,100,1100,133`.
TT2012 forces the saved mini lyric height from 33 to its 25-pixel mini-player
height. A `005 dudu.skn` double-click leaves both windows unchanged because the
skin does not have a loaded mini background.

## In-window lyric editor and right-click menu

The edit path follows `0044B066`, `00446ED7`, `0044CB58`, `0044D646` and
`0044DAE2` through `0044DDFF`.  Display mode loads menu resource `0x8F`; command
`0x802C` replaces the visible `LyricCtrl` with a `RichEdit20W` and retains a
resource-backed `WTL_ToolBar`.  Editor mode then loads menu `0x94`, including
timestamp insert/replace/delete, 500 ms earlier/later, expand/compress,
selection-only simplified/traditional conversion, cut/copy/paste/undo/redo,
save/save-as and command `0x8020` to return to display mode.  Toolbar-only
commands `0xE124/0xE129` open the RichEdit find/replace paths.

The `0x8030/0x8031/0x8032` embedded-lyric submenu is routed through the
recovered sound-reader metadata ABI using the add-ins' `Lyrics` field. Reading
rebuilds the active lyric model/editor, writing stores the canonical editor
text, and deleting writes the empty metadata value. Unsupported readers use
the original `0x817E/0x817F/0x8180` resource error paths rather than silently
accepting the command; an embedded editor also uses the original `0x817C`
save prompt instead of the ordinary file prompt `0x817D`.

The toolbar is decoded from RT_TOOLBAR `0x94` and bitmap `0x94` in
`ttpres.dll`; its 16x16 images use `ILC_COLOR32 | ILC_MASK` and the original
`#C0C0C0` mask.  The custom
time command `0x802B` also instantiates the original RT_DIALOG `0xCF`, with
signed `-10000..10000 ms` input and 500 ms acceleration.  `004431A0 ->
0043E237` streams an ordinary local LRC into RichEdit verbatim; no synthetic
`ti/ar/al/by` rows are inserted.  A non-zero `[offset:]` instead takes the
`0043F319` model-serialization branch, folds the offset into every timestamp,
removes the offset tag and marks the editor modified.  `0043E3CF` writes ACP
when every character can be represented and otherwise writes UTF-8 with a
BOM; it does not preserve the source encoding. Returning from the editor
destroys only `RichEdit20W`, hides and retains the toolbar, restores
`LyricCtrl`, and reloads the same source file.

### RichEdit/TOM contract and protected syntax ranges

The editor is initialized before its first `EM_STREAMIN`, following
`00443064 -> 00443B37` rather than relying on RichEdit defaults:

1. `EM_GETOLEINTERFACE` and `QueryInterface(ITextDocument)` acquire TOM.
2. An outer `tomSuspend` covers URL detection, `TM_MULTILEVELUNDO`, event mask
   `0x04280001`, four-pixel left/right margins, and 32 tab stops of `0x168`.
3. `00443B37` calls `004439E6`, which adds its own nested
   `tomSuspend/tomResume` around `EM_SETCHARFORMAT(SCF_DEFAULT)`.
4. The default `CHARFORMAT2W` has size `0x74`, mask `0xE8000011`, effects
   `0x04000010`, minimum height 180 twips, explicit text color/charset/face,
   cleared bold, automatic background and protected text.
5. Input/output use `EM_STREAMIN/EM_STREAMOUT` with
   `SF_TEXT | SF_UNICODE`; callbacks truncate a requested byte count to an even
   UTF-16 boundary. Formatting remains outside the user's undo history.

`00443C70 -> 00443CEF` does not parse timestamps. It scans sequentially with
`EM_FINDTEXTW(FR_DOWN)`, paints every complete `[...]` range with the tag
color and all remaining ranges with the body color. Each selection format is
performed under its own TOM suspension, while `WM_SETREDRAW(FALSE)` and
selection restoration keep the caret and scroll position stable. An unmatched
`[` is therefore ordinary body text, exactly as in the executable.

Protected formatting is intentionally paired with `ENM_PROTECTED`. The
`EN_PROTECTED` handler records seven edit actions (typing/replacement,
Delete, Backspace, cut, paste, and cut-followed-by-paste) but returns zero so
the edit proceeds. `EN_CHANGE` then reformats only the affected whole-line
range computed through `EM_EXLINEFROMCHAR`, `EM_LINEINDEX`, and
`EM_LINELENGTH`; unknown keyboard edits fall back to a full pass. This retains
live tag coloring without adding formatting operations to Undo/Redo.

After focus moves to the editor, `0044CB58` creates the five-entry accelerator
table `Ctrl+X/C/V/Z/Y -> 0xE123/0xE122/0xE125/0xE12B/0xE12C`.
`00449DA8` calls `TranslateAcceleratorW` only while `RichEdit20W` owns focus,
and `0044D646` destroys the table before destroying the editor. The modeless
Find/Replace dialog retains first refusal through `IsDialogMessageW`.

The three time-tag commands now keep RichEdit character positions throughout.
`00443460` inserts at the current line start; replacement scans every valid
time tag on that line and chooses the one with the smallest absolute distance
from playback time (first tag wins a tie). `004436B8` instead removes the next
complete bracket pair after the selection, without requiring a valid time.
Both insert and replace then run `00443789`, replacing the current line break
with CRLF and advancing the caret; at the last line RichEdit's `cpMax=-1`
sentinel appends the CRLF. Although menu command `0x804F` toggles the displayed
“修改标签后换行” check and its saved setting, 5.7.9 has no behavioural read of
that value: both native command paths advance unconditionally. Per-tag
replacement remains separately undoable, including the native no-visible-text
first Undo used for the CRLF replacement.

The supplied `陈慧娴 - 千千阙歌.flac/.lrc` pair was also compared in an
isolated default-skin run (`PackageName="<Default_Skin>"`). The original and
rebuild both produce a 486x24 toolbar and a 486x414 RichEdit outer rectangle
(469x397 client), toolbar style `0x56018945`, RichEdit style `0x563081C4`,
body/tag colors `#548EA5/#D4F5FF`, background `#18333C`, SimSun 180-twip
text, GB2312 charset, and realized pitch/family 9. Removing either bracket
recolors the same cross-line ranges, and one Undo reverses only the user edit;
the syntax pass creates no extra undo record.

An isolated LX-iPlay window probe gives the following exact lifecycle for both
the supplied original executable and the rebuild:

| State | Child contract |
| --- | --- |
| editing | visible `WTL_ToolBar`, ID `0xE800`, style `0x56018945`; visible `RichEdit20W`, ID `0`, style `0x563081C4`; hidden `LyricCtrl`; disabled `0x8039` desktop-lyric button |
| returned | hidden retained toolbar, style `0x46018945`; destroyed editor; visible `LyricCtrl`; re-enabled desktop-lyric button |

For the `500x70` LX-iPlay lyric popup the toolbar rectangle is
`135,505,565,529` and the editor rectangle is `135,529,565,565` in the probe's
screen coordinates in both binaries.  The save regression shifts
`[00:00.00]`/`[00:05.00]` with command `0x804C` to
`[00:00.50]`/`[00:05.50]` before returning. With
`陈慧娴 - 千千阙歌.flac/.lrc`, original and rebuild now expose identical
1,523-character initial editor text (beginning directly with the timed
`作词` row), identical ACP-936 save text, and identical rounded results for
all three-digit timestamps after a 500 ms shift. Expand/compact commands also
match the original four-row metadata normalization, cross-row grouping and
timestamp order.

## Scope boundary

The original desktop overlay is already visible in runtime enumeration as
three independently owned popup classes and has its own bar skin, layered
painting, lock/hover policy, karaoke fill, settings profiles, and command
state.  The lyric window's `0x8039` command is retained, but the rebuild
does not pretend that redirecting it to `TTPlayer_LyricWnd` would reproduce
that subsystem.  Desktop rendering and the retired online download provider
remain explicit follow-up targets.
