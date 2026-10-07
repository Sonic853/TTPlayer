#pragma once
#include <windows.h>
#include <climits>
#include <string>
#include <string_view>
#include <stdexcept>
namespace ttpcomm::text {
inline std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > INT_MAX) throw std::length_error("text exceeds Win32 conversion limit");
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("WideCharToMultiByte failed");
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

inline std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > INT_MAX) throw std::length_error("text exceeds Win32 conversion limit");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("invalid UTF-8");
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

}
