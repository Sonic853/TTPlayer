# Multi-monitor skin-window snapping

## 2026-09-12 correction

`ContinueSkinBackgroundDrag` previously used `SPI_GETWORKAREA` in both move
and resize paths. That supplied primary-screen edges even when the player
was on another display, and also limited auxiliary-window resizing to the
primary work-area dimensions.

`DragWorkAreaForRect` resolves the proposed screen-space rectangle using
`MonitorFromRect(MONITOR_DEFAULTTONEAREST)` and reads `MONITORINFO::rcWork`.
This follows the largest intersecting monitor, or the nearest monitor when
the proposed rectangle is in a gap. See the
[Microsoft MonitorFromRect contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-monitorfromrect).
Work-area coordinates include negative monitor positions and taskbar insets.
The helper queries current monitor information each time, not a startup cache.
If monitor lookup fails it falls back to the system work area, then the
virtual-screen rectangle.

Both movement and resizing use the proposed rectangle rather than the old
window position. An attached group follows the actively dragged window's
monitor and retains its common translation; window-to-window attraction,
packed Snap_Windows enable/distance settings, resize minimums and skin layout
are unchanged. Main, mini and auxiliary skin windows share this drag handler.
The separate desktop-lyrics drag implementation is covered by the follow-up
below; it retains its own three-surface positioning and capture logic.

## Verification

- Release player/tests build succeeds; `ttplayer_tests` passes.
- Geometry tests cover all four edges on offset, negative, portrait and
  vertically stacked work areas, plus outside-threshold cases. The real Win32
  lookup is checked against every enumerated host monitor's work area.
- `tools/probe_multimonitor_snap.ps1` uses a private default-skin runtime and
  actual mouse input on the host. Both work areas `(0,0)-(1920,1170)` and
  `(1920,0)-(3840,1170)` pass left/right/top/bottom cases at gaps of 7 and
  11 pixels with a configured 10-pixel threshold: 16 cases, clean exit.
- Host report: `%TEMP%/TTPlayer-monitor-snap-ed64e87692734d01a84e5373b6488b66/report.json`.
- Negative-coordinate layouts are synthetic test cases; the host displays
  were not rearranged. Release configuration files were restored and hash
  checked after the build. Existing desktop-lyrics edits were not changed.

## Desktop-lyrics follow-up (2026-09-12)

Desktop lyrics also used the primary `SPI_GETWORKAREA` and literal zero for
the left/top limits in `PositionWindows`. Moving the lyric rectangle beyond
those limits immediately pulled it back, even if the other monitor was valid.
Both its ten-pixel snap calculation and three-surface positioning now resolve
the proposed rectangle through `DragWorkAreaForRect`. Reachability limits and
toolbar placement use that monitor's left/top/right/bottom work-area coordinates.
The control and paint HWNDs retain identical rectangles; the toolbar remains
reachable on the selected monitor. Stored secondary/negative coordinates are
not reinterpreted as primary-screen positions on creation or settings refresh.

The existing lock/input/render/scroll paths and the staged 400..10000 width
range are preserved. Old pixel-scroll tests had assumed a 320-pixel viewport
and a 1000-pixel cap; the UI test fixtures now use 400 and 10000 respectively,
without changing the pure viewport geometry tests.

Release build and `desktop_lyrics_tests` pass, including cross-monitor
control/paint/bar alignment, four-edge snapping and recreation from saved
secondary-screen bounds. Host physical-input `probe_multimonitor_snap.ps1
-Desktop` passes 16 edge/distance cases plus primary/secondary/primary moves.
Report: `%TEMP%/TTPlayer-monitor-snap-2fa42d31737e48159e4b4952b5e93a9a/report.json`.
The host screens are side by side; negative-coordinate physical layouts were
not configured. No user display topology, music or stored configuration was
changed for the tests.
