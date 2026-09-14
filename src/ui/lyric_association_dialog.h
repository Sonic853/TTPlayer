#pragma once
#include "ttplayer/lyrics/association.h"
#include "ttplayer/lyrics/local_search.h"
#include <functional>
#include <windows.h>

namespace ttplayer::ui::detail {
void InstallLyricAssociationImages(HWND dialog, HMODULE resources, bool all_associations);
struct LyricAssociationChoice {
    INT_PTR action{IDCANCEL}; // 1 associate, 0x844 remove, 7 block, 6 unblock.
    std::filesystem::path path;
};
LyricAssociationChoice ChooseLyricAssociation(HMODULE resources, HWND owner,
    lyrics::AssociationStore& store, const lyrics::SongKey& song,
    lyrics::LocalSearchRequest request, std::function<void()> associations_changed);
} // namespace ttplayer::ui::detail
