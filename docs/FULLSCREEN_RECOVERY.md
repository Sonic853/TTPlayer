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
| `0x7DE8` | lyrics | opaque selected-screen lyrics, or that display's transparent work-area lyric surface when `TransparentFS=1` |
| `0x7DE9` | visual | full selected-screen visual surface; visual types 0 and 4 are changed to type 1 |
| `0x7DEA` | combined | per-effect `LrcSize*` split, or full visual plus transparent lyric overlay when `PosRelation*=1` |

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
the original transparent path uses non-antialiased glyphs to avoid color-key
fringes. Since 2026-09-15, the requested smooth fullscreen extension instead
uses grayscale coverage and per-pixel alpha, while preserving the saved font
and ordinary/mini color-key behavior. See
[FULLSCREEN_LYRIC_RENDERING.md](FULLSCREEN_LYRIC_RENDERING.md) for original
evidence, the intentional rendering difference and host pixel tests.

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

## Taskbar after switching to spectrum (2026-09-17)

The combined-mode profiles can change the window geometry when selecting a
different effect. With dream/oscilloscope/album configured as overlays and
spectrum configured as a split, the transition shrinks the full-monitor visual
HWND to the upper part of the monitor. The lyric HWND covers the remaining
bottom part. Neither individual HWND then covers the whole monitor.

The reproduced failure occurred after activating the lyric surface and
switching from an overlay to spectrum: both controls still had
`WS_EX_TOPMOST`, but Explorer raised the taskbar above the bottom lyric area.
This is a missing shell fullscreen notification during the layout change;
the spectrum renderer and the original split-boundary calculation were
unchanged.

`DetachVisualWindow` and `DetachLyricControl` now publish their fullscreen
state through `ITaskbarList2::MarkFullscreenWindow`. Restore clears each mark
before reparenting the control. Transparent lyrics-only mode uses the desktop
work area, so its lyric HWND is explicitly unmarked. `TaskbarCreated` refreshes
the active layout and republishes the marks without activating the player.
The interface is acquired for each update, avoiding a cached connection to a
previous Explorer instance.

Microsoft documents that an explicitly marked window makes the shell treat it
as fullscreen and lower the taskbar while that window is active. This interface
is available starting with Windows XP:
[ITaskbarList2::MarkFullscreenWindow](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-itaskbarlist2-markfullscreenwindow).

Local regression: `tests/ui/fullscreen_taskbar_probe.py`, using an isolated
temporary runtime, silent WAV, independent configuration and the real host
taskbar. It checks repeated overlay/spectrum transitions, native visual-menu
selection with a pointer click, a simulated `TaskbarCreated` notification,
transparent desktop lyrics, visual-only mode, combined reentry and exit. It
asserts which process owns the visible window at the taskbar position, as
checking only the player windows' topmost styles missed the original failure.
Tests remain local in `tests`; the release workflow does not run or package
them. Simulating the shell notification does not restart Explorer, and running
the legacy executable on the development host does not establish XP/Win7
runtime behavior.

Both Release variants passed all 15 shell integration checkpoints on the
development host. `fullscreen_album_tests`, `fullscreen_lyric_drag_tests` and
`window_topmost_tests` also passed for both builds. The legacy drag test needed
an isolated rerun: diagnostic output from the failed run recorded the cursor
moving from the requested `(72,72)` to `(987,624)` and the button being released
during the gesture. The legacy PE audit still passes all 641 static imports
from 18 DLLs against XP and Windows 7, with the subsystem version kept at 5.01.

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


## Win7 Aero shell verification — 2026-09-21

