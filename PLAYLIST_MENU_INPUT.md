# Playlist menu and input recovery

This note records behavior recovered from `TTPlayer.exe.pseudo.c`, the original
`ttpres.dll`, and physical input probes against the supplied original binary.
It describes observable compatibility; decompilation cannot establish literal
source-code or binary identity with the lost private classes.

## Resource ownership

- `FUN_00482BAF` creates the playlist toolbar and calls
  `FUN_0048AA1F((LPCWSTR)0x8B)`.
- `FUN_0048AA1F` calls `LoadMenuW(DAT_0054605C, ...)`, so menu 139 (`0x8B`)
  and its labels belong to `ttpres.dll`, not to C++ string literals.
- Menu 139 contains the seven categories Add, Delete, List, Sort, Find, Edit,
  and Mode. Their first command IDs are `0x7F09`, `0x7F13`, `0x7F01`,
  `0x7F1D`, `0x7F3B`, `0x7F30`, and `0x7F45`.
- `FUN_00488FEF` loads menu 152 (`0x98`) for one selected track, menu 153
  (`0x99`) for multiple selected tracks, and converts the seven menu-139
  category popups into the blank-area context menu. Playlist-title context
  menu 156 (`0x9C`) is loaded separately.
- Runtime title, status, channel, file-filter, default-list, error, toolbar,
  context-menu and metadata text is now obtained from the EXE-local
  `ttpres.dll`. An audit of `rebuild/src` and `rebuild/include` finds no Han
  characters in C++ string literals.
- The three startup failure messages are intentional exceptions, not missing
  resource lookups: `004C0E8F` contains the literal ttpcomm and sound-library
  messages and `004C01CD` contains the literal ttpres message. The sound-error
  caption is resource string `0x80`; the rebuild snapshots it before unloading
  `ttpres.dll`, matching the lifetime of `DAT_005474FC`. Decoder diagnostics
  remain internal and the visible ordinary-open error uses resource `0x828E`.

## Dynamic single-track menu

`FUN_00488FEF` does not display menu 152 unchanged. For a local file it removes
the network-report command `0x80D1`, the adjacent separator, download command
`0x7FEB`, and (unless applicable to a CD item) freedb command `0x7EFA`. For a
URL it removes the two local rename/send-to entries by position and command
`0x7EFC` (Browse File). The rebuild follows those same command/position edits.

Physical right-click probes on LX-iPlay now give identical live menu handles:

| Target | Original | Rebuild |
| --- | ---: | ---: |
| one local track | 19 items, first `0x7EF5` | 19 items, first `0x7EF5` |
| two selected tracks | 15 items, first `0x7EF6` | 15 items, first `0x7EF6` |
| blank list area | 7 category popups | 7 category popups |
| playlist title | 12 items, first `0x7F05` | 12 items, first `0x7F05` |

The complete command-ID sequence was compared for each case, not only the
visible first label.

## Playlist catalogue create and rename

The catalogue commands use native in-place label editing rather than a prompt
dialog. `FUN_0048468A` formats resource string `0x8192` (`新列表%d`), creates
and activates the last list, focuses catalogue `ListCtrl` 10241, then calls
`FUN_0044DEF5(index)`. That wrapper sends `LVM_EDITLABELW` (`0x1076`). The
right-click command `0x7F04` reaches `FUN_0048516E`, obtains the selected
catalogue row with `LVM_GETNEXTITEM`, and enters the same edit path.

`FUN_00489B06` is the corresponding `LVN_ENDLABELEDITW` handler. It updates a
title only when `LVITEM.mask & LVIF_TEXT`, `pszText` is non-null and the first
character is non-zero. Physical probes resolve the remaining observable
lifecycle: Enter commits, while Escape, an empty Enter, and loss of focus all
retain the previous title. Whitespace is not trimmed. A committed title marks
that catalogue entry's numbered TTBL dirty even when another playlist is
active.

