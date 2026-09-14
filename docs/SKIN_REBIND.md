# Skin switching: retain windows and rebind controls

## Recovered boundary

The package object is replaced; the existing player/lyric/equalizer/playlist
windows are not recreated. These two lifetimes were conflated by the rebuild.

| Original address | Observed operation | Rebuilt correction |
| --- | --- | --- |
| `00465695` | compare selector; invoke one package-switch transaction | one shared loader for menu and installed-skin drop routes |
| `0045D5FA` | save outgoing profile; validate package; merge target visual profile | validate first, keep the previous GDI package alive, commit target settings before presentation |
| `0045DDEE` | restore iconic window; leave mini mode for a different package; retain old package through binding | preserve these transitions without recreating HWNDs |
| `00468363` / `0046D0C1` | reuse main HWND, resize, bind images/controls, replace region | redraw suppression around the live update |
| `00449451` / `0044E5CE` / `0044919C` | update lyric skin and existing child control bindings | retain LyricCtrl, buttons and RichEdit; update font/colors/layout only |
| `0046ACE2` / `0042955B` | update existing EQ slider/button bindings and positions | retain controls even when the new skin omits their images |
| `0046BCBE` / `0047E6FC` | update playlist/list/toolbar layout and images | toolbar updates and WM_SIZE no longer destroy and recreate buttons |
| `004A43E8` / `004710A5` | shadow-option update hides/shows the window group | no hide/show when class shadow style is unchanged during a skin switch |

The previous `ApplyLoadedSkin` called `ApplyWindowShadow` unconditionally.
Although its main HWND survived, that operation hid and showed it, removing and
reintroducing its taskbar entry and triggering group activation/fade behavior.
Meanwhile the lyric and EQ child controls were destroyed on every update, as
were playlist toolbar buttons even on resize. The menu then called
`ApplyLoadedSkin` a second time after reading the sidecar. This also discarded
an open lyric editor's unsaved document, selection and undo state.

## Current transaction

1. Extract/parse and validate the target background region before mutating live
   windows. An invalid archive/region leaves the previous skin active.
2. Save the outgoing profile once. Finish pending window fades; restore an
   iconic main window and leave mini mode when replacing its package.
3. Retain the previous package/GDI resources while binding the replacement.
   Read target package defaults and `.skn.xml`/`Skin/Default.xml` before applying
   visible controls. Runtime profiles default supported auxiliary visibility
   to on when no sidecar exists, as in `0045D5FA`; saved visibility wins when it
   does exist. Options Reset All uses its supplied settings, without an outgoing
   capture or a target sidecar overwriting those defaults.
4. Update existing windows and controls in place with redraw temporarily
   suspended. Only resume WM_SETREDRAW for previously visible windows, so hidden
   auxiliary windows are not inadvertently revealed. Absent EQ/toolbar controls
   are parked rather than destroyed; a later skin can reuse them.
5. Apply final saved geometry/visibility, sync normal/mini geometry caches, then
   release the previous package. Keep playlist selection and the live RichEdit
   document/selection/modified flag. Destruction remains part of real teardown,
   not ordinary skin switching.

Mini-mode exit intentionally still hides/shows the main window (`00464B6C`).
Changing the actual shadow option also retains its native hide/show behavior.
Neither exception means a package switch should create a new main HWND.

## Topmost state during rebind

Geometry must also preserve the native window band, not only the stored
`TopMost` value. The 2026-09-13 fix removes the owned lyric window's unconditional
`HWND_NOTOPMOST` geometry call and reconciles normal/mini, lyric and desktop
topmost state at transition boundaries. See [WINDOW_TOPMOST.md](WINDOW_TOPMOST.md)
for the original-code evidence, owner-propagation failures and host regressions.

## Host verification, 2026-09-07

- Release build and all **25 CTest cases passed**.
- `skin_rebind_tests` runs from a separate temporary EXE-local runtime with no
  user playlist/config/media. It tags 40 existing HWNDs with properties to catch
  even recycled handle values. Two default → LX-iPlay → TT2012 → Let's Vista →
  default rounds preserve every tagged HWND. Resizing the playlist also retains
  its toolbar controls. The normal switch sends no main WM_SHOWWINDOW hide and
  no WM_DESTROY; unsaved RichEdit text/selection/modified state and multi-selection
  survive. Broken archives, saved hidden windows and mini-to-normal package
  switching are included.
- `tools/probe_skin_rebind.ps1` runs copies of both the supplied original EXE and
  the rebuilt Release EXE, uses their real right-click Skin submenu and mouse
  selection, and captures top-level/child HWNDs before/after/return. For default
  ↔ LX-iPlay, default ↔ TT2012 and default ↔ Let's Vista, each executable retained
  all its own observed HWNDs (59 original, 38 rebuilt without the editor).
  This checks lifecycle parity, not equality of the two complete control trees.
- Main and supported auxiliary window rectangles/visibility matched for those
  fixtures. LX-iPlay's unsupported EQ HWND is retained, hidden and parked at
  `(32767,32767,32767,32767)`, matching the original host observation.

Reports: `build/host-skin-rebind-3/report.json` and its six isolated runtime
subdirectories. The host probe restores the cursor and closes its own processes;
do not run it concurrently with other mouse automation. Original and rebuilt
user configuration/playlist files are not used as probe outputs.

Example (Windows PowerShell 5.1 reads the UTF-8 script explicitly):

```powershell
$probeScript = [scriptblock]::Create([IO.File]::ReadAllText(
  "$PWD/rebuild/tools/probe_skin_rebind.ps1", [Text.Encoding]::UTF8))
& $probeScript -Repository "$PWD" -RebuiltRuntime "$PWD/rebuild/build/Release" `
  -OutputDirectory "$PWD/rebuild/build/host-skin-rebind-new"
```

This does not claim binary-identical private UI classes or exhaustively verify
every third-party skin. The recovered and tested boundary here is package
switching and existing-window/control lifetime.
