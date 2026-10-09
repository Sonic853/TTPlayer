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

## Persistence feedback audit (2026-10-09)

Feedback under review: folder-added lists disappear after closing/reopening,
and playback order returns to its initial setting. This subsection records the
**pre-fix `2026.10.09` audit**. The confirmed defects are addressed by the
`2026.10.09p1` implementation and verification below.
The preceding 334-check import regression did not test process restart and
must not be used as evidence of persistence under every exit condition.

### Three different meanings of order

- Catalogue order, titles and each list's visible track order are stored in
  numbered `PlayList/*.ttbl` files, not in the source music folder.
- The playback mode (single, repeat-one, sequential, repeat-all or random) is
  `Player/PlayMode` in the root XML. `Player/PlayLists` and `ActiveList` record
  the catalogue count and active slot. The rebuild uses `TTPlayerRebuild.xml`;
  it imports the original `TTPlayer.xml` only when the new file is absent.
- The random playback index sequence/cursor is transient. The original
  `0045BBF6` uses main-window `+0x4384`, `+0x4388`, `+0x4394`; these are not
  serialized as the TTBL track order or the XML playback mode. A fresh random
  sequence after restart is different from losing the random-mode setting.

Both rebuild storage paths are relative to the EXE directory. Launching a
different extracted copy therefore loads that copy's state. An unwritable
installation directory currently has no alternate writable state directory.
Neither is established as the third party's actual cause without their
version, executable path and exit method.

### Original pseudocode contract

| Original entry | Persistence behavior |
| --- | --- |
| `0047444A -> 00480386` | Folder collection produces ordinary playlist items; insertion changes the same list and dirty state as other file imports. There is no folder-only volatile-list mode. |
| `00480DE3` (`00480E75` threshold branch) | Dirty-list autosave after elapsed time exceeds 29999 ms or edit count exceeds 4. Writes a temporary TTBL and publishes it; failure keeps it eligible for a later attempt. |
| `004616BD`, reached by `WM_CLOSE` | Captures state, reconciles playlist storage and commits settings before destroying the main window. |
| Main message dispatch, pseudo lines 88801–88804 | `WM_QUERYENDSESSION` (`0x11`) also calls `004616BD` and returns TRUE. Thus original shutdown/logoff participates in persistence. |
| `004812AC` | Records catalogue count and active-list index into `DAT_00547834/38`. |
| `CPlayListManager_ReconcileStorage` (pseudo line 116736) | Saves modified lists, stages/renames numbered files to the current catalogue order and reconciles old storage. |
| `004783FA` | Startup loads numbered TTBL files and restores the active list; supports the original three-/four-digit generations. |
| `004B605A` | Reads/writes `PlayMode`, `PlayLists`, `ActiveList` and other player settings. |

Normal restart is therefore expected to preserve folder-added tracks, list
order and playback mode. The original delayed autosave still permits recent
unsaved changes to be lost on a forced process termination. Its pseudocode
does not establish that access-denied or disk-write failures can be overcome.

### Current implementation and confirmed gaps

**Normal exit works in the tested current build.** Folder insertion calls
`MarkDirty`; a new catalogue list is dirty too. The UI timer calls
`FlushDirty(false)`. `WM_CLOSE` calls `SaveStoredPlaylist()` then
`PersistWindowState()`, with a `WM_DESTROY` fallback. Forced flush writes dirty
TTBLs and compacts slots to preserve catalogue order. XML writes `PlayMode`,
`PlayLists` and `ActiveList`. A normal restart test should not be described as
a reproduction of the reported loss when these writes succeed.

Confirmed unresolved issues:

1. **System-session exit does not save.** There is no `WM_QUERYENDSESSION` or
   `WM_ENDSESSION` handling anywhere in the current player sources. The default
   handler accepts the query but does not execute the normal save transaction.
   With two just-imported short lists and no intervening autosave, the test's
   query/end-session sequence leaves no XML or TTBL. The next process sees one
   empty default list and mode `2`. This is a direct difference from the native
   dispatch above. It can explain the combined symptom when exiting Windows;
   it does not explain a successful ordinary close on its own.