Original/rebuild probes now match for creation and for actual right-click ->
`重命名(&R)` input. The temporary editor is class `Edit`, style `0x54000080`,
extended style zero, and 16 pixels high; its measured rectangle and initial
text match in LX-iPlay, TT2012 and Let's Vista. For example, LX-iPlay at a
playlist origin of `(100,400)` produces `[默认]` at
`(115,400)-(165,416)` and a third newly-created `新列表3` at
`(115,432)-(171,448)`. Entering `Probe title` produces the same title in the
same `%04d.ttbl` in both programs. The reusable cases are `RenameCommit`,
`RenameEmpty`, `RenameEscape`, `RenameBlur`, `RightClickRename` and
`NewCommit` in `rebuild/tools/probe_playlist_context.ps1`.

## Toolbar geometry

The XML `toolbar/position` rectangle is the input hit rectangle. It must not be
silently replaced by the bitmap dimensions: `173 Keenwood` uses a 222-pixel
XML rectangle with a 236-pixel bitmap, and the original's seventh-button hit
region follows the XML geometry.

`FUN_00482BAF` initially creates the native toolbar with a 30-pixel (`0x1E`)
row. The only supplied tall-toolbar skin, `一听音乐.skn`, uses the lower part
of its 47-pixel toolbar rectangle for the fifth quick-find control; the other
six menu buttons remain in the upper row. The rebuilt hit test preserves that
split. Physical probes read the same seven popup counts and first command IDs
for the DLL/resource behavior and all 23 external skins. The comparison tools
are `rebuild/tools/probe_playlist_toolbar.ps1` and
`rebuild/tools/compare_playlist_toolbar_skins.ps1`.

The toolbar bitmap is a color-keyed image-list source, not an opaque strip.
Using `SRCCOPY` exposed the common `#ff00ff` mask as purple controls. The
rebuild now uses the package transparent color and the native toolbar's
integer frame/cell truncation: 236-pixel strips use seven 33-pixel frames,
while already composed equal-width strips remain intact. Original/rebuild
base toolbar crops are pixel-identical for all 23 supplied skins and all 23
rebuild captures contain zero pure-magenta pixels. This includes the distinct
geometry cases in Media Player 10, AIPOTU, TT-07, PurpleMyth, Qingping,
173 Keenwood and XPMC.

The native drop-down toolbar raises `TBN_DROPDOWN` on button-down. The rebuild
therefore opens the corresponding `0x8B` popup at that point instead of
holding a synthetic pressed state until button-up. Media Player 10 base,
hover and popup-open button crops are each pixel-identical to the original.

## Left catalogue control and selection

`FUN_00482BAF` does not paint the left catalogue as labels owned by the popup.
It creates a hidden `TreeCtrl(0x2800)`, visible `ListCtrl(0x2801)` named
`PlayLists`, and visible `ListCtrl(0x2802)` named `Files`.  The rebuild now
uses the same class names, IDs and styles (`0x40018413`, `0x50015205`, and
`0x50015001`), sends the original Unicode/list extended-style messages, and
keeps the dormant tree at its fixed 200x30 creation rectangle.

The catalogue draw path follows `FUN_004897C1`.  Its active playlist and
native focus selection are distinct states.  Without `LVS_SHOWSELALWAYS`, the
selected background/focus frame disappears when `PlayLists` loses focus,
while the active title retains `Color_HighLight`.  Focused rows use the skin
selected bitmap or the recovered `Color_Select` to `Color_Bkgnd` vertical
gradient; selected text uses `COLOR_HIGHLIGHTTEXT` with the original color
collision fallback.  Alternating backgrounds, two-pixel horizontal text
inset and `DT_SINGLELINE | DT_NOPREFIX` are retained.

With both runtimes normalized to a 500x255 playlist and `SplitOnLists=55`,
the child contracts match exactly: hidden tree `11,48,211,78`, catalogue
`11,48,66,243`, and files `71,48,489,243`.  A physical click on catalogue row
one produced the same focused selection in both captures; the 72-pixel-wide
left crop changed in 509 of 18,360 pixels, with maximum summed RGB delta 12
and mean delta 0.2276 (font/edge rasterisation only).

## Selection and drag

The original track surface is `ListCtrl` ID `0x2802`. Its notification
`LVN_BEGINDRAG` (`-109`) enters `FUN_004894AF`, builds a selected-row drag
image/data object, calls `DoDragDrop`, and sends delete-selected command
`0x7F13` when a move completes. Right-click keeps an existing multi-selection;
Ctrl and Shift retain native multi-select semantics, while a plain click on an
already selected row collapses the selection only after a non-drag release.

