#pragma once

#include <windows.h>

namespace ttplayer::skin {
// The historical defaults in 00401E96 are used only when a DLL field cannot
// be read. Normal initialization overlays these with ZIP/<DEFAULT_SKIN>.
struct PlaylistColors {
    COLORREF text_color{RGB(84, 142, 165)};
    COLORREF highlight_color{RGB(212, 245, 255)};
    COLORREF number_color{RGB(69, 133, 158)};
    COLORREF duration_color{RGB(69, 133, 158)};
    COLORREF selected_color{RGB(132, 206, 249)};
    COLORREF background_color{RGB(29, 56, 64)};
    COLORREF alternate_background_color{RGB(24, 51, 60)};
};
struct LyricColors {
    COLORREF text_color{RGB(84, 142, 165)};
    COLORREF highlight_color{RGB(212, 245, 255)};
    COLORREF background_color{RGB(24, 51, 60)};
};
struct VisualColors {
    COLORREF spectrum_top_color{RGB(25, 77, 92)};
    COLORREF spectrum_bottom_color{RGB(25, 77, 92)};
    COLORREF spectrum_middle_color{RGB(25, 77, 92)};
    COLORREF spectrum_peak_color{RGB(25, 77, 92)};
    COLORREF blur_scope_color{RGB(25, 77, 92)};
    COLORREF text_color{RGB(255, 255, 255)};
};
struct SkinColors {
    PlaylistColors playlist;
    LyricColors lyric;
    VisualColors visual;
};

// Reads only the three color XML entries, in memory. It does not materialize
// a LegacySkin or read user profiles. Missing/invalid fields retain the
// historical defaults above (Color_Select uses the original system-color
// exception when its PlayList node exists); an invalid ZIP resource throws.
SkinColors ReadDefaultSkinColors(HMODULE resources, HMODULE ttpcomm);

// A process-wide immutable snapshot from EXE-local ttpres.dll / ttpcomm.dll.
// Settings, skin layouts and Reset All share this baseline. No CWD lookup,
// temporary extraction, or dependency on when the first HWND is created.
const SkinColors& DefaultSkinColors() noexcept;
} // namespace ttplayer::skin