2. **Save failure is invisible to the caller.** `PlaylistStore::FlushDirty`
   catches write errors and returns `void`; it retains dirty entries during
   the live session but cannot report the final failure to shutdown. XML
   `SaveWindowState` likewise returns `void` and ignores the `save` HRESULT.
   The user can see an apparently successful exit even though old state was
   retained. Fault injection blocking both storage destinations reproduces
   empty-list/default-mode restart. This demonstrates a failure path, not that
   the reporter's directory permissions have been diagnosed.
3. **A failed settings save suppresses later attempts.** `PersistWindowState`
   sets `window_state_saved_ = true` before it calls the XML writer. With an
   existing read-only XML at mode `2`, changing to mode `4` fails to save;
   removing the read-only flag and calling persistence again still leaves
   mode `2`, because the guard returns early. The test also loses the new
   active-list setting, while the successfully saved TTBL contents survive.

Additional source-level reliability concern: the root XML is saved directly
to its final path, unlike the TTBL temporary-file transaction. A malformed XML
causes `LoadRuntimeSettings` to return defaults. This audit did not inject a
mid-write crash or disk-full event, so it does not claim a reproduced partial
XML write. XML damage alone does not delete valid TTBL files in the same
runtime directory.

### Restart and failure verification

The local-only probe links the same Release core and creates the real
`PlayerWindow`. It appends a folder, creates a second folder list through the
production `CF_HDROP` handler, reverses two track rows, reorders the catalogue,
changes the active list and playback mode, and issues a normal `WM_CLOSE`.
A separate fresh process loads the saved XML/TTBL and reports its state.
These are host-level tests; the folder chooser itself is covered separately.

| Case | Windows 11 | Windows XP | Windows 7 |
| --- | --- | --- | --- |
| Normal close, all five modes, list/track order and active list | All five pass | Random mode checked, passes | Random mode checked, passes |
| Query/end-session before autosave | Missing-save defect reproduced | Same | Same |
| Failed XML write, then restored write access and retry | Stale-mode/saved-flag defect reproduced | Same | Same |
| Both storage destinations deliberately blocked | Combined loss reproduced | Not run | Not run |

The session probe sends the actual session messages only to its own test
window and then ends that test process without providing an extra normal
close/destruction callback. It does **not** log off or shut down the host/VM.
Windows 11 remains substitute coverage for Windows 10, not an actual Win10 run.

Evidence stays local under `tests/folder_intake/`: `persistence_audit.cpp`,
`persistence_audit.inc`, `run_persistence_audit.py`,
`persistence-20261009-025016/results.json` (16 subprocess runs) and
`persistence-vm-results-20261009.json`. No tests or test execution are added to
Actions or the release archive.

Recommended fix order:

1. Implement explicit session-end state capture/flush, independent of
   close-to-tray and visual/audio closing effects. Handle cancelled session
   termination without leaving saving permanently disabled.
2. Return structured success/failure from playlist and settings commits;
   publish `window_state_saved_` only after successful required writes. Keep
   failed changes retryable and report the affected path instead of silently
   claiming success.
3. Save root XML through a complete temporary document and atomic replacement,
   with a validated recovery copy; retain the preceding file on failure.
4. Add restart, read-only/locked-file and session-end regression coverage to
   the existing local tests. Diagnose the third-party report using its exact
   build, runtime directory and exit method before assigning a single cause.

## Persistence fixes and verification (2026-10-09p1)

The preceding audit's three confirmed gaps are fixed. Folder import still
uses the original dirty-list mechanism; the changes complete the save and
restart paths shared by all playlists.

### Save results and failed-close behavior

- `SaveResult` carries the actual destination and HRESULT. Playlist flush,
  catalogue compaction and root settings save return their result to the
  caller; failed list entries remain dirty for retry.
- `SavePersistentState` completes playlist writes/compaction before writing
  `ActiveList` and the other root settings. A list-write failure prevents a new
  XML index from claiming an unfinished catalogue has been committed.
- `window_state_saved_` is set only after the root XML and active skin profile
  succeed. A failed attempt remains retryable. A changed active slot after
  compaction invalidates an earlier settings snapshot too.
- An ordinary close that cannot save displays the failed path and error code,
  leaves the player and its in-memory contents alive, and does not start close
  fades or disable its windows. After fixing the read-only flag or releasing
  the file lock, another close saves and exits normally. Desktop-lyric unlock
  and auto-shutdown timer cancellation are deferred until that save succeeds.
- Explicit settings saves in the options and related UI paths report errors
  through the same feedback method. Destruction/session-confirmation fallbacks
  log failures without opening a modal dialog during teardown.