The rebuild implements the same observable internal move behavior with a drag
threshold, insertion line, edge auto-scroll, selected-row block reorder, and
playing-item remapping. A physical row-0-to-after-row-1 drag was executed in
both an embedded LX-iPlay list and a separate TT2012 playlist window. After
orderly shutdown, both original and rebuilt TTBL files contained this order:

1. `INMU KING.wav`
2. `A.wav`

The physical probe and TTBL parser are available through
`rebuild/tools/probe_playlist_context.ps1 -Target Drag`.
`-Target DragMultiple` also Ctrl-selects two rows before beginning the physical
drag; the supplied three-row probe preserves the same selection/order in both
programs for a non-accepting block-drop target.

## File selection and external OLE intake

External intake now follows the separate `0048059D` OpenFile path and the
`0045A8BE`/`004822AD` OLE-drop path rather than translating either into the
playlist's internal selected-row drag.  The dialog preserves the original
`0x00C81224` flags, `0xFFFC`-character Explorer multi-select buffer and
directory-plus-leaf parsing. Main-player use replaces and plays; playlist
command `0x7F09` appends and starts only when both original idle checks return
`-1`.

The rebuilt top-level player, playlist and lyric windows register real OLE
`IDropTarget` objects and consume `CF_HDROP`; the equalizer deliberately does
not. Track-pane drops insert at the indicated row, catalogue drops target or
create lists, and `0044AC70` limits lyric intake to the first dropped item.
Legacy `.ttpl` and other explicitly opened non-TTBL/non-M3U XML playlists now
follow `00475219`/`004749A7`, including relative paths and tag metadata.

The expanded isolated original/rebuild comparison uses a real `DoDragDrop`
source. Base report `20260904-212933` matches 9 scenarios/18 runs, including
single/multi main and playlist intake, ZIP, lyric-first-item and equalizer
rejection; Dialog report `20260904-213147` matches 4 scenarios/8 real common
file-dialog runs. Both reports have `Success=true`. Native report
`20260904-210505` records exit code 0 without timeout for `ttplayer_tests`,
`audio_recovery_tests`, `equalizer_recovery_tests` and `ttpcomm_api_test`.
Complete Extended report `20260904-222201` matches 17 scenarios/34
original-and-rebuild executions with `Success=true`.
Current Base report `20260904-225446` matches all 9 scenarios/18 runs after
removing the command-line-playback startup race. Current native report
`20260904-225758` has `Success=true`; `ttplayer_tests`,
`audio_recovery_tests`, `equalizer_recovery_tests` and `ttpcomm_api_test` all
exit 0 without timeout.

Focused Extended reports `20260904-220956`, `20260904-221045`,
`20260904-221519` and `20260904-221556` match embedded-CUE persistence,
blank-catalogue multi-playlist insertion, and row/blank `WAV/M3U/WAV`
order plus catalogue/track selection. They exposed and corrected a stale
matrix oracle before the complete Extended rerun succeeded.

The probe now emits atomic heartbeat checkpoints throughout staging, every
runtime/case, native tests and cleanup. A 60-second stale heartbeat aborts the
run; cross-process messages, OLE/helper work, dialogs and shutdown all have
finite deadlines. The 173-skin native scan receives 180 seconds (its clean-VM
runtime is about 67 seconds), and per-case runtime directories are validated
and deleted after evidence capture. This removes both sources of the reported
apparent hang: a false 60-second native-test timeout and nearly 1 GB of retained
VM-local copies after repeated matrices. The complete evidence map and scope
boundary are documented in `FILE_INTAKE_RECOVERY.md`; harness details are in
`tools/windows_sandbox/README.md`. This remains an observable-compatibility
recovery, not a claim that the lost private classes were reproduced word for
word.

The rebuilt UI also avoids two genuine unbounded waits encountered by these
paths. Audio reader open/stop calls now have finite UI budgets while retaining
the worker and plug-in DLL lifetime, and skin catalogue publication is a
40-ms-at-most readiness check with cooperative cancellation rather than a
blocking `future::get`.

The legacy tag-editor/service dialogs remain separate compatibility targets.
