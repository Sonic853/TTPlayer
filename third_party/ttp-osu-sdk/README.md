# TTPlayerOSU host ABI

Source: https://github.com/TTPlayerRebuild/TTPlayerOSU

`include/ttp_osu/abi.h` is copied from the independent plugin's ABI 1 header.
`include/ttp_osu/visual.h` defines the independent visual ABI 3, exported as
`ttp_osu_query_visual` at ordinal 2. OSZ ordinal 1 remains unchanged.
Keep the two headers identical. The player dynamically loads
`AddIn/ttp_osu.dll` and queries the versioned function table.
ABI 2 appends the logo setter. ABI 1 queries still write only the old prefix;
older visual plugins retain fullscreen support without the logo selector.
ABI 3 appends `transition` and `transition_state`. Query 3, then 2, then 1;
check the returned size before reading appended callbacks. The original welcome
and departure use the same steady millisecond clock as `update`, independent of
track position. These options default off and require both callbacks. The host
keeps the full-screen surface alive until departure completes, then releases
the entire session on its window thread. Forced closure must still clean up
immediately. No animation code, welcome image or game audio lives in the host.

The plugin receives an **OSZ archive path** and orchestrates enumeration,
metadata reads, audio selection and per-track background selection. Host
callbacks supply generic, read-only archive I/O and preserve CRC verification,
password interaction, cancellation and caching. No OSZ parser is linked into
the player, and standalone `.osu` files are not audio inputs.

All calls are synchronous. Do not retain callback pointers or borrowed buffers.
See the canonical header and the plugin's implementation document for limits.

The visual DLL owns FFT2048, timing/Kiai interpretation, scene animation and
CPU/D3D9 drawing. `OsuVisualClient` supplies continuous PCM with its actual
playback cursor, a clock, pointer coordinates, generic archive reads and decoded
PBGRA artwork. Low-latency output may use bounded PCM history: `pcm_cursor_offset`
separates the output cursor from the FFT window start. Never splice UI samples.
Only the target HWND thread may present or detach the D3D9 device. The player
exposes this renderer only in fullscreen/combined fullscreen. CPU paint remains
available for systems without a D3D9 device and for private comparisons.