- Compaction now also checks removal of its committed old slots/stages/marker.
  A failed cleanup is not reported as a successful transaction and remains
  available for recovery/retry.

### Windows session end and cancellation

`WM_QUERYENDSESSION` captures and saves state without routing through the
caption-close handler. Close-to-tray, lyric save prompts and visual/audio
closing effects therefore cannot prevent the storage commit. A failed query
commit returns FALSE and queues the path-specific error for the live player.

The query snapshot does not permanently latch `window_state_saved_`.
`WM_ENDSESSION(FALSE)` clears the pending/final-save state, so changing the
mode or active list after cancelling shutdown is saved on the later normal
exit. `WM_ENDSESSION(TRUE)` takes a fresh final snapshot, including changes
after the query, without waiting for a normal close/destruction callback.
This retains the original `0x11` persistence intent while supporting cancelled
shutdown. Microsoft's message contract confirms that FALSE means the session
is not ending and that explicit window destruction is not required on TRUE.
See [WM_ENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-endsession)
and [WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession).

### Atomic XML and validated recovery copy

The root and skin-profile XML serializers now use the same commit helper:

1. Serialize into a uniquely named adjacent `.writing.<pid>.<serial>` file.
2. Reparse it, require the `ttplayer` root, and flush its file buffers.
3. Preserve a validated preceding document in `<destination>.bak`; for the
   first save, create a recovery copy of the new valid document. A corrupt
   primary never replaces an existing valid recovery copy.
4. Replace the primary with `ReplaceFileW`, or publish a new file with
   `MoveFileExW`. A read-only or locked destination is an error; the code never
   clears its attributes or deletes it to force a replacement.
5. Clean up only this attempt's staging files. Unknown XML fields are retained.

Startup prefers a valid primary document, then its validated `.bak`. A
missing primary with a good new-format backup uses that backup before trying
the one-time original `TTPlayer.xml` import. A malformed existing rebuild
configuration never revives an unrelated original XML. Early language loading
uses the same primary/backup preference. Incomplete `.writing.*` files are not
used as configuration. The backup is the preceding valid snapshot, so recovery
can retain an earlier setting rather than the very last edit.

### Completed tests and release

All three systems ran the same **13 scenarios / 25 subprocesses**. Each
restart is a fresh process reading the files produced by its predecessor.

| Case | XP | Win7 | Win11 |
| --- | --- | --- | --- |
| Normal close/restart, all five playback modes | Pass | Pass | Pass |
| Added folder lists, catalogue/track ordering and active list | Pass | Pass | Pass |
| Session-end save, including close-to-tray setting | Pass | Pass | Pass |
| Cancel shutdown, change mode/current list, then close | Pass | Pass | Pass |
| Read-only save/query failure, restore write access, retry | Pass | Pass | Pass |
| Ordinary-close error dialog identifies the path and retains the live player | Pass | Pass | Pass |
| Locked TTBL, cancelled close, unlock and successful restart | Pass | Pass | Pass |
| Locked root/backup XML, unchanged primary, successful retry | Pass | Pass | Pass |
| Corrupt/missing primary, valid/invalid backup and unknown XML fields | Pass | Pass | Pass |
| Interrupted compaction and failed autosave retry | Pass | Pass | Pass |

The existing folder chooser/intake/playback suite additionally passes **334
checks on each system** after the changes. Session notifications are sent only
to isolated test windows; these tests do not log off or shut down Windows.
Windows 11 is the requested substitute coverage for Windows 10.

The failure-dialog test originally attempted to dismiss the message box before
its static text/button initialization completed. The local probe now waits
for the exact expected path and closes only that test process's error dialog;
both path matching and retention of the player are asserted before retry.

Local evidence: `tests/folder_intake/persistence-fixed-run.log`, its timestamped
`persistence-fixed-*/results.json`,
`persistence-fixed-vm-results-20261009p1.json`,
`folder-after-persistence.log`, and `verification-20261009p1.json`.
Test sources/results remain local; Actions and the release archive exclude them.

Universal release: `build/Release/TTPlayerRebuild-2026.10.09p1.zip`.
The player and updater pass the XP/Win7 static import audits. Package contents
and all archived file hashes are verified. Source/backup storage still requires
a writable destination; forced process termination or failing storage cannot
be made equivalent to a successful graceful exit.

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
