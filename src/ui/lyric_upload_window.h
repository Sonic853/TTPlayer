#pragma once
#include <windows.h>
#include <oaidl.h>
#include <string>
#include <string_view>

namespace ttplayer::ui {
inline constexpr wchar_t kLyricUploadWindowClass[] = L"TTPlayerRebuild.LyricUpload";
struct LyricUploadData { std::wstring artist, title, album, lyrics; };
std::wstring LyricUploadUrl(std::wstring_view search_url);
// The original opens a browser FORM, not the lyric-search DLL's download
// interface. Populating these controls must never submit the form itself.
bool PopulateLyricUploadDocument(IDispatch* document, const LyricUploadData& data);
HWND ShowLyricUploadWindow(HWND owner, std::wstring_view caption,
    const LyricUploadData& data, std::wstring_view page);
bool TranslateLyricUploadMessage(const MSG& message);
}
