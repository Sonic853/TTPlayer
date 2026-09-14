# Windows taskbar thumbnail playback controls

This is a requested modern Windows integration, not an assertion that the
original TTPlayer executable contained this feature. It uses the existing
player's playback commands, playlist navigation policy and `ttpres.dll` labels.

## Behavior

- Hover the player's Windows taskbar button to show the native thumbnail toolbar:
  previous, play/pause, next. Playback switches the middle icon and tooltip.
- Previous/next use the main skin controls' playlist-boundary and play-mode
  rules. Empty lists disable playback; opening, stop-fade and application close
  disable transport commands. Stale clicks are checked against live state.
- Clicks do not dismiss the thumbnail, activate or restore the main window.
  Playback state refreshes also update the toolbar while minimized.
- Existing mini-mode/tool-window and hide-to-tray rules are preserved. Windows
  only shows this toolbar when the main window has a taskbar entry; this change
  does not force a taskbar button for tray-only or tool-window modes.
- No custom preview window, global media-key hook, SMTC session, new setting,
  plugin or network dependency is introduced.

## Implementation

`TaskbarPlaybackControls` owns `ITaskbarList3` and four alpha icons on the UI
thread. Registration waits for the registered `TaskbarButtonCreated` message;
three buttons are installed with `ThumbBarAddButtons`, then updated only when
state, labels or system icon dimensions change. Explorer's `TaskbarCreated`
notification and HWND destruction release registration state and icon/COM
resources. A repeated button-created notification updates an existing toolbar,
or reinstalls it if the shell reports it absent. Optional shell failures do not
block playback or startup.

Main-window hiding also releases registration state. Mini-mode switching uses
`ShowWindow(SW_HIDE)` and fullscreen entry uses `SetWindowPos(SWP_HIDEWINDOW)`;
both retain the HWND but remove its taskbar entry. `WM_WINDOWPOSCHANGED` with
`SWP_HIDEWINDOW` now calls `Reset()`. The subsequent `TaskbarButtonCreated`
therefore calls AddButtons rather than trusting an update to the obsolete
entry. No registration is attempted before the shell's notification, and
ordinary minimization does not reset the toolbar. DefWindowProc still processes
WINDOWPOSCHANGED so skin layout receives its normal move/size notifications.

The native `WM_COMMAND / THBN_CLICKED` route is handled separately from existing
menu commands. Labels come from commands `kCmdPrevious`, `kCmdPlay`, `kCmdPause`
and `kCmdNext`; `PlayCurrent`, `Pause`, `Resume` and `SelectRelative` remain the
only transport implementations.

Microsoft contracts:
[ITaskbarList3](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-itaskbarlist3),
[ThumbBarAddButtons](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-itaskbarlist3-thumbbaraddbuttons),
[THUMBBUTTON](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ns-shobjidl_core-thumbbutton).

## Verification (2026-09-12)

2026-09-12 regression: Release build and `taskbar_playback_tests` pass. The
test includes six same-HWND hide/recreate cycles with successful obsolete
update responses, confirming a fresh Add and current play/pause state after
each reset. Host `probe_taskbar_playback.ps1 -ModeCycles` passed two mini-mode
and two combined-fullscreen round trips on the default skin. Each return
retained the main HWND and restored all three real Explorer buttons; physical
pause/resume clicks worked without restoring the minimized player. Test copies
use silent WAVs and private settings; original Release configs were restored
with hash verification. No Explorer restart or Windows Sandbox was used.
Host report: `%TEMP%/TTPlayer-taskbar-modes-1aa039f664254e489886bf1f1ad9407a/report.json`.

## Verification (2026-09-07)

Release build and all 24 CTest cases pass. `taskbar_playback_tests` exercises
registration timing, duplicate/recreated entries, play/pause icons, disabled
commands, tooltip truncation, cached updates and COM error/retry cleanup with
an injected shell implementation. Explorer restart is simulated, not performed
on the user's desktop.

Host probe `tools/probe_taskbar_playback.ps1` creates an isolated runtime, two
120-second silent WAVs and a private playlist/config. It uses UI Automation/MSAA
to locate the real Explorer thumbnail buttons and mouse input to click them;
it does not substitute direct player messages for thumbnail transport clicks.
The sequence play, pause, resume, next, previous passed while the main window
remained minimized; previous/next availability changed at the two-track
playlist boundaries, and the process exited normally. Report and thumbnail:
`build/host-taskbar-playback-14/report.json` and `preview.png`.

Run in an interactive host desktop without concurrent mouse automation. Use a
new output directory each time. Windows PowerShell 5.1 must read this UTF-8 script
explicitly (or use PowerShell 7):

```powershell
$probeScript = [scriptblock]::Create([IO.File]::ReadAllText(
  "$PWD/rebuild/tools/probe_taskbar_playback.ps1", [Text.Encoding]::UTF8))
& $probeScript -Runtime "$PWD/rebuild/build/Release" `
  -OutputDirectory "$PWD/rebuild/build/host-taskbar-new"
```

The probe temporarily moves the cursor, restores it on exit and operates only
on its own player instance. It requires a unique visible player taskbar entry;
it stops if that identification is ambiguous. Host shell policy, an inactive
desktop or concurrent mouse use can prevent a preview from opening.
