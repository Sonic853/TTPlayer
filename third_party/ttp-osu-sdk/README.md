# TTPlayerOSU host ABI

Source: https://github.com/TTPlayerRebuild/TTPlayerOSU

`include/ttp_osu/abi.h` is copied from the independent plugin's ABI 1 header.
Keep the two headers identical. The player dynamically loads
`AddIn/ttp_osu.dll` and queries the versioned function table.

The plugin receives an **OSZ archive path** and orchestrates enumeration,
metadata reads, audio selection and per-track background selection. Host
callbacks supply generic, read-only archive I/O and preserve CRC verification,
password interaction, cancellation and caching. No OSZ parser is linked into
the player, and standalone `.osu` files are not audio inputs.

All calls are synchronous. Do not retain callback pointers or borrowed buffers.
See the canonical header and the plugin's implementation document for limits.
