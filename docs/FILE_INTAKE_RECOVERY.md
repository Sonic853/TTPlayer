# File intake recovery

This note records the recovered file-selection, path-classification and OLE
drop behavior of the player, playlist and lyric windows.  The evidence comes
from `TTPlayer.exe.pseudo.c`, the rebuilt implementation and an isolated
Windows Sandbox comparison with the supplied original executable.

The compatibility claim is deliberately limited to observable behavior.  The
lost private C++ classes and their original source text cannot be recovered
word for word from a decompiler, and the rebuilt classes are not claimed to be
binary- or source-identical replacements.

## Recovered call map

| Address | Evidence in `TTPlayer.exe.pseudo.c` | Observable contract |
| --- | --- | --- |
| `0048059D` | Builds the audio/archive/all-files filter, allocates a `0xFFFC`-wide-character result buffer and separates the single- and multi-select layouts | Shared audio `OpenFile` operation |
| `004739DB` | Resolves `.lnk` targets, checks filesystem attributes and returns a type for ordinary files, registered audio, CUE, ZIP/RAR, playlists or directories | Common path classifier |
| `004742DD` | Dispatches classifier results to ordinary-file creation, CUE expansion, archive expansion, playlist loading or recursive directory intake | One-path import dispatcher |
| `0047444A` | Enumerates `directory\*.*`, descends into subdirectories when enabled and sends acceptable children back through `004742DD` | Recursive folder import |
| `0045A8BE` | Obtains `CF_HDROP`, treats a first `.skn` item as skin installation, otherwise clears/imports/selects/starts playback | Main-player OLE drop |
| `004822AD` | Materializes all dropped paths, hit-tests the playlist track and catalogue controls, inserts or routes the result, and reports effect `4` | Playlist-window OLE drop |
| `0044AC70` | Requests clipboard format `15`, reads only `DragQueryFileW(..., 0, ...)`, passes that path to the lyric loader and reports effect `4` | Lyric-window OLE drop |

Clipboard format `15` is `CF_HDROP`; effect `4` is `DROPEFFECT_LINK`.
`00481F2A`, called by `004822AD`, first tries the original same-process custom
format and then falls back to `CF_HDROP`.  Its `0xFFFFFFFF` count query and
indexed `DragQueryFileW` loop prove that the external drop path preserves all
items in their `HDROP` order.  It also has a `UniformResourceLocatorW`
fallback.

No equivalent equalizer drop entry point is present in the recovered dispatch.
The rebuilt drop-surface enum consequently contains only player, playlist and
lyric, and no `IDropTarget` is registered on the equalizer window.  Dropping a
file over the equalizer is intentionally rejected rather than forwarded to the
main player.

## Path classification and expansion

`004739DB` returns the following effective categories:

| Value | Recovered meaning | `004742DD` action |
| ---: | --- | --- |
| `0` | explicit path whose attributes cannot be read | create one ordinary playlist item |
| `1` | existing ordinary file | create one ordinary playlist item |
| `2` | registered audio extension | create one playlist item |
| `3` | CUE sheet | expand its audio sub-tracks |
| `4` | ZIP or RAR archive | expand supported children |
| `5` | playlist file | load it and merge its items |
| `6` | directory | enumerate through `0047444A` |

A shortcut is resolved before the final classification, so a `.lnk` can turn
into either a file or a directory. A failed shortcut resolution retains the
`.lnk` as the ordinary input, while a successfully resolved but now-missing
target follows type 0. Direct explicit paths likewise remain rows when their
attributes cannot be read. Recursive directory intake is narrower: it admits
only supported audio, CUE and archive inputs, skips nested playlist files, and
uses `Histroy/CheckSubFolder` to decide whether to descend. The rebuild keeps
this distinction in `CollectImportedTracks` and adds only an ancestry check to
prevent a reparse-point directory cycle from recursing forever.

### Folder intake follow-up (2026-10-08)

The folder shortcut rule needs one further distinction. In `0047444A`, the
type-6 result for a shortcut discovered during enumeration does **not** call
the dispatcher or recurse. This was verified against the original machine
code at `004745B3` through `00474652`, not inferred from the decompiler's
unused string comparisons. An explicitly supplied folder shortcut still
enters directory intake. The rebuild now preserves this distinction, including
when `CheckSubFolder` is disabled: a discovered `.lnk` cannot bypass the
subdirectory setting. Audio-file shortcuts remain eligible.

Menu command `0x7F0A` follows `004851A1 -> 00480C91 -> 0047444A -> 00480386`:

- Leave fullscreen before opening the folder picker (`0044AB8F`).
- Restore the last folder and `Histroy/CheckSubFolder` state. Resource `0x8152`
  is `请选择一个文件夹:`; resource `0x8153` is `包括子目录`.
- Show the subdirectory checkbox inside the picker. Modern Windows retains
  `IFileDialogCustomize`; the XP fallback uses a WTL `CFolderDialogImpl` with
  a real checkbox aligned to the left of OK, matching `004367E3/004368E6`.
  Its font follows OK, it accepts keyboard focus, and its rectangle is updated
  after dialog resizing. There is no second yes/no question after selection.
- Keep edits local to the dialog until a valid filesystem folder is accepted.
  Cancel does not change history, recursion state, or playlist contents.
- Save the accepted checkbox value **before** enumerating the selected folder.
- Append the collected batch to the active list; select the imported range.
  Empty append remains a no-op. Ordinary repeated imports retain duplicate rows.
- Keep the existing idle-play gate: both the destination playing marker and
  the open-source state must be absent before auto-start is allowed.

Directory enumeration is depth first in `FindFirstFileW/FindNextFileW` order;
this path does not add a name sort or a hidden/system-file exclusion. Nested
playlist files are skipped; CUE and supported ZIP/RAR members use their existing
expansion paths. The original scans directories synchronously, then optionally
pre-reads information (1–5 entries inline, 6+ on one worker thread). The rebuild
retains its existing asynchronous metadata queue and directory ancestry guard.

Folder drops continue to share this setting and classification. Track-pane
drops insert at the hit row; a catalogue-row drop appends to that list; blank
catalogue space creates one list from the ordinary batch. `00481F2A` overwrites
the temporary title for each folder, so the last folder names a multi-folder
batch. The blank-catalogue branch does not select the new track rows.

Validation for this follow-up:

- Universal Release build and XP/Win7 static-import audit passed.
- The local regression executable passed **83 checks on Windows 11, Windows 7,
  and Windows XP**, including the classic and OS-selected folder dialogs,
  checkbox initial values/toggling, OK/cancel, checkbox geometry/font/resizing,
  fullscreen exit, history preservation, recursion on/off, explicit/discovered
  folder shortcuts, real directories named with `.lnk`, audio/missing shortcuts,
  nested playlists, CUE, ZIP, empty
  and repeated imports, stopped playing-marker behavior, imported selection,
  and actual `CF_HDROP` dispatch to the three playlist destinations.
- Windows 11 provides the previously agreed substitute coverage for Windows
  10; this is not a claim of execution on an actual Windows 10 installation.
- Test source and fixtures stay under `rebuild/tests/folder_intake`; they are
  local only. No workflow was changed, and `BUILD_TESTING` remains disabled in
  the Release build.
- The resulting universal package is
  `rebuild/build/Release/TTPlayerRebuild-2026.10.08p4.zip`. It contains the
  player, updater, required rebuilt `ttpcomm.dll`, verified HTTPS component,
  and `SHA256SUMS.txt`; no test executables are included. Existing manual
  replacement/update behavior is unchanged.

## Open-file dialog and multi-select layout

`0048059D` composes its filter from resource-backed audio descriptions, the
registered reader/decoder patterns, the ZIP/RAR entry and the all-files entry.
Formats therefore appear only after the corresponding built-in or add-in
reader has successfully registered; the dialog does not manufacture codec
support from a static extension list.

The recovered final `OPENFILENAMEW::Flags` value is `0x00C81224`:

- `OFN_HIDEREADONLY`
- `OFN_ALLOWMULTISELECT`
- `OFN_FILEMUSTEXIST`
- `OFN_ENABLEHOOK`
- `OFN_ENABLEINCLUDENOTIFY`
- `OFN_EXPLORER`
- `OFN_ENABLESIZING`

The result buffer contains up to `0xFFFC` wide characters.  A single selection
is one full path followed by the terminating empty string.  Explorer-style
multi-selection is a directory followed by each selected leaf name and a
second null terminator.  The native loop joins relative leaf names to that
first directory, while already absolute/URL-like values are retained.  The
rebuilt parser follows the same layout and preserves selection order. It also
persists the recovered `Histroy/SoundPath` spelling: a single choice stores the
complete selected path, while Explorer multi-select stores the leading
directory.

The two visible command contexts intentionally differ:

- Main command `0xE101` reaches wrapper `00464A2E`, exits full screen, runs
  `0048059D`, replaces the active list, selects the first imported item and
  starts it.
