#pragma once

#include <objidl.h>
#include <winerror.h>

namespace ttplayer::ui {

// 004894AF treats a successful drop with a zero reported effect as a source
// move as well.  Some legacy targets consume CF_HDROP but leave pdwEffect at
// zero; COPY/LINK and every cancel/error result must keep the source rows.
[[nodiscard]] constexpr bool ShouldRemovePlaylistDragSource(
    HRESULT drag_result, DWORD effect) noexcept {
    return drag_result == DRAGDROP_S_DROP &&
        (effect == DROPEFFECT_NONE || (effect & DROPEFFECT_MOVE) != 0);
}

} // namespace ttplayer::ui
