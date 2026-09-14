# Association fallback icons

Extracted without re-encoding from the repository's original **5.7.9**
`TTPlayer.exe` RT_GROUP_ICON and RT_ICON resources. No TTPlayer6120 reference.

| File | Original group ID | ExtractIconEx index | SHA-256 |
| --- | --- | --- | --- |
| AudioFile.ico | 300 | 1 | 19CEE71769B018ACFED4A0F5B038B6146C4D98004EC3AB88E8C37BFD1D4F8898 |
| PlaylistFile.ico | 301 | 2 | 1641427C0DF0FEA853FC1AD3514D5884BC762F0AED0E1D0557D9CCDC0F0C4119 |

`00491213` constructs `"%s",1` and `"%s",2` (format strings at `0051E548`
and `0051E538`). `0049D2FA` uses these when no per-extension icon is available.
`resources.rc` retains group IDs 128, 300, 301 in index order 0, 1, 2.
All original size/depth frames are retained; there is no runtime dependency on
the original executable. Existing `Icons/<extension>.ico` files remain preferred
when no previously saved icon location exists.

The resource extraction copies each 14-byte GRPICONDIRENTRY to an ICO directory
entry: retain its first 8 bytes, record the RT_ICON payload size and file offset
instead of the resource ID, then append the unchanged RT_ICON bytes.
`options_drawing_tests` compares extracted large-icon pixels directly with the
original executable and exercises the selection-driven preview.