- Playlist command `0x7F09` calls `0048059D` as an append operation.  The new
  block is selected.  It starts playback only when both the list's current
  item and the player query `SendMessageW(..., 0x7F3, 0, 0)` are `-1`; this is
  the condition visible at pseudo lines `123936-123945`.

The rebuilt implementation centralizes the mutation in
`CommitImportedTracks`: replacement stops the old source and clears the
destination first, while insertion shifts the stored playing index when the
new block is inserted before it.  This avoids accidentally changing the
currently playing song during a playlist-only insert.

## Window-specific OLE behavior

The native windows consume an `IDataObject`; this is not a `WM_DROPFILES`
shortcut.  The rebuild registers real `IDropTarget` implementations and calls
`IDataObject::GetData` for `CF_HDROP` (with the native Unicode URL fallback on
the playlist target), so item ordering and the negotiated drop effect remain
observable to the source.

| Surface | Recovered behavior |
| --- | --- |
| Main player | A first `.skn` installs/switches that skin. Otherwise all dropped paths replace the active playlist and the first imported item starts playing. |
| Playlist track pane | All paths are inserted at the hit/insertion row. The active playing row is remapped if insertion occurs before it. An idle target with no playing marker can auto-start the first inserted item; see the 2026-10-09 correction below. |
| Playlist catalogue row | Ordinary files append to the hit playlist with the same conditional auto-start. Dropping on catalogue whitespace creates a new list without auto-start; a dropped playlist file is added as its own list. |
| Lyric window | Only `HDROP` item zero is inspected, matching `0044AC70`; the lyric loader then validates/loads that first path. |
| Equalizer | No target is registered and the drop is rejected. |

The playlist target maintains an insertion cue while OLE is over the track
pane and a target-list cue while it is over the catalogue.  These are visual
reconstructions of the native hit-test contract, not evidence that the lost
private drawing implementation had the same class layout.

### Drop argument audit and completed follow-up (2026-10-09)

The original x86 argument audit corrected the earlier blanket statement that
a playlist-window drop cannot start playback. The two differences identified
by that audit are now implemented. The original calls are:

| Native call site | Arguments to `00480386` |
| --- | --- |
| `00482470..00482479`, existing catalogue row | `insertion = destination.count`, `read_info = 1`, `allow_idle_play = 1` |
| `00482612..0048261B`, track pane | `insertion = hit row / count`, `read_info = 1`, `allow_idle_play = 1` |
| `00482482..004824BA`, blank catalogue space | Appends the temporary list through `0047928F` and activates it through `0047F294`; does not call `00480386` |

Both ordinary insert/append calls push `1, 1, insertion` in x86 right-to-left
argument order. `004804AD` tests the third argument, then checks the current
list's `+0x1C == -1` and main-window message `0x7F3 == -1`, before calling
`0047FEA3(insertion, 0)`. Stopped playback can retain a playing marker, so
"stopped" and "eligible to auto-start" are different states. The rebuild now
uses `ImportPlayback::if_idle` for these two drop paths. The blank-catalogue
new-list path retains `none` and does not select the new list's tracks.

The 2026-10-08 regression run passed its 83 assertions, but the drop cases
reused a target carrying `PlayingRow(0)`. They therefore covered preserving
stopped playback, not the missing idle/no-marker auto-start case. The previous
result is not evidence that all drop playback states match the original.

There is also an empty-batch ordering edge case. For blank catalogue space,
the original only changes the signed insertion cursor from `-1` to the old
catalogue count when the temporary **track batch is nonempty**. With an empty
folder plus two explicit playlist files, no ordinary list is created: the
first playlist is appended at `-1`, then the cursor becomes `0` and the second
playlist is inserted at the front. The rebuild previously normalized this
cursor when the ordinary **path vector was nonempty**, even if those paths
produced no tracks. It now normalizes only inside the nonempty track-batch
branch. It also retains a raw nonnegative hit, including the synthetic row
immediately below the final list (`count`), rather than treating it as `-1`.

For existing lists `[A, B]` and incoming playlist files `first`, `second`:

| Drop contents / location | Final catalogue |
| --- | --- |
| Empty folder + playlists, true blank (`-1`) | `[second, A, B, first]` |
| Empty folder + playlists, synthetic bottom row (`2`) | `[A, B, first, second]` |
| Playlists only, true blank / synthetic bottom row | Same respective orders as above |
| Nonempty ordinary batch + playlists, either blank location | `[A, B, first, second, ordinary]` |
| Any of these batches on existing row `A` | `[first, second, A, B]`; ordinary tracks append to `A` |