The current legacy Release passed the same 15 shell checkpoints on Win7 SP1 with Aero enabled, using a local native harness and silent fixtures. Coverage includes repeated overlay/spectrum transitions, a physical click in the native visual menu, transparent lyrics using the work area, visual-only/combined reentry and exit. The window hit at the taskbar position belonged to the player during fullscreen and to Explorer after exit. `TaskbarCreated` was simulated; Explorer was not restarted. See the [Aero VM report](XP_WIN7_VM_VALIDATION.md#aero-开启后的补充验证2026-09-21).

## 2026-11-05：透明全屏模式切换的置顶修复

### 原版规则

本次重新核对根目录 5.7.9 `TTPlayer.exe`，SHA-256 为
`c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c`。
原版依据来自已有 `reverse/decompiled/TTPlayer.exe.pseudo.c`；运行回归针对重建版，
不把它表述为此次重新运行原版或新生成 REA 反编译证据。

| 原版函数 | 关键分支 | 对应行为 |
| --- | --- | --- |
| `0046228D`，全屏配置 `0x2000` 分支 | `0x7DE8/0x7DE9/0x7DEA` 设置模式 1/2/3 | 仅歌词／仅视觉／歌词与视觉同屏 |
| `0046228D`，`00462AE8` 分支 | 恢复进入前的 `TransparentFS`；支持透明时传工作区矩形和参数 1 给 `0044ABB1` | 透明“仅歌词”是桌面工作区上的置底窗口，不应强制置顶 |
| `0046228D`，组合布局 | 按 `LrcSize*` 和 `PosRelation*` 计算分屏或叠加；传参数 0 给 `0044ABB1` | 同屏歌词应置顶，包括透明叠加歌词 |
| `0044ABB1 → 0044E090` | `hWndInsertAfter = (param_2 != 0) * 2 - 1`，标志 `0x50` | 参数 0 是 `HWND_TOPMOST(-1)`，参数 1 是 `HWND_BOTTOM(1)`；显示而不激活。每次布局都执行，不只第一次脱离父窗 |
| `0044ABB1 / 0044ABFC` | 脱离后把 `WS_CHILD` 改成 `WS_POPUP`；恢复时反向操作 | 复用歌词 HWND，保留退出后的父窗口和矩形 |
| `0044E1CA`、`0046228D` 退出分支 | 先恢复原矩形与父窗口，再恢复配置和普通窗口 | 模式切换不能提前显示普通歌词窗口；退出后还原原透明偏好 |

原版没有重建版为现代透明全屏空白区域增加的输入代理窗口。
逐像素平滑文字和现代输入覆盖仍属于已记录的扩展，不宣称原版具有这些实现。

### 根因与修复

重建版原有 `UpdateFullScreenLayout` 已正确选择 `HWND_BOTTOM / HWND_TOPMOST`。
故障出在 `LyricCtrl` 拥有的 `TTPlayer_FullScreenLyricInput`：它仍存在时，
`SetWindowPos` 会连带调整被拥有窗口；歌词收到同步 `WM_WINDOWPOSCHANGED`、
`WM_STYLECHANGED` 后又立即更新或销毁输入代理，干扰了这一轮层级迁移。

修复前的真实 HWND 跟踪记录了：从透明仅歌词进入同屏模式时，输入代理取得
`WS_EX_TOPMOST`，歌词控件的该标志仍为 0；最终代理被移除后，歌词仍未置顶。
这不是把“透明仅歌词置底”错误地当成 BUG。对应失败日志保留在
`tests/fullscreen_zorder_20261105/before.log`。

Windows 的拥有关系会影响置顶组：提升拥有者会同时提升被拥有窗口，降低被拥有窗口
也可能影响拥有者；`SWP_NOOWNERZORDER` 不能代替完整的层级迁移管理。
参见 [SetWindowPos 的拥有关系与置顶规则](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos#remarks)。

`DetachLyricControl` 现在按以下顺序操作：

1. 暂停同步消息中的输入代理刷新；使用作用域恢复标志，避免提前恢复。
2. 若即将改变置顶组，或进入不透明歌词，先销毁旧输入代理。
3. 完成原版要求的父窗口、位置、层级、样式和透明绘制更新。
4. 在最终状态下刷新输入区域；只有仍需要且系统支持时才创建代理。

重复布局且层级不变时保留代理 HWND，不在每次布局中无条件销毁重建。
歌词控件本身仍复用，退出恢复和透明仅歌词置底规则保持不变。
没有增加新 API 导入，也没有放宽 `WS_EX_NOREDIRECTIONBITMAP` 的系统门槛：
输入代理仍只在 Windows 8 及以上并开启桌面合成时使用；XP／Win7 沿用原有歌词窗口路径。

### 验证与发行

本地测试源码保存在 `tests/ui/lyric_fullscreen_visibility_tests.inc`，通过真实播放器
窗口和原命令处理函数执行；只用测试时钟代替音频播放，不打开音频设备。
16 组普通／迷你、透明／不透明、上下分屏／叠加、输入代理开／关组合，
每组依次选择模式 `1→3→1→2→3→2→1→3→1`，共 144 次模式选择。
检查实际可见性、父窗口、矩形、置顶标志、代理与歌词相邻且同属一个置顶组、
重复布局保留代理、隐藏宿主重排不挤压全屏、双击／菜单退出后的恢复及透明偏好还原。

| 环境 | 执行方式 | 结果 |
| --- | --- | --- |
| Windows 11，10.0.26200 | 本机 Release，实际启用现代输入代理 | 16 组通过，退出码 0 |
| Windows XP SP3，5.1.2600 | 用户已启动的 XP 虚拟机，通过 VirtualBox MCP 复制、执行并回收日志 | 同一测试 EXE 的 16 组通过，退出码 0，现代输入代理未创建 |
| Windows 7 SP1，6.1.7601 | 用户已启动的 Win7 虚拟机，通过 VirtualBox MCP 复制、执行并回收日志 | 同一测试 EXE 的 16 组通过，退出码 0，现代输入代理未创建 |

另通过本机完整 `lyric_main_menu_tests`：上一轮 17 个工具栏按钮、逐字／区间歌词、
快捷键、编辑中播放结束，以及此前未通过的全屏扩展回归。
`fullscreen_lyric_drag_tests --messages-only` 的拖动、取消、捕获丢失和双击输入回归通过。
本次没有执行共享桌面的真实鼠标 `SendInput` 测试，也不把层级断言等同于重新完成所有
任务栏遮挡、显示器热插拔、混合 DPI 或全屏渲染像素验收。

日志目录为 `tests/fullscreen_zorder_20261105`：`host-matrix-final.log`、
`host-expanded.log`、`host-input.log`、`XP-fullscreen.log`、`Win7-fullscreen.log`，
两台虚拟机的 `*-fullscreen-exit.txt` 均为 0。
Release 主程序和更新器的 XP／Win7 静态导入审计通过；主程序仍为 x86、子系统 5.01。
更新 `build/Release` 中的两种主程序及 `TTPlayerRebuild-2026.11.05.zip`、
`TTPlayer-2026.11.05.zip`。测试只保留本地，不进入发行包或 Actions。
