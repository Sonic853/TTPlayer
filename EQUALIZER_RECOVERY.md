# Equalizer recovery

This implementation follows the private equalizer path in
`TTPlayer.exe.pseudo.c`; it is not a generic replacement dialog.

## Recovered object and skin layout

- `CSkinParser_ParseEqualizerWindow` at `004A8B73` parses
  `equalizer_window.position`, `image`, and `eq_interval`.
- `title` and `close` are image elements; `enabled`, `profile`, and `reset`
  are four-frame buttons; `balance`, `surround`, `preamp`, and `eqfactor` are
  skinned sliders.
- `FUN_0042955B` clones `eqfactor` ten times at
  `template width + eq_interval`.
- `FUN_00429AAA` creates balance `0x66` with range `-10..10`, surround
  `0x67` with `0..16`, preamp `0xC8` with `-12..12`, and ten bands
  `0xC9..0xD2` with `-12..12`.
- `FUN_0046AF50` creates the separate owned window named `Equalizer` using
  `WS_POPUP` and `WS_EX_TOOLWINDOW`. Command `0x7D65` alternates exactly
  between `SW_HIDE` and `SW_SHOW`; closing the popup never exits the player.

The parser retains missing image handles instead of rejecting the skin. This
matters for the shipped `Let's Vista (浅蓝+蓝灰).skn`: its XML references
`progress_thumb3.bmp`, but that file is absent from the ZIP. The background,
fills, coordinates, and remaining controls still load. LX-iPlay contains no
`equalizer_window`, so no substitute generic equalizer is invented.

## Input state machine

The shared slider routines at `004521C5`, `00452326`, `00452295`, and
`00452413` are represented directly:

- pressing the thumb captures that `SkinSlider` child window and preserves the pointer-to-thumb
  offset;
- pressing the track jumps immediately, sends the final-position notification,
  and retains only the slider's non-pressed hover capture;
- captured movement sends a live value change only when the value changes;
- release commits the current captured value without recalculating it from the
  mouse-up point, then releases capture;
- hover applies only over the thumb; bitmap frames are normal, hot, pressed,
  and disabled;
- Escape restores the drag-start value; horizontal sliders accept Left/Right,
  and vertical sliders accept Up/Down;
- background drag uses the same attached-window and ten-pixel magnetic snap
  path as the main and playlist windows;
- right-click anywhere on the equalizer opens its profile popup; double-click
  and mouse-wheel have no equalizer-specific action.

The slider notification is also part of the player-window protocol, not just
an EQ value update. `FUN_00429DDB` first forwards the original
`WM_HSCROLL`/`WM_VSCROLL` message and slider HWND to the player. In
`FUN_00460AB1`, `SB_THUMBTRACK` (`5`) temporarily replaces the main skin's
`status` text, while `SB_THUMBPOSITION` (`4`) restores the stored playback
status. Balance uses resource `0x81BD/0x81BE/0x81BF`, surround uses
`0x81C0/0x81C1`, and preamp/bands `0xC8..0xD2` use `0x8177`. The rebuild now
preserves that tracking-only lifetime and reads every format string from the
EXE-local `ttpres.dll`; a track click or keyboard change therefore does not
leave an EQ value in the main status field.

EQ control enablement is changed only when the enabled/profile state changes,
as in `FUN_00429E86`. Reissuing `EnableWindow(FALSE)` for the already-disabled
preamp/bands during a surround drag generates `WM_CANCELMODE` on Win32; that
had released the surround slider's capture and reverted its new value in the
rebuild. Avoiding that spurious state transition restores the original
continuous surround drag.

One ordering detail is essential in the reconstructed parent-owned state
machine. `SetCapture` synchronously sends `WM_CAPTURECHANGED` to the control
that previously owned capture. The new active/hover slider is therefore stored
*after* `SetCapture` returns. In the original each `SkinSlider` owns these
fields independently (`004521C5`/`00452326`); storing the aggregate parent
field first lets the synchronous old-control notification erase the new drag
and produces the observed “thumb cannot be dragged” failure.

`FUN_00451E07` also establishes an easy-to-miss paint rule: slider fill
bitmaps keep their native dimensions and are clipped at the thumb centre. They
are not stretched to the complete slider client. Horizontal fills are centred
vertically and copied from the left; vertical fills are centred horizontally
and copied from the thumb centre to the bottom. `FUN_00451F59` supplies the
normal/hot/pressed/disabled four-frame thumb state.