After each imported playlist the original activates `count - 1`, even if the
playlist was inserted earlier. Existing-source identity is preserved when
insertion shifts its playlist or track index.

Folder naming is independent of whether each folder contributed tracks.
`0048205E..0048208E` scans a directory using `Histroy/CheckSubFolder`, then
unconditionally copies its final path component into the shared temporary
list's title. Multiple folder inputs therefore form one ordinary batch and
the last folder wins, including a final empty folder. If the whole batch is
empty and there are no explicit playlist files, the drop returns no effect
and creates no list. Folder contents are references; the external drop's
`DROPEFFECT_LINK` does not move the source directory or create `.lnk` files.

The local regression harness now adds 18 playback cases (three destinations
times six initial states), nine mixed-order cases, and an empty-folder-only
case. Playback states cover fully idle, stopped with a retained marker,
playing/paused in the target list, and playing/paused in another list while
the target has no marker. Silent PCM fixtures exercise the real audio engine;
the assertions check actual playback state, source identity, selection and
the first inserted track, rather than merely checking that a play request was
issued. Before the fix, the new test reproduced the missing idle autoplay in
the track pane. The mixed-order cases cover both blank hit kinds, existing
rows, playlist-only input, and a nonempty folder followed by an empty folder.

Validation completed on 2026-10-09:

- **334 assertions passed on each of Windows XP, Windows 7 and local Windows
  11**, including the earlier folder chooser/scanning tests. Windows 11 is the
  requested substitute for Windows 10; no actual Windows 10 run is claimed.
- These new cases call the production drop handler using real `CF_HDROP`
  objects and actual list-control coordinates; they are not new end-to-end
  mouse-drag comparisons against the original executable. The expected native
  behavior comes from the pseudocode and x86 call/branch audit above.
- Release `2026.10.09` builds successfully. The player and updater pass the
  static import checks against both XP and Win7 inventories (717 and 292
  imports respectively).
- The local-only harness and results remain under `tests/folder_intake/`
  (`drop-fixed-run.log`, `drop-vm-results.json`,
  `verification-20261009.json`). Test code is not added to the player repository
  or release archive, and Actions do not run it.
- The universal archive is
  `build/Release/TTPlayerRebuild-2026.10.09.zip`. Its five entries are the player,
  updater, HTTPS plugin, rebuilt `ttpcomm.dll` and `SHA256SUMS.txt`; every
  checksum is verified against the archived bytes.

## Legacy XML playlists

`CPlayList_LoadFromFile` at `00475219` reserves only `.ttbl` and
`.m3u`/`.m3u8` for their dedicated readers.  Every other explicitly opened
playlist path enters the XML reader after the playlist file's directory has
become the relative-path base (pseudo lines `114251-114277`).  The SAX callback
at `004749A7` supplies the schema evidence:

- root `<ttplaylist version="..." title="...">`, accepting native versions
  through 4;
- `<items count="..."><item ...>`;
- item attributes `file`, cached `title`, `subtk`, `len`, `ftm`, `SongName`,
  `SongArtist` and `tid`;
- legacy tag attributes for versions 1--3 and version-4
  `<tag><f name="..." val="..."/></tag>` metadata;
- append of the completed item on `</item>` in `0047512A`.

`Playlist::LoadXml` now reads `.ttpl` and other non-TTBL/non-M3U XML files,
accepts a UTF-8 BOM, decodes named and numeric XML entities, restores root
title, CUE sub-track, duration, song/artist/album metadata and resolves relative
media paths against the XML file's directory without changing the process-wide
current directory.  Empty `file` items are ignored as in the callback.  XML
playlist writing is not claimed by this recovery item.

Unit coverage includes a BOM-prefixed `.ttpl`, relative and URL paths,
`subtk`/`len`, `SongName`/`SongArtist`, version-4 `<f>` metadata, a missing-file
item and the arbitrary `.xml` fallback dispatch.

## M3U and persisted TTBL state

The dedicated list readers retain the native distinctions that are visible
during import. `.m3u` text uses the process ANSI code page, `.m3u8` uses UTF-8,
relative rows resolve against the list directory, and `#EXTINF` takes its title
after the last comma. CUE rows expand to sub-tracks. With
`PlayList/IgnoreBadFiles=1`, missing local M3U/XML rows are filtered but URL
rows remain. An empty or comments-only M3U is consumed as a playlist input yet
does not create an empty catalogue item.

