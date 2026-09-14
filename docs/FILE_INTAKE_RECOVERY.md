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
| Playlist track pane | All paths are inserted at the hit/insertion row. The active playing row is remapped if insertion occurs before it; the drop itself does not start another song. |
| Playlist catalogue row | Ordinary files append to the hit playlist. Dropping on catalogue whitespace creates a new list; a dropped playlist file is added as its own list. |
| Lyric window | Only `HDROP` item zero is inspected, matching `0044AC70`; the lyric loader then validates/loads that first path. |
| Equalizer | No target is registered and the drop is rejected. |

The playlist target maintains an insertion cue while OLE is over the track
pane and a target-list cue while it is over the catalogue.  These are visual
reconstructions of the native hit-test contract, not evidence that the lost
private drawing implementation had the same class layout.

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