The profile popup is owner-drawn. `WM_MEASUREITEM` and `WM_DRAWITEM` are routed
through the equalizer window itself, as they are by the original owner window;
routing them only through the main player produces the characteristic blank,
20-pixel-wide popup. Resource menu `0x90` supplies Recommended, Custom, the
preset submenu, load/save, enable-EQ, and Dolby-Surround commands.

## Native child controls and hover information

The original equalizer does not hit-test a single painted parent window.
`FUN_00429AAA` creates real `SkinButton` and `SkinSlider` child HWNDs. The
rebuild now exposes the same observable classes and IDs: profile `0x64`, reset
`0x65`, balance `0x66`, surround `0x67`, preamp `0xC8`, bands `0xC9..0xD2`,
and close `0x7D65`. Mouse capture therefore belongs to the slider being
dragged, just as required by `FUN_004521C5/00452326/00452295`; the parent still
composites the complete bitmap skin, while child messages are translated into
the recovered equalizer state machine.

The non-drag hover path also follows the private controls rather than a parent
`TrackMouseEvent` approximation.  `0040A5E3` gives a `SkinButton` capture on
entry, selects frame one, and releases/restores frame zero when its captured
move leaves the client (unless the left button is held).  `00452326` similarly
captures a `SkinSlider` only over its current thumb and uses that capture for
the hot frame.  `WM_CAPTURECHANGED`/`WM_CANCELMODE` clear the per-control state,
so crossing controls cannot leave a stale hot or pressed bitmap behind.

The aggregate parent state must not assign the new button hover value before
its common transition/invalidation branch. Doing so made that branch observe
"old == new": the correct child acquired capture, but profile `0x64`, reset
`0x65`, close, and an unchecked enabled button remained on frame zero. The
recovered order is now `SetCapture` first, then compare/commit the hot state,
then invalidate the owner-painted equalizer surface, matching `0040A5E3`.

`FUN_00452FFF` and the following `FUN_004052F1` also preserve a distinction
visible in padded skins. The bitmap installs a native-sized four-frame image,
but the child ultimately receives the complete XML rectangle. A Let's Vista
profile button is therefore a `96x21` child containing a `94x19` frame copied
at client origin; it is neither reduced to `94x19` nor stretched. The parser
and frame compositor now retain those two dimensions independently.

`FUN_0040EE16` creates the main player's `tooltips_class32` with zero style and
extended style. `FUN_0040EE49` plus `FUN_004085C6` register main/equalizer child
HWNDs as `TTF_IDISHWND` callback tools. Main-player text is selected by
`FUN_0045C269` and equalizer text by `FUN_00429911`; the rebuild handles both
`TTN_GETDISPINFOA` and `TTN_GETDISPINFOW`.

The original tool registration does not set `TTF_SUBCLASS`:
`FUN_0040CE9A` forwards the original queued `MSG` through
`FUN_0040EE7F`/`TTM_RELAYEVENT` before `TranslateMessage` and
`DispatchMessage`. The rebuild now uses that same single-relay message-loop
model. Parent rectangle tools have flags zero and child-HWND tools have only
`TTF_IDISHWND`. Combining manual window-procedure relay with `TTF_SUBCLASS`
had delivered the same motion twice and caused hover timing/pop behaviour to
differ from the original. Reconstruction-only periodic `TTM_UPDATE` calls
were also removed; the pseudo-code wrapper contains no equivalent path.

The playlist toolbar is a distinct path. The native toolbar retains its
playlist-owned eight-tool tooltip, but `FUN_0047E6FC` adds the visible tools to
the player-owned shared tooltip and passes each `GetMenuStringW` result directly
rather than callback text. The rebuild preserves both parts of that ownership
model. All text comes from the EXE-local `ttpres.dll`, including dynamic
play/pause, volume/balance/surround values, playback progress, profile/reset,
and the seven playlist toolbar menu names.

The `browser` element was also restored to the main-window four-state button
set. Ghidra omitted its literal from the pseudo-C output, but the call at
`CSkinParser_ParsePlayerWindow` pushes VA `0051C438`, whose PE bytes contain
`browser\0`. It therefore no longer falls through to the unrelated open-file
command.

