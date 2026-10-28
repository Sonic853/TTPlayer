# TTPlayerOSU host ABI

`include/ttp_osu/download_page.h` defines independent download-page ABI 2,
exported as `ttp_osu_download_page_query` at ordinal 6. ABI 1 queries retain
only the original page/OSZ recognizer prefix. ABI 2 appends a provider script
and an exact-origin audio URL recognizer. The host tries 2 then 1, copies the
script, and bounds-checks each optional browser callback. Only a configured
osu!/lazer executable passing the plugin installation check enables the page.
The script intercepts downloads and explicit detail playback; it does not
expose the legacy player object or mute the browser. All messages are checked
again against the active page and native recognizers before queue submission.

Source: https://github.com/TTPlayerRebuild/TTPlayerOSU

`include/ttp_osu/abi.h` is copied from the independent plugin's ABI 1 header.
`include/ttp_osu/visual.h` defines the independent visual ABI 5, exported as
`ttp_osu_query_visual` at ordinal 2. OSZ ordinal 1 remains unchanged.
`include/ttp_osu/library.h` defines library ABI 1, exported as
`ttp_osu_library_query` at ordinal 3. Keep all SDK headers identical to the
plugin copies. The player dynamically loads
`AddIn/ttp_osu.dll` and queries the versioned function table.
ABI 2 appends the logo setter. ABI 1 queries still write only the old prefix;
older visual plugins retain fullscreen support without the logo selector.
ABI 3 appends `transition` and `transition_state`. Query 5, then 4, 3, 2, and 1;
check the returned size before reading appended callbacks. The original welcome
and departure use the same steady millisecond clock as `update`, independent of
track position. These options default off and require both callbacks. The host
keeps the full-screen surface alive until departure completes, then releases
the entire session on its window thread. Forced closure must still clean up
immediately. No animation code, welcome image or game audio lives in the host.

ABI 4 appends `style`: 0 selects the existing stable scene, 1 selects lazer.
Only a complete ABI 4 table with that callback enables fullscreen type 8,
“星环lazer”. Existing DLLs retain type 7 and their previous capabilities.
Lazer entrance/departure settings default off and persist separately from the
stable settings. Badge selection and custom-image paths now persist separately;
flags, colours and BPM fallback remain shared.
Explicitly saved enabled settings remain enabled when loading existing profiles.
Both scenes remain fullscreen-only. The DLL reports transition completion;
the host imposes no stable-specific duration and continues updates after Stop.

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
The size-versioned frame tail optionally includes left-button state while the
pointer is inside the actual canvas. The DLL owns press/release visual feedback;
the player's existing click-to-switch and double-click policies remain separate.
Only the target HWND thread may present or detach the D3D9 device. The player
exposes this renderer only in fullscreen/combined fullscreen. CPU paint remains
available for systems without a D3D9 device and for private comparisons.

ABI 5 appends `intro_text(instance, first, second, third)`. Three non-null
UTF-16 strings (including empty strings) configure the lazer intro. Each is
limited to 64 code units and copied synchronously; partial null input and
overlong input fail without replacing the current text. Three null pointers
restore automatic logo-based wording. This does not enable/restart transitions
or change stable's welcome image. Original Torus metrics render Latin text;
a cached GDI atlas supplies other Unicode glyphs using installed system fonts.
The host sends its selected automatic phrase before asynchronous artwork loads
complete, so a delayed archive read cannot show the previous logo's wording.

Use `TTP_OSU_VISUAL_ABI4_SIZE`, not `sizeof(TtpOsuVisualApi)`, to validate an
ABI 4 table. The host hides custom text for older plugins. Per-effect FPS is a
host scheduling preference (10..240), separate from the DLL ABI; two star-ring
settings default to 60 and are not overwritten by skin/visual colour presets.

Library enumeration reads stable `.osu` directories and lazer's `client.realm`
and hash store inside the optional DLL. Call it on a worker thread, copy borrowed
record strings synchronously, honor cancellation, and only publish a complete
successful result. No Realm objects cross the ABI. A missing library export in
an older DLL leaves the two list commands disabled; a missing DLL hides the
commands and osu! options page entirely. Source audio and database are read-only.

Installation ABI 1 (`installation.h`, `ttp_osu_installation_query`, ordinal 4)
owns executable identification, default locations and stable/lazer storage
configuration. Query it independently; older DLLs can still support archives
and library enumeration. No PE identity or game storage parser is linked into
the player. Copy callback strings before returning; NULL requests discovery,
whereas an empty executable preserves an explicitly cleared path. The player
hides star effects until either configured executable is recognized, and asks
to update an older DLL that lacks this interface. Unavailable defaults must not
be serialized as deliberately empty user settings.

Export ABI 1 (`export.h`, `ttp_osu_export_query`, ordinal 5) exports complete
stable/lazer beatmap sets. Pass the library root and selected track identities;
the DLL deduplicates sets, restores lazer's original filenames, and writes
streaming ZIP/ZIP64 OSZ files. Single-file mode optionally replaces a destination
after the host save dialog confirms; directory mode uses numbered collision
names without overwriting. Progress/cancel and completed-path callbacks are
synchronous and borrowed. Run on a worker thread. A failure/cancel leaves any
already completed sets intact and removes the current temporary archive. Source
directories and the Realm database stay read-only. Older DLLs retain their
existing capabilities; the host hides only the new export menu entries.
