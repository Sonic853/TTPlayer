#pragma once
#include <cstddef>
#include <cstdint>

namespace ttpcomm::bytes {
inline bool Contains(size_t size, size_t offset, size_t length) noexcept {
    return offset <= size && length <= size - offset;
}
inline uint16_t Le16(const unsigned char* p) noexcept {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t Le32(const unsigned char* p) noexcept {
    return uint32_t(Le16(p)) | uint32_t(Le16(p + 2)) << 16;
}
inline uint32_t Be32(const unsigned char* p) noexcept {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint32_t Synchsafe32(const unsigned char* p) noexcept {
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80) return UINT32_MAX;
    return uint32_t(p[0]) << 21 | uint32_t(p[1]) << 14 | uint32_t(p[2]) << 7 | p[3];
}
// Borrowed view: it never owns storage and cannot cross a DLL ABI boundary.
struct View {
    const unsigned char* data_{};
    size_t size_{};
    View() = default;
    View(const unsigned char* data, size_t size) : data_(data), size_(size) {}
    const unsigned char* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return !size_; }
    const unsigned char* begin() const noexcept { return data_; }
    const unsigned char* end() const noexcept { return size_ ? data_ + size_ : data_; }
    unsigned char operator[](size_t index) const noexcept { return data_[index]; }
    View subspan(size_t offset) const noexcept { return {offset ? data_ + offset : data_, size_ - offset}; }
};
}