The TTBL header's fourth DWORD is CPlayList `+0x1c`, the last-playing row,
rather than the transient ListCtrl caret at `+0x20`. It is restored, remapped
when tracks are inserted/deleted/reordered, and written back with the playlist;
ordinary selection changes update only the in-memory caret. Numbered-list
shutdown also closes slot holes without losing a later slot; a small
transaction marker lets startup finish an interrupted compaction before
scanning the catalogue.

## Isolated validation

The Release probe runs as `WDAGUtilityAccount` and uses a helper that publishes
a real `IDataObject` with `CF_HDROP`, calls `DoDragDrop`, and performs a physical
left-button release. It does not inject `WM_DROPFILES`. The current completed
reports are:

| Report directory | Scope | Result |
| --- | --- | --- |
| `build/sandbox-playlist-import/20260904-210505/` | four native executables plus the `main-single` original/rebuild case | all native exits 0, no timeout; case succeeds |
| `build/sandbox-playlist-import/20260904-212933/` | complete Base group | `Success=true`, 9 scenarios/18 runs |
| `build/sandbox-playlist-import/20260904-213147/` | complete Dialog group | `Success=true`, 4 scenarios/8 runs |
| `build/sandbox-playlist-import/20260904-222201/` | complete Extended group | `Success=true`, 17 scenarios/34 runs |
| `build/sandbox-playlist-import/20260904-225446/` | current Base product matrix after responsiveness fixes | `Success=true`, 9 scenarios/18 runs; no forced termination |
| `build/sandbox-playlist-import/20260904-225758/` | current native matrix plus `main-single` | `Success=true`; all four native exits 0, no timeout |
| `build/sandbox-playlist-import/20260904-220956/` | embedded-CUE ZIP | `Success=true`, 2 runs |
| `build/sandbox-playlist-import/20260904-221045/` | two playlists on catalogue whitespace | `Success=true`, 2 runs |
| `build/sandbox-playlist-import/20260904-221519/` | catalogue-row `WAV/M3U/WAV` | `Success=true`, 2 runs |
| `build/sandbox-playlist-import/20260904-221556/` | catalogue-whitespace `WAV/M3U/WAV` | `Success=true`, 2 runs |

The completed OLE cases preserve the original `0x00040100` return and effect
`4`, keep both processes alive after import, persist the expected order, and
close without forced termination. The focused catalogue runs additionally
match the native distinction among active list, selected catalogue row,
playing row and track caret. In particular, dropping ordinary media on blank
catalogue space creates a list without selecting its imported tracks; playlist
inputs retain the signed insertion cursor/order recovered from `004822AD`.

The embedded-CUE fixture matches the archive path spelling and exact member
lookup recovered from `004E323B`/`0047E177`. Its CUE rows persist the logical
title, `-1` duration, empty metadata and flags `0x43`; the ordinary archive
audio row uses flags `0x42`. A relative member containing `..` is not silently
canonicalized, so the same failed first-CUE open clears the remembered playing
identity in both observed implementations without displaying a decoder-error
dialog.

The original report's lack of visible progress was a test-harness issue, not
evidence of a player message-loop hang. The all-skin native test normally needs
about 67 seconds and now has a 180-second deadline with heartbeat output. OLE,
cross-process messages, dialogs and cleanup all have finite deadlines; progress
and completion JSON are atomically published. Per-scenario VM-local runtime
copies are removed after evidence capture, preventing the previous roughly
46-MB-per-case accumulation from slowing later original-player launches.

The Base fixture also used to begin OLE input 1.2 seconds into a two-second
command-line WAV. That raced the original decoder's natural-completion path.
It now waits three seconds, requires three consecutive bounded responsiveness
replies, and suppresses unrelated online lyric auto-download because Sandbox
networking is disabled. Report `20260904-225446` consequently completes all 18
original/rebuild runs without a forced close.

The rebuild itself now bounds a reader's synchronous open wait to four seconds
and a UI-issued stop reap to 1.5 seconds. The worker is never detached or
terminated, and DLL/object ownership is retained until it exits. Skin-menu
catalogue publication waits at most 40 ms; stale scans use cooperative
cancellation and no longer block menu expansion or `.skn` drop through an
unconditional `future::get`.

While extending the catalogue matrix, an earlier failure exposed a stale
expected-order/selection oracle. The focused reports isolated the corrected
behavior, and the subsequent complete Extended report `20260904-222201` plus
its atomic completion marker both record `Success=true`.
