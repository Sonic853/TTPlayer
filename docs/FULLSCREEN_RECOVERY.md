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
| `0x7DE8` | lyrics | opaque selected-screen lyrics, or that display's color-keyed work-area lyric surface when `TransparentFS=1` |
| `0x7DE9` | visual | full selected-screen visual surface; visual types 0 and 4 are changed to type 1 |
| `0x7DEA` | combined | per-effect `LrcSize*` split, or full visual plus color-keyed lyric overlay when `PosRelation*=1` |

The normal lyric window is hidden for every full-screen mode, including
visual-only mode, and its pre-entry visibility is restored on exit. Mode
switching does not prematurely reveal it. `VK_ESCAPE`, menu exit and restore
use the same exit path. Cross-process activation no longer exits fullscreen
(the requested multi-monitor extension described below).

The generic detach helpers also preserve the less visible parts of the binary
contract: an initially empty control is temporarily made `1 x 1`, the desired
client rectangle is expanded by the detached non-client margins, and restore
first reapplies the saved desktop rectangle before restoring the parent. The
original `0x50` `SetWindowPos` flags and integer-truncated split boundary are
retained, including a legitimate zero-height lyric surface at `LrcSize*=0`.
Stopping playback still synchronously leaves full screen. Cross-process
deactivation leaves the detached surfaces in place.

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
command deletions from `FUN_004427B1`. Its original top-level sequence has 12
items: lyric adjustment, separator, copy, unassociate, reload, separator,
display, simplified/traditional conversion, character encoding, full-screen
submenu, separator and exit full screen. Resource 390 supplies commands
`0x7DE8`, `0x7DE9` and `0x7DEA`.

The full-screen visual path loads resource 145, grafts resource 390, and keeps
the original seven top-level items: dream, spectrum, oscilloscope, separator,
full-screen submenu, separator and exit. Both detached-control menus are raw
native `TrackPopupMenu(..., TPM_RIGHTBUTTON, ..., mainHwnd)` menus; they do not
use the normal skinned owner-draw popup path. The community multi-monitor
extension appends a separator and screen-selection submenu to these menus.

## Multi-monitor extension (2026-09-12)

This is an intentional improvement requested for multi-screen use, not a claim
that the original primary-screen/activation policy was different:

- `FUN_004657DF` exits on a foreign thread's activation; the reconstructed
  `WM_ACTIVATEAPP` and detached controls' `WM_ACTIVATE` paths used to repeat
  that policy. Neither now tears down fullscreen on focus loss. Esc, explicit
  exit, stopping playback and restoring the main window retain their existing
  exit paths.
- Entry resolves `MonitorFromWindow` **before** minimizing/hiding the hosts.
  Main-window and embedded visual menus use the main HWND. Lyric text and
  lyric skin/chrome menus use the lyric HWND, even when chrome uses the main
  menu's contents. For that forwarded menu only, `TPM_RETURNCMD|TPM_NONOTIFY`
  plus explicit command dispatch preserves the scoped source HWND; relying on
  User32's queued `WM_COMMAND` loses it after the popup returns. Each new
  fullscreen session resolves the origin again.
- Layout uses `rcMonitor`, including its left/top offsets; transparent
  lyric-only mode uses that monitor's `rcWork`. Combined split/overlay keeps
  the existing integer rounding and profile settings. An all-lyrics split
  restores any detached visual surface so it cannot remain on a former screen.
- **Only the already-fullscreen right-click menus** have a top-level
  `屏幕选择` submenu. Both visual and lyric surfaces offer all attached displays,
  with resolution, primary-screen marker and current-screen checkmark. The
  ordinary windows' fullscreen submenus are unchanged. Selection repositions
  the existing HWNDs, rebuilds lyric sizing and reconfigures the visual buffer
  dimensions; switching modes retains the selected display.
- Store the active display's device name, not a cached HMONITOR. Re-query on
  `WM_DISPLAYCHANGE`; use the closest display to the previous bounds if the
  device has disappeared. Work-area changes also refresh transparent lyrics.
  This does not modify the user's display topology or add XML settings.
- `WM_CONTEXTMENU` coordinates are already screen coordinates. Do not add the
  lyric monitor offset a second time; keyboard menus anchor to their surface.
- New menu labels are executable string resources (`src/app/resources.rc` /
  `resource_ids.h`), not modifications to the original `ttpres.dll`.

Windows defines window-to-monitor selection by largest intersection (and uses
the pre-minimized bounds):
[MonitorFromWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-monitorfromwindow).
Resolution changes are re-queried instead of treating the primary resolution
in the [WM_DISPLAYCHANGE](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-displaychange)
message as the fullscreen display.

Host regression tool: `tools/probe_fullscreen_monitors.ps1`. It creates an
isolated temporary runtime, a silent WAV and a separate-process focus window;
it never overwrites production configuration or changes monitor arrangement.
It checks both entry windows, both physical displays, all three modes, actual
menu selection/checkmarks, cross-process mouse clicks, mode changes, display
refresh messages, Esc/menu exit and HWND reuse. `-Overlay`, `-Transparent` and
`-Chrome` cover overlay/work-area layouts and lyric-host chrome routing.

The 2026-09-12 Release host runs passed 12 default-layout/text-entry cases and
12 `-Overlay -Transparent -Chrome` cases, with clean shutdown. Both host screens
were 1920x1200, side by side, with 1920x1170 work areas. Actual unplugging,
mixed-DPI changes and negative-origin physical layouts were not exercised;
the probe does not alter system display configuration. Release builds and
`taskbar_playback_tests` also passed; existing runtime XML/INI/TTBL files were
restored and hash-verified after CMake's post-build staging.

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
