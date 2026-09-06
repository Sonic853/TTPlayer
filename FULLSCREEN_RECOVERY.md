# Full-screen visual and lyric recovery

This reconstruction follows the original full-screen path rather than creating
new replacement windows. The relevant recovered entry points are:

| Original routine | Rebuilt responsibility |
| --- | --- |
| `FUN_0046228D` | mode `0/1/2/3` state machine, main-window snapshot/hide/restore and layout selection |
| `FUN_0044ABB1` / `FUN_0044ABFC` | detach and restore the existing `LyricCtrl` |
| `FUN_00457A75` / `FUN_00457A94` | detach and restore the existing `VisualCtrl` |
| `FUN_004427B1` | native full-screen lyric context menu |
| `FUN_00458020` | native full-screen visual context menu |
| `FUN_0044B4A1` / `FUN_004499AD` | apply full-screen lyric fields and `AutoFontFS` |

## Layout and window ownership

`LyricCtrl` changes from `WS_CHILD` to `WS_POPUP`, gains
`WS_EX_TOOLWINDOW`, is reparented to the desktop, and retains its original
parent and rectangle for restoration. `VisualCtrl` deliberately retains
`WS_CHILD` while it is reparented to the desktop, matching the original HWND
contract.

The three commands retain their original meanings:

| Command | Mode | Layout |
| --- | --- | --- |
| `0x7DE8` | lyrics | opaque primary-screen lyrics, or a color-keyed work-area lyric surface when `TransparentFS=1` |
| `0x7DE9` | visual | full primary-screen visual surface; visual types 0 and 4 are changed to type 1 |
| `0x7DEA` | combined | per-effect `LrcSize*` split, or full visual plus color-keyed lyric overlay when `PosRelation*=1` |

The normal lyric window is hidden for every full-screen mode, including
visual-only mode, and its pre-entry visibility is restored on exit. Mode
switching does not prematurely reveal it. `VK_ESCAPE`, menu exit, restore and
cross-process activation all use the same exit path; the color-keyed
lyrics-only desktop mode is the original deactivation exception.

The generic detach helpers also preserve the less visible parts of the binary
contract: an initially empty control is temporarily made `1 x 1`, the desired
client rectangle is expanded by the detached non-client margins, and restore
first reapplies the saved desktop rectangle before restoring the parent. The
original `0x50` `SetWindowPos` flags and integer-truncated split boundary are
retained, including a legitimate zero-height lyric surface at `LrcSize*=0`.
Stopping playback and cross-process deactivation synchronously leave full
screen before the corresponding playback or activation work continues.

## Full-screen lyric configuration

The rebuilt settings reader/writer now preserves the independent values used
by the detached control:

`ScrollModeFS`, `TextAlignFS`, `RowIntervalFS`, `FadeIndexFS`,
`FadeHilightFS`, `KaraokeModeFS`, `TransparentFS`, `AutoFontFS`, `FontFS`,
`TextColorFS`, `HilightColorFS` and `BkgndColorFS`.

`AutoFontFS` first identifies the widest row with the control's current font.
Only when that raw width does not fit does it try the configured positive font
height in four-pixel steps; each candidate's fit width includes that
candidate's absolute height and must be strictly smaller than the client
width. This preserves the original `16 -> 12` terminal case and never writes
the temporary height back to `TTPlayer.xml`. A nonzero lyric `CharSet`
overrides the selected normal or full-screen `LOGFONT` after fitting, and
transparent lyrics use non-antialiased glyphs so the background color key
leaves no fringe.

## Context menus

The full-screen lyric path loads resource 143 and performs the positional and
command deletions from `FUN_004427B1`. Its final top-level sequence has 12
items: lyric adjustment, separator, copy, unassociate, reload, separator,
display, simplified/traditional conversion, character encoding, full-screen
submenu, separator and exit full screen. Resource 390 supplies commands
`0x7DE8`, `0x7DE9` and `0x7DEA`.

The full-screen visual path loads resource 145, grafts resource 390, and keeps
the original seven top-level items: dream, spectrum, oscilloscope, separator,
full-screen submenu, separator and exit. Both detached-control menus are raw
native `TrackPopupMenu(..., TPM_RIGHTBUTTON, ..., mainHwnd)` menus; they do not
use the normal skinned owner-draw popup path.

## Verification

Build and unit tests:

```powershell
cmake --build rebuild/build --config Debug --parallel 4
ctest --test-dir rebuild/build -C Debug --output-on-failure
```

The reproducible Windows Sandbox harness is documented under
`tools/windows_sandbox/`. It compares the supplied original and rebuilt
runtimes in opaque lyrics, desktop-transparent lyrics, visual, combined-split
and combined-overlay variants. It records HWND styles/parents/rectangles,
captures both menus, clicks the visible exit item with real pointer input,
re-enters, sends a physical Escape key and verifies bounded clean shutdown.

On 2026-09-04 the same probe was also run directly on the development host for
both executables. All ten runtime/variant cases entered, opened the expected
12-item lyric or seven-item visual menu, exited by a real pointer click,
re-entered, restored with a physical Escape key and shut down without a forced
process termination. Windows Sandbox itself was not present on that host
(`WindowsSandbox.exe` and the `Containers-DisposableClientVM` feature were
absent), so this host result is deliberately not labelled as isolated proof.
`run_fullscreen_sandbox.ps1 -GenerateOnly` validates the `.wsb`; after the
optional feature is installed, the same launcher performs and automatically
checks the isolated comparison.