The preamp and ten bands are disabled while `Profile == -2`; balance and
surround remain live. Balance values are multiplied by ten before reaching the
audio engine, matching the original `-100..100` playback balance.

## Profiles, persistence, and DSP

The executable's thirteen ten-band arrays (recommended profile zero plus
twelve named categories) were recovered from the table at VA `0053B260`.
Selecting a preset replaces only the ten bands and retains
preamp. A user band change selects custom profile `-1` and copies `Current` to
`Custom`. Disable stores `ProfileLast` and selects `-2`; re-enable restores the
stored profile. Reset zeros all eleven values and selects custom unless the EQ
is disabled. Commands `0x7E2F/0x7E30` load and save the native
`ttplayer_eq/Equalizer Custom` profile shape.

`EqualizerWnd`, `EqualizerVisible`, `Profile`, `ProfileLast`, `Surround`,
`Custom`, `Current`, and the player `Balance` are now read and written in
`TTPlayer.xml`.
`Custom`/`Current` retain the native delimiter shape
`preamp:band1,band2,...,band10` rather than normalising all values to colons.
`AudioEngine::SetEqualizer` implements the observable private-message `0x7EE`
effect: the waveOut worker notices the new processor generation before its next
decoded block, updates ordinal 103 EQ values, and recreates ordinal 104 only
when surround changes. Processor objects remain owned by the decode thread.

## Verification

- Debug `ttplayer_rebuild`, `audio_recovery_tests`, and
  `equalizer_recovery_tests` build successfully as Win32 targets.
- The equalizer test covers the DLL default, TT2012, Let's Vista, Media Player
  10, and LX-iPlay packages.
- Original and rebuilt default-skin runtime captures both place the equalizer
  at `(1500,959)-(1827,1058)` and match the 327x99 bitmap/control layout.
- After native-size fill clipping, the default equalizer comparison differs in
  only 43 of 32,373 pixels, all on shaped-window corner/shadow boundaries; its
  control area is pixel-identical.
- Original and rebuilt profile popups are both 127x116 with the same six
  top-level rows. Their comparison differs in 88 of 14,732 pixels, with a
  maximum summed RGB delta of 3 (low-order rasterisation rounding).
- Runtime probes show the resource-backed main `音乐窗`, equalizer `配置文件`,
  and playlist `添加` hover popups on their corresponding controls.
- Runtime child enumeration exposes the original equalizer `SkinButton`/
  `SkinSlider` ID set. A physical `SetCursorPos`/left-button drag of band
  `0xC9` keeps capture on `SkinSlider` ID 201 before/down/move/up, changes the
  persisted value from `-2` to `+9` in both binaries, and selects Custom
  profile `-1`.
- Isolated physical-input comparisons cover balance `0x66`, surround `0x67`,
  preamp `0xC8`, and all bands `0xC9..0xD2`. All thirteen controls retain the
  same child-HWND capture, persist the same final values/profile as 5.7.9,
  produce pixel-identical tracking-status crops, and restore their normal
  playback-status crop on button release.
- Dedicated physical-pointer hover probes (no injected `WM_MOUSEMOVE` or
  `WM_MOUSEHOVER`) give both binaries `SkinButton(0x64)` capture and display a
  61x22 profile tooltip.  With the same enabled-EQ fixture, both give
  `SkinSlider(0xC9)` capture and display one value tooltip; capture and tooltip
  are gone after the pointer leaves.  The playlist-add tooltip is 37x22 in
  both binaries.
- Normal/hot/leave screen-crop hashes are now identical between the original
  and rebuild for default profile `0x64`, reset `0x65`, surround `0x67`, and
  the unchecked enabled button `0x7E2C`. TT2012 and Media Player 10 profile
  hashes also match. Let's Vista additionally matches its original `96x21`
  child rectangle, native `94x19` frame, both image hashes, and leave-state
  restoration.
- The playlist-owned companion tooltip is eight tools with style/ex-style
  `0x84000000/0x000800A0` in both binaries. Its `添加` hover is displayed by
  the player-owned shared tooltip and measures 37x22 in both runtime probes.
- `audio_recovery_tests` and `equalizer_recovery_tests` return zero; the UI
  `--smoke-test` returns zero.
- The older all-skin aggregate test still stops on the independently malformed
  user package `Skin/004941_088.skn` (`Skin.xml` is missing a required
  semicolon); the equalizer-specific regression does not hide that fixture
  error.
