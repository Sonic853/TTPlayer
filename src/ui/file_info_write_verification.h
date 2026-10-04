#pragma once

#include "file_info_probe_protocol.h"
#include "ttplayer/core/text.h"

#include <algorithm>
#include <string_view>

namespace ttplayer::ui::detail {

inline std::wstring FileInfoSaveErrorText(std::wstring_view prefix, HRESULT error) {
    wchar_t description[512]{};
    const DWORD code = HRESULT_FACILITY(error) == FACILITY_WIN32
        ? HRESULT_CODE(error) : static_cast<DWORD>(error);
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, description, static_cast<DWORD>(std::size(description)), nullptr);
    std::wstring message(prefix);
    std::wstring reason(description);
    while (!reason.empty() && (reason.back() == L'\r' || reason.back() == L'\n' || reason.back() == L' '))
        reason.pop_back();
    if (!reason.empty()) message += L"\n\n" + reason;
    wchar_t number[32]{};
    swprintf_s(number, L" (0x%08lX)", static_cast<unsigned long>(error));
    return message + number;
}

inline bool FileInfoTagNameEqual(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        const auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; };
        if (lower(left[i]) != lower(right[i])) return false;
    }
    return true;
}

inline std::wstring_view FileInfoMpegTagName(std::wstring_view name) {
    if (FileInfoTagNameEqual(name, L"Orchestra") || FileInfoTagNameEqual(name, L"Album Artist")) return L"AlbumArtist";
    if (FileInfoTagNameEqual(name, L"Lyric")) return L"Lyrics";
    if (FileInfoTagNameEqual(name, L"Author")) return L"Artist";
    if (FileInfoTagNameEqual(name, L"Track")) return L"Tracknumber";
    if (FileInfoTagNameEqual(name, L"Year")) return L"Date";
    return name;
}

// Run only after the writer has released every reference and a fresh reader
// has opened the file.  SetValue success alone is not a commit acknowledgement.
inline void VerifyFileInfoWrite(const FileInfoProbeWriteRequest& request,
                                const FileInfoProbeReadResult& actual,
                                FileInfoProbeWriteResult& result,
                                bool mpeg_aliases = false) {
    const HRESULT mismatch = FAILED(actual.status) ? actual.status
        : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    const auto fail = [&](HRESULT& status) {
        status = mismatch;
        if (SUCCEEDED(result.status)) result.status = mismatch;
    };
    if (result.fields.size() != request.fields.size()) {
        result.status = E_UNEXPECTED;
        result.fields.resize(request.fields.size(), E_UNEXPECTED);
    }
    for (size_t i = 0; i < request.fields.size(); ++i) {
        if (FAILED(result.fields[i])) {
            if (SUCCEEDED(result.status)) result.status = result.fields[i];
            continue;
        }
        if (FAILED(actual.status)) { fail(result.fields[i]); continue; }
        const auto name = core::Utf8ToWide(request.fields[i].name);
        const auto expected_name = mpeg_aliases ? FileInfoMpegTagName(name) : std::wstring_view(name);
        std::wstring_view value;
        bool conflicting{};
        for (const auto& entry : actual.metadata) {
            const auto key = mpeg_aliases ? FileInfoMpegTagName(entry.name) : std::wstring_view(entry.name);
            if (!FileInfoTagNameEqual(key, expected_name)) continue;
            value = entry.value;
            conflicting = conflicting || value != request.fields[i].value;
        }
        if (conflicting || value != request.fields[i].value) fail(result.fields[i]);
    }
    if (request.cover_action != FileInfoProbeCoverAction::unchanged && SUCCEEDED(result.cover_status)) {
        if (FAILED(actual.status) ||
            (request.cover_action == FileInfoProbeCoverAction::remove
                ? !actual.cover.empty() : actual.cover != request.cover)) fail(result.cover_status);
    }
}

} // namespace ttplayer::ui::detail
