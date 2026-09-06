#include "player_window_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <olectl.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windowsx.h>

namespace ttplayer::ui {
using namespace detail;
namespace {

constexpr size_t kAnalysisSamples = 512;
constexpr uint32_t kMaximumPictureBytes = 64U * 1024U * 1024U;

bool LayeredWindowsAvailable() noexcept {
    static const bool available = [] {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 && GetProcAddress(
            user32, "SetLayeredWindowAttributes") != nullptr;
    }();
    return available;
}

RECT WindowRectForClientTarget(HWND window, RECT target) noexcept {
    // FUN_0044E090/FUN_00467735 receive a desired client rectangle.  Their
    // SetWindowPos rectangle includes any non-client margins which remain on
    // the control after it is detached.
    RECT window_rect{};
    RECT client_rect{};
    GetWindowRect(window, &window_rect);
    GetClientRect(window, &client_rect);
    POINT client_top_left{client_rect.left, client_rect.top};
    POINT client_bottom_right{client_rect.right, client_rect.bottom};
    if (ClientToScreen(window, &client_top_left))
        ClientToScreen(window, &client_bottom_right);
    target.left += window_rect.left - client_top_left.x;
    target.top += window_rect.top - client_top_left.y;
    target.right += window_rect.right - client_bottom_right.x;
    target.bottom += window_rect.bottom - client_bottom_right.y;
    return target;
}

uint32_t BigEndian32(const unsigned char* value) noexcept {
    return (static_cast<uint32_t>(value[0]) << 24) |
           (static_cast<uint32_t>(value[1]) << 16) |
           (static_cast<uint32_t>(value[2]) << 8) |
           static_cast<uint32_t>(value[3]);
}

uint32_t SyncSafe32(const unsigned char* value) noexcept {
    return (static_cast<uint32_t>(value[0] & 0x7fU) << 21) |
           (static_cast<uint32_t>(value[1] & 0x7fU) << 14) |
           (static_cast<uint32_t>(value[2] & 0x7fU) << 7) |
           static_cast<uint32_t>(value[3] & 0x7fU);
}

bool ReadExact(std::ifstream& input, void* destination, size_t size) {
    if (size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
        return false;
    input.read(static_cast<char*>(destination),
               static_cast<std::streamsize>(size));
    return input.good() || input.gcount() == static_cast<std::streamsize>(size);
}

bool LegacyPictureMimeMatches(std::string_view mime,
                              std::span<const unsigned char> data) noexcept {
    // FUN_004AD0AD compares the MIME spelling case-sensitively.  A concrete
    // legacy MIME is trusted as-is; only the three wildcard forms take the
    // JPEG/BMP/GIF signature path.  In particular, PNG and image/pjpeg are
    // not accepted by TTPlayer 5.7.9.
    if (mime == "image/jpeg" || mime == "image/jpg" ||
        mime == "image/bmp" || mime == "image/gif") return true;
    if (!mime.empty() && mime != "image/" && mime != "image/*") return false;
    return (data.size() >= 4 && data[0] == 0xff && data[1] == 0xd8 &&
            data[data.size() - 2] == 0xff && data.back() == 0xd9) ||
           (data.size() >= 2 && data[0] == 'B' && data[1] == 'M') ||
           (data.size() >= 3 && data[0] == 'G' && data[1] == 'I' &&
            data[2] == 'F');
}

std::vector<unsigned char> ParseFlacPictureBlock(
    const std::vector<unsigned char>& block, uint32_t* picture_type) {
    size_t cursor{};
    auto take32 = [&](uint32_t& result) {
        if (cursor + 4 > block.size()) return false;
        result = BigEndian32(block.data() + cursor);
        cursor += 4;
        return true;
    };
    uint32_t type{}, mime_length{}, description_length{};
    if (!take32(type) || !take32(mime_length) ||
        mime_length > block.size() - cursor) return {};
    const std::string mime(
        reinterpret_cast<const char*>(block.data() + cursor), mime_length);
    cursor += mime_length;
    if (!take32(description_length) ||
        description_length > block.size() - cursor) return {};
    cursor += description_length;
    uint32_t width{}, height{}, depth{}, colors{};
    if (!take32(width) || !take32(height) || !take32(depth) ||
        !take32(colors)) return {};
    static_cast<void>(colors);
    // The original reader metadata path rejects malformed PICTURE records
    // before CVisualCtrl sees their byte blob.  In particular, the reference
    // FLAC carries a decodable JPEG but declares a zero-sized picture; the
    // stock 5.7.9 player deliberately shows the skin background instead.
    if (width == 0 || height == 0 || depth == 0) return {};
    uint32_t data_length{};
    if (!take32(data_length) || data_length == 0 ||
        data_length > kMaximumPictureBytes ||
        data_length > block.size() - cursor) return {};
    const std::span<const unsigned char> data(block.data() + cursor,
                                               data_length);
    if (!LegacyPictureMimeMatches(mime, data)) return {};
    if (picture_type) *picture_type = type;
    return {data.begin(), data.end()};
}

std::vector<unsigned char> ReadFlacPicture(std::ifstream& input,
                                            bool* picture_declared) {
    for (;;) {
        unsigned char header[4]{};
        if (!ReadExact(input, header, sizeof(header))) return {};
        const bool last = (header[0] & 0x80U) != 0;
        const unsigned int type = header[0] & 0x7fU;
        const uint32_t length = (static_cast<uint32_t>(header[1]) << 16) |
                                (static_cast<uint32_t>(header[2]) << 8) |
                                header[3];
        if (length > kMaximumPictureBytes) return {};
        if (type == 6) {
            if (picture_declared) *picture_declared = true;
            std::vector<unsigned char> block(length);
            if (!ReadExact(input, block.data(), block.size())) return {};
            uint32_t picture_type{};
            auto picture = ParseFlacPictureBlock(block, &picture_type);
            static_cast<void>(picture_type);
            // FUN_004AD83E always asks ISoundThumbnail for index zero.  The
            // compatibility parser used when no legacy reader owns the file
            // must likewise keep file order instead of searching for picture
            // type 3 (front cover).
            return picture;
        } else {
            input.seekg(static_cast<std::streamoff>(length), std::ios::cur);
            if (!input) return {};
        }
        if (last) return {};
    }
}

size_t SkipId3Description(std::span<const unsigned char> frame,
                          size_t cursor, unsigned char encoding) {
    if (encoding == 1 || encoding == 2) {
        while (cursor + 1 < frame.size()) {
            if (frame[cursor] == 0 && frame[cursor + 1] == 0)
                return cursor + 2;
            cursor += 2;
        }
    } else {
        while (cursor < frame.size() && frame[cursor] != 0) ++cursor;
        if (cursor < frame.size()) ++cursor;
    }
    return cursor;
}

std::vector<unsigned char> ParseId3Apic(
    std::span<const unsigned char> frame, unsigned int version,
    unsigned int* picture_type) {
    if (frame.size() < 4) return {};
    size_t cursor = 1;
    std::string mime;
    if (version == 2) {
        if (cursor + 3 > frame.size()) return {};
        const std::string_view format(
            reinterpret_cast<const char*>(frame.data() + cursor), 3);
        if (format == "JPG") mime = "image/jpeg";
        else if (format == "BMP") mime = "image/bmp";
        else if (format == "GIF") mime = "image/gif";
        else mime.assign(format);
        cursor += 3;
    } else {
        const size_t mime_begin = cursor;
        while (cursor < frame.size() && frame[cursor] != 0) ++cursor;
        if (cursor == frame.size()) return {};
        mime.assign(reinterpret_cast<const char*>(frame.data() + mime_begin),
                    cursor - mime_begin);
        ++cursor;
    }
    if (cursor >= frame.size()) return {};
    const unsigned int type = frame[cursor++];
    cursor = SkipId3Description(frame, cursor, frame[0]);
    if (cursor >= frame.size() || frame.size() - cursor > kMaximumPictureBytes)
        return {};
    const std::span<const unsigned char> data(frame.data() + cursor,
                                               frame.size() - cursor);
    if (!LegacyPictureMimeMatches(mime, data)) return {};
    if (picture_type) *picture_type = type;
    return {data.begin(), data.end()};
}

std::vector<unsigned char> ReadId3Picture(std::ifstream& input,
                                          const unsigned char header[10],
                                          bool* picture_declared) {
    const unsigned int version = header[3];
    if (version < 2 || version > 4) return {};
    const uint32_t tag_size = SyncSafe32(header + 6);
    if (tag_size == 0 || tag_size > kMaximumPictureBytes) return {};
    std::vector<unsigned char> tag(tag_size);
    if (!ReadExact(input, tag.data(), tag.size())) return {};
    size_t cursor{};
    while (cursor < tag.size()) {
        const size_t frame_header = version == 2 ? 6 : 10;
        if (cursor + frame_header > tag.size() || tag[cursor] == 0) break;
        const bool apic = version == 2
            ? std::memcmp(tag.data() + cursor, "PIC", 3) == 0
            : std::memcmp(tag.data() + cursor, "APIC", 4) == 0;
        const uint32_t frame_size = version == 2
            ? (static_cast<uint32_t>(tag[cursor + 3]) << 16) |
              (static_cast<uint32_t>(tag[cursor + 4]) << 8) |
              tag[cursor + 5]
            : (version == 4 ? SyncSafe32(tag.data() + cursor + 4)
                            : BigEndian32(tag.data() + cursor + 4));
        cursor += frame_header;
        if (frame_size == 0 || frame_size > tag.size() - cursor) break;
        if (apic) {
            if (picture_declared) *picture_declared = true;
            unsigned int picture_type{};
            auto picture = ParseId3Apic(
                std::span<const unsigned char>(tag.data() + cursor,
                                               frame_size),
                version, &picture_type);
            static_cast<void>(picture_type);
            return picture;
        }
        cursor += frame_size;
    }
    return {};
}

std::vector<unsigned char> ReadEmbeddedPicture(
    const std::filesystem::path& source, bool* picture_declared) {
    if (picture_declared) *picture_declared = false;
    std::ifstream input(source, std::ios::binary);
    if (!input) return {};
    unsigned char header[10]{};
    if (!ReadExact(input, header, sizeof(header))) return {};
    if (std::memcmp(header, "fLaC", 4) == 0) {
        input.seekg(4, std::ios::beg);
        return ReadFlacPicture(input, picture_declared);
    }
    if (std::memcmp(header, "ID3", 3) == 0)
        return ReadId3Picture(input, header, picture_declared);
    return {};
}

struct WicPicture {
    HBITMAP bitmap{};
    UINT width{};
    UINT height{};
};

WicPicture DecodePictureWithWic(const std::vector<unsigned char>& bytes) {
    WicPicture result;
    if (bytes.empty() || bytes.size() > std::numeric_limits<UINT>::max())
        return result;
    IStream* stream = SHCreateMemStream(bytes.data(),
                                        static_cast<UINT>(bytes.size()));
    if (!stream) return result;
    IWICImagingFactory* factory{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICFormatConverter* converter{};
    HRESULT status = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(status)) {
        status = factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(status)) status = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(status)) status = frame->GetSize(&result.width,
                                                   &result.height);
    if (SUCCEEDED(status) &&
        (result.width == 0 || result.height == 0 ||
         result.width > static_cast<UINT>(std::numeric_limits<LONG>::max()) ||
         result.height > static_cast<UINT>(std::numeric_limits<LONG>::max()) ||
         result.width > std::numeric_limits<UINT>::max() / 4U ||
         result.height > std::numeric_limits<UINT>::max() /
                         (result.width * 4U))) {
        status = E_INVALIDARG;
    }
    if (SUCCEEDED(status)) status = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(status)) {
        status = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);
    }
    void* pixels{};
    if (SUCCEEDED(status)) {
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = static_cast<LONG>(result.width);
        information.bmiHeader.biHeight = -static_cast<LONG>(result.height);
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        result.bitmap = CreateDIBSection(nullptr, &information,
            DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!result.bitmap || !pixels) status = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(status)) {
        const UINT stride = result.width * 4U;
        status = converter->CopyPixels(nullptr, stride,
            stride * result.height, static_cast<BYTE*>(pixels));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    stream->Release();
    if (FAILED(status)) {
        if (result.bitmap) DeleteObject(result.bitmap);
        result = {};
    }
    return result;
}

IPicture* DecodePictureWithOle(const std::vector<unsigned char>& bytes) {
    if (bytes.empty() || bytes.size() >
            static_cast<size_t>(std::numeric_limits<LONG>::max()) ||
        bytes.size() > std::numeric_limits<UINT>::max())
        return nullptr;
    IStream* stream = SHCreateMemStream(bytes.data(),
                                        static_cast<UINT>(bytes.size()));
    if (!stream) return nullptr;
    IPicture* result{};
    if (FAILED(OleLoadPicture(stream, static_cast<LONG>(bytes.size()), FALSE,
                              IID_IPicture,
                              reinterpret_cast<void**>(&result))))
        result = nullptr;
    stream->Release();
    return result;
}

COLORREF Interpolate(COLORREF first, COLORREF second, int value, int limit) {
    if (limit <= 0) return second;
    value = std::clamp(value, 0, limit);
    const auto channel = [=](BYTE left, BYTE right) {
        return static_cast<BYTE>((static_cast<int>(left) * (limit - value) +
                                  static_cast<int>(right) * value) / limit);
    };
    return RGB(channel(GetRValue(first), GetRValue(second)),
               channel(GetGValue(first), GetGValue(second)),
               channel(GetBValue(first), GetBValue(second)));
}

uint32_t DibColor(COLORREF color) noexcept {
    return (static_cast<uint32_t>(GetRValue(color)) << 16) |
           (static_cast<uint32_t>(GetGValue(color)) << 8) |
           static_cast<uint32_t>(GetBValue(color));
}

uint32_t BlendDibColor(uint32_t background, COLORREF foreground,
                       unsigned int foreground_parts) noexcept {
    foreground_parts = std::min(foreground_parts, 4U);
    const unsigned int background_parts = 4U - foreground_parts;
    const uint32_t source = DibColor(foreground);
    const auto channel = [&](unsigned int shift) {
        return ((((background >> shift) & 0xffU) * background_parts) +
                (((source >> shift) & 0xffU) * foreground_parts)) / 4U;
    };
    return (background & 0xff000000U) | (channel(16) << 16) |
           (channel(8) << 8) | channel(0);
}

LOGFONTW DefaultVisualFont() {
    LOGFONTW font{};
    font.lfHeight = -12;
    font.lfWeight = FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfOutPrecision = OUT_DEFAULT_PRECIS;
    font.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    font.lfQuality = DEFAULT_QUALITY;
    font.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(font.lfFaceName, L"Microsoft YaHei UI");
    return font;
}

using DreamCreate = void* (__fastcall*)(uint32_t, int);
using DreamResize = void (__fastcall*)(void*, uint32_t, int);
using DreamProcess = const uint32_t* (__fastcall*)(
    void*, const int16_t*, const int16_t*, int);
using DreamDestroy = void (__fastcall*)(void*);
using SpectrumFactory = void* (__stdcall*)();
using SpectrumProcess = void (__thiscall*)(void*, int16_t*, int);
using SpectrumDestroy = void (__thiscall*)(void*, unsigned char);

#if defined(_MSC_VER) && defined(_M_IX86)
void* SafeDreamCreate(DreamCreate function, uint32_t width, int height) noexcept {
    __try { return function ? function(width, height) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool SafeDreamResize(DreamResize function, void* object,
                     uint32_t width, int height) noexcept {
    __try {
        if (!function || !object) return false;
        function(object, width, height);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const uint32_t* SafeDreamProcess(DreamProcess function, void* object,
                                const int16_t* left, const int16_t* right) noexcept {
    __try {
        return function && object ? function(object, left, right, 0) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void SafeDreamDestroy(DreamDestroy function, void* object) noexcept {
    __try { if (function && object) function(object); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SafeDreamClear(const uint32_t* pixels, size_t count) noexcept {
    if (!pixels || count == 0) return;
    __try {
        std::memset(const_cast<uint32_t*>(pixels), 0,
                    count * sizeof(uint32_t));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* SafeSpectrumCreate(SpectrumFactory function) noexcept {
    __try { return function ? function() : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool SafeSpectrumProcess(void* object, int16_t* buffer, int mode) noexcept {
    __try {
        if (!object || !buffer) return false;
        auto** table = *reinterpret_cast<void***>(object);
        if (!table || !table[1]) return false;
        reinterpret_cast<SpectrumProcess>(table[1])(object, buffer, mode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SafeSpectrumDestroy(void* object) noexcept {
    __try {
        if (!object) return;
        auto** table = *reinterpret_cast<void***>(object);
        if (table && table[0])
            reinterpret_cast<SpectrumDestroy>(table[0])(object, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
#else
void* SafeDreamCreate(DreamCreate, uint32_t, int) noexcept { return nullptr; }
bool SafeDreamResize(DreamResize, void*, uint32_t, int) noexcept { return false; }
const uint32_t* SafeDreamProcess(DreamProcess, void*, const int16_t*,
                                const int16_t*) noexcept { return nullptr; }
void SafeDreamDestroy(DreamDestroy, void*) noexcept {}
void SafeDreamClear(const uint32_t*, size_t) noexcept {}
void* SafeSpectrumCreate(SpectrumFactory) noexcept { return nullptr; }
bool SafeSpectrumProcess(void*, int16_t*, int) noexcept { return false; }
void SafeSpectrumDestroy(void*) noexcept {}
#endif

} // namespace

// Runtime counterpart of the original CVisualCtrl object at CPlayerWnd+0x438.
// It deliberately owns only renderer state; the child HWND and settings remain
// in PlayerWindow, matching the original lifetime split.
class VisualRuntime {
public:
    ~VisualRuntime() {
        DestroyDream();
        DestroySpectrumProcessor();
        DestroySurface();
        if (cover_bitmap_) DeleteObject(cover_bitmap_);
        if (cover_) cover_->Release();
    }

    void SetModule(HMODULE module) {
        std::scoped_lock lock(mutex_);
        if (module_ == module) return;
        DestroyDream();
        DestroySpectrumProcessor();
        module_ = module;
        create_ = module ? reinterpret_cast<DreamCreate>(
            GetProcAddress(module, MAKEINTRESOURCEA(90))) : nullptr;
        resize_ = module ? reinterpret_cast<DreamResize>(
            GetProcAddress(module, MAKEINTRESOURCEA(91))) : nullptr;
        process_ = module ? reinterpret_cast<DreamProcess>(
            GetProcAddress(module, MAKEINTRESOURCEA(92))) : nullptr;
        destroy_ = module ? reinterpret_cast<DreamDestroy>(
            GetProcAddress(module, MAKEINTRESOURCEA(93))) : nullptr;
        spectrum_factory_ = module ? reinterpret_cast<SpectrumFactory>(
            GetProcAddress(module, MAKEINTRESOURCEA(105))) : nullptr;
        EnsureDream();
        EnsureSpectrumProcessor();
    }

    void Configure(const settings::VisualSettings& settings, SIZE size,
                   bool full_screen = false, HBITMAP background = nullptr,
                   const RECT* background_bounds = nullptr) {
        std::scoped_lock lock(mutex_);
        output_width_ = std::max(0, static_cast<int>(size.cx));
        output_height_ = std::max(0, static_cast<int>(size.cy));
        // FUN_0045775A applies the 320x240/240x180/512x384 working-size caps
        // only when CVisualCtrl+0x24 contains a non-empty full-screen target.
        // The main player's embedded control deliberately leaves that RECT
        // empty and renders at the visual element's exact skin dimensions.
        int next_width = output_width_;
        int next_height = output_height_;
        if (full_screen) {
            switch (settings.type) {
            case 1:
                next_width = std::min(next_width, 320);
                next_height = std::min(next_height, 240);
                break;
            case 2:
                next_width = std::min(next_width, 240);
                next_height = std::min(next_height, 180);
                break;
            case 3:
                next_width = std::min(next_width, 512);
                next_height = std::min(next_height, 384);
                break;
            default:
                break;
            }
        }
        const bool geometry_changed = next_width != width_ ||
                                      next_height != height_;
        settings_ = settings;
        settings_.type = std::clamp(settings_.type, 0, 4);
        settings_.frames_per_second = std::clamp(
            settings_.frames_per_second, 0, 100);
        settings_.blur_speed = std::clamp(settings_.blur_speed, 0, 255);
        width_ = next_width;
        height_ = next_height;
        full_screen_ = full_screen;
        // FUN_0045775A reinitializes the active renderer on every call, even
        // when mode and dimensions are unchanged (for example, same-sized
        // skin changes while paused).  Old bars, scope trails or dream bits
        // must never survive that configuration boundary.
        spectrum_.assign(static_cast<size_t>(width_), int16_t{});
        peaks_.assign(static_cast<size_t>(width_), int16_t{});
        peak_speed_.assign(static_cast<size_t>(width_), int16_t{});
        previous_peaks_.assign(static_cast<size_t>(width_), int16_t{});
        scope_.assign(static_cast<size_t>(width_ + 2) * (height_ + 2), 0);
        have_dynamic_frame_ = false;
        dream_bits_ = nullptr;
        if (geometry_changed || !surface_dc_ || !surface_bits_)
            RecreateSurface();
        CacheBackground(background, background_bounds);
        if (settings_.type == 1) EnsureDream(true);
        else DestroyDream();
        if (settings_.type == 2) EnsureSpectrumProcessor();
        else DestroySpectrumProcessor();
        // Both branches of FUN_0045775A clear the raw picture buffer, decoded
        // IPicture and prompt CString.  Only CPlayerWnd::Run in playing state
        // may repopulate Type 4 after any mode/geometry configuration.
        source_.clear();
        fallback_.clear();
        thumbnail_interface_ = false;
        thumbnail_.clear();
        cover_payload_present_ = false;
        fallback_bounds_ = {};
        if (cover_bitmap_) {
            DeleteObject(cover_bitmap_);
            cover_bitmap_ = nullptr;
            cover_bitmap_size_ = {};
        }
        if (cover_) {
            cover_->Release();
            cover_ = nullptr;
        }
    }

    void SetSource(const std::filesystem::path& source,
                   std::wstring fallback,
                   const audio::AudioMetadata& metadata) {
        std::scoped_lock lock(mutex_);
        // With a reader-owned ISoundThumbnail whose first entry produces no
        // bytes, the original worker leaves Type 4 as the bare skin panel:
        // it never initializes the 0x821B prompt/hit rectangle.  This is
        // distinct from a reader without that interface, for which the
        // compatibility metadata path may still yield a picture or prompt.
        fallback_ = metadata.thumbnail_interface && metadata.thumbnail.empty()
            ? std::wstring{} : std::move(fallback);
        const bool metadata_changed =
            thumbnail_interface_ != metadata.thumbnail_interface ||
            thumbnail_ != metadata.thumbnail;
        if (source_ == source && !metadata_changed) return;
        source_ = source;
        thumbnail_interface_ = metadata.thumbnail_interface;
        thumbnail_ = metadata.thumbnail;
        if (settings_.type == 4) LoadCover();
    }

    void ClearDynamic() {
        std::scoped_lock lock(mutex_);
        // FUN_004570AE/FUN_00457599 restore the cached background into the
        // output DIB, but retain the spectrum's four-short state and the
        // scope blur mask.  A following track therefore resumes their decay
        // rather than starting those two renderers from zero.
        // FUN_004569C3 clears the DrawDib-sized pixel buffer returned by
        // ordinal 92 while retaining the dream object for the next track.
        SafeDreamClear(dream_bits_, static_cast<size_t>(
            std::max(0, dream_width_)) * std::max(0, dream_height_));
        // Unlike reconfiguration, the stop routine does not clear +0x42C.
        // Its subsequent WM_PAINT therefore draws that zeroed (black) dream
        // bitmap; spectrum and scope instead expose their restored backing.
        have_dynamic_frame_ = settings_.type == 1 && dream_bits_ != nullptr;
    }

    void ClearPlayback() {
        std::scoped_lock lock(mutex_);
        ClearDynamic();
        source_.clear();
        // FUN_00457BCF releases the decoded picture and calls FUN_00403A2B on
        // CVisualCtrl+0x1588, clearing the 0x821B prompt as well. Spectrum,
        // scope and cover expose the backing; a prior dream frame is the
        // special case documented in ClearDynamic (its zeroed DIB remains).
        fallback_.clear();
        thumbnail_interface_ = false;
        thumbnail_.clear();
        cover_payload_present_ = false;
        fallback_bounds_ = {};
        if (cover_bitmap_) {
            DeleteObject(cover_bitmap_);
            cover_bitmap_ = nullptr;
            cover_bitmap_size_ = {};
        }
        if (cover_) {
            cover_->Release();
            cover_ = nullptr;
        }
    }

    void Update(const audio::VisualizationSamples& samples) {
        std::scoped_lock lock(mutex_);
        if (width_ <= 0 || height_ <= 0) return;
        // FUN_004ABBDC asks the reader for exactly 0x200 samples per channel
        // and refuses a short block.  The previous frame is retained until a
        // complete analysis block is available.
        if (samples.count < audio::VisualizationSamples::capacity) return;
        // FUN_004ABBDC writes into one persistent 0xC00-byte scratch block.
        // Every visual mode refreshes the first 512 shorts, while only the
        // stereo dream mode refreshes the second 512; ordinal 105 owns the
        // final 512 spectrum slots.  Keeping this block persistent matters
        // for unusually wide embedded scope controls after a mode switch.
        std::copy_n(samples.left.begin(), kAnalysisSamples,
                    analysis_work_.begin());
        if (settings_.type == 1) {
            std::copy_n(samples.right.begin(), kAnalysisSamples,
                        analysis_work_.begin() + kAnalysisSamples);
        }
        switch (settings_.type) {
        case 1: UpdateDream(samples); break;
        case 2: UpdateSpectrum(samples); break;
        case 3: UpdateScope(samples); break;
        default: break;
        }
    }

    void Paint(HDC dc, const RECT& bounds) {
        std::scoped_lock lock(mutex_);
        if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            return;
        switch (settings_.type) {
        case 1:
            PaintCachedBackground(dc, bounds);
            PaintDream(dc, bounds);
            break;
        case 2:
            if (have_dynamic_frame_) PaintSpectrum(dc, bounds);
            else PaintCachedBackground(dc, bounds);
            break;
        case 3:
            if (have_dynamic_frame_) PaintScope(dc, bounds);
            else PaintCachedBackground(dc, bounds);
            break;
        case 4:
            PaintCover(dc, bounds);
            break;
        default:
            PaintCachedBackground(dc, bounds);
            break;
        }
    }

    void PaintBackgroundOnly(HDC dc, const RECT& bounds) {
        std::scoped_lock lock(mutex_);
        if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            return;
        PaintCachedBackground(dc, bounds);
    }

    [[nodiscard]] bool FallbackHit(POINT point) const noexcept {
        std::scoped_lock lock(mutex_);
        return settings_.type == 4 && !cover_payload_present_ &&
               !fallback_.empty() &&
               PtInRect(&fallback_bounds_, point) != FALSE;
    }

private:
    void DestroySurface() noexcept {
        if (surface_dc_ && surface_old_bitmap_)
            SelectObject(surface_dc_, surface_old_bitmap_);
        surface_old_bitmap_ = nullptr;
        if (surface_bitmap_) DeleteObject(surface_bitmap_);
        surface_bitmap_ = nullptr;
        surface_bits_ = nullptr;
        if (surface_dc_) DeleteDC(surface_dc_);
        surface_dc_ = nullptr;
    }

    void RecreateSurface() {
        DestroySurface();
        if (width_ <= 0 || height_ <= 0) return;
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = width_;
        information.bmiHeader.biHeight = -height_;
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        void* pixels{};
        surface_bitmap_ = CreateDIBSection(
            nullptr, &information, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!surface_bitmap_ || !pixels) {
            if (surface_bitmap_) DeleteObject(surface_bitmap_);
            surface_bitmap_ = nullptr;
            return;
        }
        surface_dc_ = CreateCompatibleDC(nullptr);
        if (!surface_dc_) {
            DeleteObject(surface_bitmap_);
            surface_bitmap_ = nullptr;
            return;
        }
        surface_old_bitmap_ = SelectObject(surface_dc_, surface_bitmap_);
        surface_bits_ = static_cast<uint32_t*>(pixels);
    }

    void CacheBackground(HBITMAP bitmap, const RECT* source_bounds) {
        if (!surface_dc_ || !surface_bits_ || width_ <= 0 || height_ <= 0)
            return;
        std::fill_n(surface_bits_, static_cast<size_t>(width_) * height_, 0U);
        if (bitmap && source_bounds) {
            const HDC source = CreateCompatibleDC(surface_dc_);
            if (source) {
                const HGDIOBJ old = SelectObject(source, bitmap);
                const int source_width = source_bounds->right -
                                         source_bounds->left;
                const int source_height = source_bounds->bottom -
                                          source_bounds->top;
                if (source_width == width_ && source_height == height_) {
                    BitBlt(surface_dc_, 0, 0, width_, height_, source,
                           source_bounds->left, source_bounds->top, SRCCOPY);
                } else if (source_width > 0 && source_height > 0) {
                    const int previous = SetStretchBltMode(surface_dc_,
                                                           COLORONCOLOR);
                    StretchBlt(surface_dc_, 0, 0, width_, height_, source,
                               source_bounds->left, source_bounds->top,
                               source_width, source_height, SRCCOPY);
                    SetStretchBltMode(surface_dc_, previous);
                }
                SelectObject(source, old);
                DeleteDC(source);
            }
        }
        background_pixels_.assign(
            surface_bits_, surface_bits_ + static_cast<size_t>(width_) * height_);
    }

    bool RestoreBackground() {
        const size_t pixels = static_cast<size_t>(width_) * height_;
        if (!surface_bits_ || background_pixels_.size() != pixels) return false;
        std::copy(background_pixels_.begin(), background_pixels_.end(),
                  surface_bits_);
        return true;
    }

    void PaintCachedBackground(HDC dc, const RECT& bounds) {
        if (!RestoreBackground()) return;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchBlt(dc, bounds.left, bounds.top,
                   bounds.right - bounds.left, bounds.bottom - bounds.top,
                   surface_dc_, 0, 0, width_, height_, SRCCOPY);
    }

    void DestroyDream() noexcept {
        if (dream_) SafeDreamDestroy(destroy_, dream_);
        dream_ = nullptr;
        dream_bits_ = nullptr;
        dream_width_ = 0;
        dream_height_ = 0;
    }

    void DestroySpectrumProcessor() noexcept {
        SafeSpectrumDestroy(spectrum_processor_);
        spectrum_processor_ = nullptr;
    }

    void EnsureSpectrumProcessor() {
        if (settings_.type == 2 && !spectrum_processor_)
            spectrum_processor_ = SafeSpectrumCreate(spectrum_factory_);
    }

    void EnsureDream(bool force_reconfigure = false) {
        if (settings_.type != 1) return;
        // FUN_004568AA destroys a previously allocated dream object when the
        // new target is smaller than 2x2.  Merely skipping configuration
        // would let ordinal 92 keep producing an old-sized frame.
        if (width_ < 2 || height_ < 2) {
            DestroyDream();
            return;
        }
        const int width = width_;
        const int height = height_;
        if (dream_ && dream_width_ == width && dream_height_ == height) {
            if (!force_reconfigure) return;
            if (!SafeDreamResize(resize_, dream_, static_cast<uint32_t>(width),
                                 height)) {
                DestroyDream();
            } else {
                dream_bits_ = nullptr;
                return;
            }
        }
        if (dream_) {
            if (!SafeDreamResize(resize_, dream_, static_cast<uint32_t>(width),
                                 height)) {
                DestroyDream();
            }
        }
        if (!dream_)
            dream_ = SafeDreamCreate(create_, static_cast<uint32_t>(width), height);
        if (dream_) {
            dream_width_ = width;
            dream_height_ = height;
        }
    }

    void UpdateDream(const audio::VisualizationSamples& samples) {
        EnsureDream();
        if (dream_) {
            dream_bits_ = SafeDreamProcess(process_, dream_,
                analysis_work_.data(),
                analysis_work_.data() + kAnalysisSamples);
        }
        static_cast<void>(samples);
        have_dynamic_frame_ = dream_bits_ != nullptr;
    }

    void UpdateSpectrum(const audio::VisualizationSamples&) {
        if (spectrum_.size() != static_cast<size_t>(width_))
            spectrum_.assign(static_cast<size_t>(width_), int16_t{});
        if (peaks_.size() != spectrum_.size()) peaks_ = spectrum_;
        if (peak_speed_.size() != spectrum_.size()) peak_speed_ = spectrum_;

        // The sound-player vtable method at +0x64 passes a 0xC00-byte work
        // block through ttpcomm ordinal 105.  Mode 1 leaves the 512 spectrum
        // magnitudes at short offset 0x400.  Reusing that processor removes
        // the reconstruction-only Hann/normalisation curve on the original
        // x86 build while retaining the local FFT solely as a safe fallback.
        EnsureSpectrumProcessor();
        // FUN_004ABBDC invokes the installed processor only when its object
        // exists.  If ordinal 105 is unavailable or faults, the original
        // leaves the persistent third 512-short block untouched; it does not
        // substitute a host FFT with a different response curve.
        static_cast<void>(SafeSpectrumProcess(
            spectrum_processor_, analysis_work_.data(), 1));

        const int active_columns = std::min(width_, 256);
        const int bin_columns = std::min(width_ + 6, 256);
        std::vector<int> targets(static_cast<size_t>(width_), 0);
        int previous_bin{};
        for (int x = 0; x < active_columns; ++x) {
            int bin = static_cast<int>(std::floor(std::pow(
                2.0, 7.5 * static_cast<double>(x) /
                         static_cast<double>(std::max(1, bin_columns)))));
            bin = std::clamp(std::max(bin, previous_bin + 1), 1, 255);
            previous_bin = bin;
            const int fft_value = std::max(0, static_cast<int>(analysis_work_[
                kAnalysisSamples * 2 + static_cast<size_t>(bin)]));
            const int shifted = fft_value >> 5;
            targets[static_cast<size_t>(x)] = shifted == 0 ? 0 :
                static_cast<int>(std::log(static_cast<double>(shifted)) *
                    (static_cast<double>(height_) * 1.3333) /
                    std::log(256.0));
        }
        const int maximum = std::max(0, height_ - 1);
        for (int x = 0; x < active_columns; ++x) {
            const int current = targets[static_cast<size_t>(x)];
            const int left = x == 0 ? current
                : spectrum_[static_cast<size_t>(x - 1)];
            const int right = x + 1 == active_columns ? current
                : spectrum_[static_cast<size_t>(x + 1)];
            const int smooth = std::clamp(
                (left + current * 2 + right) / 4, 0, maximum);
            auto& bar = spectrum_[static_cast<size_t>(x)];
            if (smooth > bar) bar = static_cast<int16_t>(smooth);
            else if (bar > 2) bar -= 2;
            auto& peak = peaks_[static_cast<size_t>(x)];
            auto& fall_counter = peak_speed_[static_cast<size_t>(x)];
            previous_peaks_[static_cast<size_t>(x)] = peak;
            if (peak <= bar) {
                peak = bar;
                fall_counter = 0;
            } else if (peak > 0) {
                peak = static_cast<int16_t>(peak - (fall_counter >> 3));
                ++fall_counter;
            }
            if (fall_counter >= maximum) peak = 0;
        }
        have_dynamic_frame_ = true;
    }

    static void DrawScopeColumn(std::vector<unsigned char>& mask, int stride,
                                int rows, int column, int from_y, int to_y) {
        if (column <= 0 || column >= stride - 1) return;
        const int first = std::clamp(std::min(from_y, to_y), 0, rows - 1);
        const int last = std::clamp(std::max(from_y, to_y), 0, rows - 1);
        for (int y = first; y <= last; ++y)
            mask[static_cast<size_t>(y) * stride + column] = 255;
    }

    void UpdateScope(const audio::VisualizationSamples& samples) {
        const int stride = width_ + 2;
        const int rows = height_ + 2;
        if (scope_.size() != static_cast<size_t>(stride) * rows)
            scope_.assign(static_cast<size_t>(stride) * rows, 0);
        if (settings_.blur) {
            std::fill(scope_.begin(), scope_.begin() + stride,
                      static_cast<unsigned char>(0));
            std::fill(scope_.end() - stride, scope_.end(),
                      static_cast<unsigned char>(0));
            for (int y = 0; y < rows; ++y) {
                scope_[static_cast<size_t>(y) * stride] = 0;
                scope_[static_cast<size_t>(y) * stride + stride - 1] = 0;
            }
            // FUN_00457452 updates the mask in place: upper/left neighbours
            // are from this pass, lower/right neighbours from the prior pass.
            for (int y = 1; y <= height_; ++y) {
                for (int x = 0; x <= width_; ++x) {
                    const size_t offset = static_cast<size_t>(y) * stride + x;
                    const int average = (scope_[offset - stride] +
                                         scope_[offset + stride] +
                                         scope_[offset - 1] +
                                         scope_[offset + 1]) / 4;
                    scope_[offset] = static_cast<unsigned char>(std::max(
                        0, average - settings_.blur_speed));
                }
            }
        } else {
            std::fill(scope_.begin(), scope_.end(), static_cast<unsigned char>(0));
        }
        const size_t count = std::min(samples.count, kAnalysisSamples);
        int previous_y = std::clamp(
            (height_ + 2) -
                ((static_cast<int>(count ? analysis_work_[0] : 0) +
                  32768) * (height_ + 2)) / 65536 - 1,
            0, height_ + 1);
        for (int x = 0; x < width_; ++x) {
            // The original renderer advances through the persistent 1536
            // shorts without clamping at sample 511.  Intended skin widths
            // stay inside that allocation; keep an explicit zero guard for
            // malformed, wider rectangles instead of repeating sample 511.
            const int value = static_cast<size_t>(x) < analysis_work_.size()
                ? analysis_work_[static_cast<size_t>(x)] : 0;
            const int y = std::clamp((height_ + 2) -
                ((value + 32768) * (height_ + 2)) / 65536 - 1,
                0, height_ + 1);
            // FUN_004574F7 advances only by the bitmap row stride: each
            // sample therefore paints a vertical segment in one fixed
            // padded column, not a Bresenham diagonal into the next column.
            DrawScopeColumn(scope_, stride, rows, x + 1, previous_y, y);
            previous_y = y;
        }
        have_dynamic_frame_ = true;
    }

    void PaintDream(HDC dc, const RECT& bounds) const {
        if (!have_dynamic_frame_ || !dream_bits_ ||
            dream_width_ <= 0 || dream_height_ <= 0) return;
        if (dream_bits_ && dream_width_ > 0 && dream_height_ > 0) {
            BITMAPINFO information{};
            information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            information.bmiHeader.biWidth = dream_width_;
            information.bmiHeader.biHeight = dream_height_;
            information.bmiHeader.biPlanes = 1;
            information.bmiHeader.biBitCount = 32;
            information.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top,
                0, 0, dream_width_, dream_height_, dream_bits_, &information,
                DIB_RGB_COLORS, SRCCOPY);
            return;
        }
    }

    void PaintSpectrum(HDC dc, const RECT& bounds) {
        if (!have_dynamic_frame_) return;
        if (width_ <= 0 || height_ <= 0 || !RestoreBackground())
            return;
        const int height = height_;
        const int split = height * 3 / 5;
        const int active_columns = std::min<int>(
            256, std::min<int>(width_, spectrum_.size()));
        for (int x = 0; x < active_columns; ++x) {
            if (settings_.spectrum_wide != 0 && x % 4 == 3) continue;
            // FUN_00456EDA divides wide-mode x by four, skips remainder 3,
            // then multiplies the quotient back by four.  Each visible
            // three-column group therefore shares the first state record.
            const size_t state_index = static_cast<size_t>(
                settings_.spectrum_wide != 0 ? (x & ~3) : x);
            const int bar_height = std::clamp<int>(
                spectrum_[state_index], 0, height);
            for (int offset = 0; offset < bar_height; ++offset) {
                const int y = height - 1 - offset;
                const COLORREF color = y < split
                    ? Interpolate(settings_.spectrum_top_color,
                                  settings_.spectrum_middle_color, y, split)
                    : Interpolate(settings_.spectrum_middle_color,
                                  settings_.spectrum_bottom_color, y - split,
                                  std::max(1, height - split));
                surface_bits_[static_cast<size_t>(y) * width_ + x] =
                    DibColor(color);
            }
            const int peak = height - 1 - peaks_[state_index];
            if (peak > 0 && peak < height) {
                surface_bits_[static_cast<size_t>(peak) * width_ + x] =
                    DibColor(settings_.spectrum_peak_color);
            }
            if (settings_.spectrum_wide == 0 &&
                peaks_[state_index] < previous_peaks_[state_index]) {
                // The source DIB is bottom-up.  FUN_00456DA5 advances from
                // the previous (higher) peak toward the falling marker, so
                // after converting to this top-down DIB the three blended
                // pixels sit above the current marker, not below the bar.
                for (int trail = 1; trail <= 3 && peak - trail > 0;
                     ++trail) {
                    const size_t pixel = static_cast<size_t>(peak - trail) *
                                         width_ + x;
                    surface_bits_[pixel] = BlendDibColor(
                        surface_bits_[pixel], settings_.spectrum_peak_color,
                        static_cast<unsigned int>(4 - trail));
                }
            }
        }
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchBlt(dc, bounds.left, bounds.top,
                   bounds.right - bounds.left, bounds.bottom - bounds.top,
                   surface_dc_, 0, 0, width_, height_, SRCCOPY);
    }

    void PaintScope(HDC dc, const RECT& bounds) {
        if (!have_dynamic_frame_ || scope_.empty()) return;
        if (!RestoreBackground()) return;
        const unsigned int red = GetRValue(settings_.blur_scope_color);
        const unsigned int green = GetGValue(settings_.blur_scope_color);
        const unsigned int blue = GetBValue(settings_.blur_scope_color);
        const int stride = width_ + 2;
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                // The original mask and destination are positive-height
                // DIBs traversed from their last scanline.  In this top-down
                // surface the corresponding mask row is H-y, not y+1.
                const unsigned int mask = scope_[
                    static_cast<size_t>(height_ - y) * stride + x + 1];
                if (mask == 0) continue;
                const size_t offset = static_cast<size_t>(y) * width_ + x;
                const uint32_t background = surface_bits_[offset];
                const unsigned int amount = mask + 1;
                const unsigned int keep = 256 - amount;
                const unsigned int out_red =
                    (((background >> 16) & 0xffU) * keep + red * amount) >> 8;
                const unsigned int out_green =
                    (((background >> 8) & 0xffU) * keep + green * amount) >> 8;
                const unsigned int out_blue =
                    ((background & 0xffU) * keep + blue * amount) >> 8;
                surface_bits_[offset] = (background & 0xff000000U) |
                    (out_red << 16) | (out_green << 8) | out_blue;
            }
        }
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchBlt(dc, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            surface_dc_, 0, 0, width_, height_, SRCCOPY);
    }

    void PaintCover(HDC dc, const RECT& bounds) {
        fallback_bounds_ = {};
        // FUN_00457B11 keeps update and paint inside CVisualCtrl's critical
        // section, and FUN_00457E2D presents one completed visual frame.  The
        // reconstruction previously copied the skin background to the window
        // DC first and only then alpha-blended the WIC bitmap.  At the visual
        // worker cadence that exposed a real background-only intermediate
        // frame, especially while the layered parent was being refreshed.
        // Compose the complete Type 4 frame in the persistent DIB and publish
        // it with one final blit.  WIC remains the primary decoder; IPicture is
        // still the fallback, but neither path can now be observed half drawn.
        if (!RestoreBackground()) return;
        const RECT frame{0, 0, width_, height_};
        if (cover_payload_present_) {
            if (cover_bitmap_) {
                const int source_width = cover_bitmap_size_.cx;
                const int source_height = cover_bitmap_size_.cy;
                const int available_width = frame.right - frame.left;
                const int available_height = frame.bottom - frame.top;
                const double scale = std::min(1.0, std::min(
                    static_cast<double>(available_width) / source_width,
                    static_cast<double>(available_height) / source_height));
                const int width = std::max(1,
                    static_cast<int>(source_width * scale));
                const int height = std::max(1,
                    static_cast<int>(source_height * scale));
                const int x = frame.left + (available_width - width) / 2;
                const int y = frame.top + (available_height - height) / 2;
                const HDC source = CreateCompatibleDC(surface_dc_);
                if (source) {
                    const HGDIOBJ old = SelectObject(source, cover_bitmap_);
                    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                    AlphaBlend(surface_dc_, x, y, width, height, source, 0, 0,
                               source_width, source_height, blend);
                    SelectObject(source, old);
                    DeleteDC(source);
                }
            } else if (cover_) {
                LONG himetric_width{};
                LONG himetric_height{};
                cover_->get_Width(&himetric_width);
                cover_->get_Height(&himetric_height);
                const int source_width = MulDiv(
                    himetric_width, GetDeviceCaps(surface_dc_, LOGPIXELSX), 2540);
                const int source_height = MulDiv(
                    himetric_height, GetDeviceCaps(surface_dc_, LOGPIXELSY), 2540);
                if (source_width <= 0 || source_height <= 0) {
                    BlitSurface(dc, bounds);
                    return;
                }
                const int available_width = frame.right - frame.left;
                const int available_height = frame.bottom - frame.top;
                const double scale = std::min(1.0, std::min(
                    static_cast<double>(available_width) / source_width,
                    static_cast<double>(available_height) / source_height));
                const int width = std::max(1,
                    static_cast<int>(source_width * scale));
                const int height = std::max(1,
                    static_cast<int>(source_height * scale));
                const int x = frame.left + (available_width - width) / 2;
                const int y = frame.top + (available_height - height) / 2;
                cover_->Render(surface_dc_, x, y, width, height, 0, himetric_height,
                               himetric_width, -himetric_height, nullptr);
            }
        } else if (!fallback_.empty()) {
            const LOGFONTW description = settings_.font_valid
                ? settings_.font : DefaultVisualFont();
            const HFONT font = CreateFontIndirectW(&description);
            const HGDIOBJ old_font = font ? SelectObject(surface_dc_, font)
                                          : nullptr;
            SetBkMode(surface_dc_, TRANSPARENT);
            SetTextColor(surface_dc_, settings_.text_color);
            RECT measured{};
            DrawTextW(surface_dc_, fallback_.c_str(), -1, &measured,
                      DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            const int width = std::min<int>(measured.right - measured.left,
                                            frame.right - frame.left);
            const int height = std::min<int>(measured.bottom - measured.top,
                                             frame.bottom - frame.top);
            RECT text_bounds{
                frame.left + ((frame.right - frame.left) - width) / 2,
                frame.top + ((frame.bottom - frame.top) - height) / 2,
                frame.left + ((frame.right - frame.left) + width) / 2,
                frame.top + ((frame.bottom - frame.top) + height) / 2};
            DrawTextW(surface_dc_, fallback_.c_str(), -1, &text_bounds,
                      DT_SINGLELINE | DT_NOPREFIX);
            const int output_width = bounds.right - bounds.left;
            const int output_height = bounds.bottom - bounds.top;
            fallback_bounds_ = {
                bounds.left + MulDiv(text_bounds.left, output_width,
                                     std::max(1, width_)),
                bounds.top + MulDiv(text_bounds.top, output_height,
                                    std::max(1, height_)),
                bounds.left + MulDiv(text_bounds.right, output_width,
                                     std::max(1, width_)),
                bounds.top + MulDiv(text_bounds.bottom, output_height,
                                    std::max(1, height_))};
            if (font) {
                SelectObject(surface_dc_, old_font);
                DeleteObject(font);
            }
        }
        BlitSurface(dc, bounds);
    }

    void BlitSurface(HDC dc, const RECT& bounds) const {
        if (!dc || !surface_dc_ || width_ <= 0 || height_ <= 0) return;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchBlt(dc, bounds.left, bounds.top,
                   bounds.right - bounds.left, bounds.bottom - bounds.top,
                   surface_dc_, 0, 0, width_, height_, SRCCOPY);
    }

    void LoadCover() {
        if (cover_bitmap_) {
            DeleteObject(cover_bitmap_);
            cover_bitmap_ = nullptr;
            cover_bitmap_size_ = {};
        }
        if (cover_) {
            cover_->Release();
            cover_ = nullptr;
        }
        cover_payload_present_ = false;
        fallback_bounds_ = {};
        if (source_.empty()) return;
        // CPlayerWnd_Run obtains the byte blob from the active reader's
        // embedded-picture metadata and decodes exactly that blob. Shell
        // thumbnails may silently substitute Folder.jpg or generic artwork,
        // which the original never treats as a track cover.
        try {
            if (thumbnail_interface_) {
                // FUN_004AD83E asks ISoundThumbnail slot 6 for index zero;
                // it neither searches for picture type 3 nor falls through to
                // another parser when the reader owns this capability.
                cover_payload_present_ = !thumbnail_.empty();
                const auto wic = DecodePictureWithWic(thumbnail_);
                cover_bitmap_ = wic.bitmap;
                cover_bitmap_size_ = {
                    static_cast<LONG>(wic.width), static_cast<LONG>(wic.height)};
                if (!cover_bitmap_)
                    cover_ = DecodePictureWithOle(thumbnail_);
            } else {
                bool picture_declared{};
                auto bytes = ReadEmbeddedPicture(source_, &picture_declared);
                cover_payload_present_ = !bytes.empty();
                const auto wic = DecodePictureWithWic(bytes);
                cover_bitmap_ = wic.bitmap;
                cover_bitmap_size_ = {
                    static_cast<LONG>(wic.width), static_cast<LONG>(wic.height)};
                if (!cover_bitmap_) cover_ = DecodePictureWithOle(bytes);
            }
        } catch (const std::exception&) {
            cover_ = nullptr;
        }
    }

    HMODULE module_{};
    DreamCreate create_{};
    DreamResize resize_{};
    DreamProcess process_{};
    DreamDestroy destroy_{};
    SpectrumFactory spectrum_factory_{};
    void* spectrum_processor_{};
    HDC surface_dc_{};
    HBITMAP surface_bitmap_{};
    HGDIOBJ surface_old_bitmap_{};
    uint32_t* surface_bits_{};
    void* dream_{};
    const uint32_t* dream_bits_{};
    int dream_width_{};
    int dream_height_{};
    settings::VisualSettings settings_{};
    int width_{};
    int height_{};
    int output_width_{};
    int output_height_{};
    bool full_screen_{};
    mutable std::recursive_mutex mutex_;
    // CVisualCtrl+0x94c: one persistent 0xC00-byte analysis work block.
    std::array<int16_t, kAnalysisSamples * 3> analysis_work_{};
    std::vector<uint32_t> background_pixels_;
    // FUN_00456A11 updates four packed signed 16-bit values per column:
    // bar, fall counter, peak and previous peak.  Keeping integral state is
    // required for its truncating neighbour average and 2-pixel decay.
    std::vector<int16_t> spectrum_;
    std::vector<int16_t> peaks_;
    std::vector<int16_t> peak_speed_;
    std::vector<int16_t> previous_peaks_;
    std::vector<unsigned char> scope_;
    bool have_dynamic_frame_{};
    std::filesystem::path source_;
    std::wstring fallback_;
    bool thumbnail_interface_{};
    std::vector<unsigned char> thumbnail_;
    bool cover_payload_present_{};
    HBITMAP cover_bitmap_{};
    SIZE cover_bitmap_size_{};
    IPicture* cover_{};
    RECT fallback_bounds_{};
};

void PlayerWindow::SetTtpCommModule(HMODULE module) noexcept {
    ttpcomm_module_ = module;
    audio_.SetTtpCommModule(module);
    if (visual_runtime_) visual_runtime_->SetModule(module);
}

LRESULT CALLBACK PlayerWindow::VisualWindowProc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<PlayerWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<PlayerWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        // CreateWindowExW does not return until WM_NCCREATE/WM_CREATE have
        // completed.  Keep the native handle available during that interval;
        // otherwise the default branch would call DefWindowProcW(nullptr,
        // WM_NCCREATE, ...) and reject creation before visual_window_ can be
        // assigned by the caller.
        if (self) self->visual_window_ = window;
    }
    return self ? self->HandleVisualMessage(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT PlayerWindow::HandleVisualMessage(
    UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(visual_window_, &paint);
        PaintVisualControl(dc);
        EndPaint(visual_window_, &paint);
        return 0;
    }
    case WM_LBUTTONUP: {
        // FUN_00457EF5 dispatches every WM_LBUTTONUP through WM_COMMAND; it
        // does not require a preceding pressed-state transition.
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        InvokeVisualAction(point);
        return 0;
    }
    case WM_KEYDOWN:
        if (fullscreen_mode_ != 0 && wparam == VK_ESCAPE) {
            SetFullScreenMode(0);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (fullscreen_mode_ != 0 && LOWORD(wparam) == WA_INACTIVE)
            HandleFullScreenDeactivate(reinterpret_cast<HWND>(lparam));
        break;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        // FUN_00458020 and the embedded parent route forward the signed
        // lParam coordinates unchanged, including keyboard (-1,-1).
        ShowVisualContextMenu(point);
        return 0;
    }
    case WM_SETCURSOR: {
        if (reinterpret_cast<HWND>(wparam) != visual_window_ ||
            LOWORD(lparam) != HTCLIENT) break;
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(visual_window_, &point);
        // FUN_0045813C claims the message only over the no-cover prompt. At
        // every other point the ordinary class/parent cursor path remains in
        // charge; forcing IDC_ARROW here is observably different.
        if (VisualFallbackHit(point)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    default:
        return DefWindowProcW(visual_window_, message, wparam, lparam);
    }
    return DefWindowProcW(visual_window_, message, wparam, lparam);
}

void PlayerWindow::StartVisualWorker() {
    if (visual_worker_.joinable() || !window_) return;
    visual_worker_ = std::jthread([this](std::stop_token stop) {
        for (;;) {
            const auto interval = std::chrono::milliseconds(
                std::max(1U, visual_interval_ms_.load(
                    std::memory_order_relaxed)));
            std::unique_lock lock(visual_worker_mutex_);
            visual_worker_condition_.wait_for(
                lock, stop, interval, [] { return false; });
            lock.unlock();
            if (stop.stop_requested()) return;
            if (visual_worker_enabled_.load(std::memory_order_acquire) &&
                audio_.State() == audio::PlaybackState::playing) {
                // CPlayerWnd::Run invokes FUN_00457B11 on this worker.  It
                // updates under the visual object's critical section, then
                // obtains the child DC and paints immediately; no UI-thread
                // frame message is involved.  Consequently modal menus do
                // not freeze spectrum, scope or dream animation.
                const auto runtime = visual_runtime_;
                const HWND visual = visual_window_;
                if (!runtime || !visual || !IsWindow(visual)) continue;
                runtime->Update(audio_.Visualization());
                const HDC dc = GetDC(visual);
                if (dc) {
                    RECT client{};
                    GetClientRect(visual, &client);
                    runtime->Paint(dc, client);
                    ReleaseDC(visual, dc);
                }
            }
        }
    });
}

void PlayerWindow::StopVisualWorker() {
    if (!visual_worker_.joinable()) return;
    visual_worker_.request_stop();
    visual_worker_condition_.notify_all();
    visual_worker_.join();
}

void PlayerWindow::ApplySkinVisualSettings() {
    if (!skin_) return;
    const auto& source = skin_->Visual();
    if (!source.valid) return;
    const auto assign = [](auto& target, const auto& value) {
        if (value) target = *value;
    };
    assign(settings_.visual.spectrum_top_color, source.spectrum_top_color);
    assign(settings_.visual.spectrum_bottom_color, source.spectrum_bottom_color);
    assign(settings_.visual.spectrum_middle_color, source.spectrum_middle_color);
    assign(settings_.visual.spectrum_peak_color, source.spectrum_peak_color);
    assign(settings_.visual.spectrum_wide, source.spectrum_wide);
    assign(settings_.visual.blur_speed, source.blur_speed);
    assign(settings_.visual.type, source.type);
    assign(settings_.visual.blur, source.blur);
    assign(settings_.visual.blur_scope_color, source.blur_scope_color);
    assign(settings_.visual.text_color, source.text_color);
    if (source.font) {
        settings_.visual.font = *source.font;
        settings_.visual.font_valid = true;
    }
    settings_.visual.type = std::clamp(settings_.visual.type, 0, 4);
}

void PlayerWindow::UpdateVisualWindowLayout() {
    if (!window_) return;
    if (!visual_runtime_) {
        visual_runtime_ = std::make_shared<VisualRuntime>();
        visual_runtime_->SetModule(ttpcomm_module_);
    }
    if (fullscreen_visual_detached_) {
        RECT client{};
        GetClientRect(visual_window_, &client);
        const int width = std::max<LONG>(0, client.right - client.left);
        const int height = std::max<LONG>(0, client.bottom - client.top);
        visual_runtime_->Configure(settings_.visual, {width, height}, true,
                                   nullptr, nullptr);
        const UINT interval = settings_.visual.frames_per_second > 0
            ? std::max(1, 1000 / settings_.visual.frames_per_second) : 50;
        visual_interval_ms_.store(interval, std::memory_order_relaxed);
        visual_worker_enabled_.store(settings_.visual.type != 0,
                                     std::memory_order_release);
        visual_worker_condition_.notify_all();
        InvalidateRect(visual_window_, nullptr, FALSE);
        return;
    }
    const auto* element = FindActiveSkinElement(L"visual");
    if (!visual_window_ || !element ||
        element->bounds.right <= element->bounds.left ||
        element->bounds.bottom <= element->bounds.top) {
        if (visual_window_) ShowWindow(visual_window_, SW_HIDE);
        visual_worker_enabled_.store(false, std::memory_order_release);
        return;
    }
    const int width = element->bounds.right - element->bounds.left;
    const int height = element->bounds.bottom - element->bounds.top;
    SetWindowPos(visual_window_, nullptr, element->bounds.left,
                 element->bounds.top, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    const HBITMAP background = ActiveSkinBackground();
    visual_runtime_->Configure(settings_.visual, {width, height}, false,
                               background, &element->bounds);
    const UINT interval = settings_.visual.frames_per_second > 0
        ? std::max(1, 1000 / settings_.visual.frames_per_second) : 50;
    visual_interval_ms_.store(interval, std::memory_order_relaxed);
    visual_worker_enabled_.store(settings_.visual.type != 0,
                                 std::memory_order_release);
    visual_worker_condition_.notify_all();
    InvalidateRect(visual_window_, nullptr, FALSE);
}

void PlayerWindow::UpdateVisualFrame() {
    if (!visual_runtime_ || !visual_window_) return;
    const auto state = audio_.State();
    if (state == audio::PlaybackState::playing) {
        std::filesystem::path source;
        if (const auto* track = PlaybackTrackForUi()) source = track->path;
        visual_runtime_->SetSource(source, ResourceText(0x821b),
                                   audio_.Metadata());
        // Dynamic modes are advanced exclusively by CPlayerWnd::Run's
        // worker cadence.  UI commands only publish cover/source state and
        // request a repaint; updating here would add an extra decay/blur
        // step whenever a skin or visual type changes.
    } else if (state == audio::PlaybackState::stopped ||
               state == audio::PlaybackState::failed) {
        // CPlayerWnd's stop/failure paths call FUN_00457BCF before the next
        // paint.  That routine clears both animated state and the decoded
        // picture; retaining the previous track's cover after Stop is not an
        // original behavior.
        visual_runtime_->ClearPlayback();
        // FUN_00457BCF calls FUN_00457600 before invalidating: clear the
        // visible child synchronously instead of waiting for a later UI
        // WM_PAINT (the render worker has already stopped at this point).
        const HDC dc = GetDC(visual_window_);
        if (dc) {
            RECT client{};
            GetClientRect(visual_window_, &client);
            // FUN_00457600 restores only the backing here.  The following
            // invalidation may then repaint Type 1's retained zeroed bitmap.
            visual_runtime_->PaintBackgroundOnly(dc, client);
            ReleaseDC(visual_window_, dc);
        }
    }
    InvalidateRect(visual_window_, nullptr, FALSE);
}

void PlayerWindow::PaintVisualControl(HDC dc) const {
    if (!dc || !visual_window_) return;
    RECT client{};
    GetClientRect(visual_window_, &client);
    if (fullscreen_visual_detached_) {
        FillRect(dc, &client, static_cast<HBRUSH>(
            GetStockObject(BLACK_BRUSH)));
        if (visual_runtime_) visual_runtime_->Paint(dc, client);
        return;
    }
    const auto* element = FindActiveSkinElement(L"visual");
    const HBITMAP background = ActiveSkinBackground();
    if (background && element) {
        const HDC source = CreateCompatibleDC(dc);
        const HGDIOBJ old = SelectObject(source, background);
        BitBlt(dc, 0, 0, client.right, client.bottom, source,
               element->bounds.left, element->bounds.top, SRCCOPY);
        SelectObject(source, old);
        DeleteDC(source);
    }
    if (visual_runtime_) visual_runtime_->Paint(dc, client);
}

void PlayerWindow::SetVisualType(int type) {
    settings_.visual.type = fullscreen_mode_ != 0
        ? ((type == 0 || type == 4) ? 1 : std::clamp(type, 1, 3))
        : std::clamp(type, 0, 4);
    if (fullscreen_mode_ == 3) UpdateFullScreenLayout();
    UpdateVisualWindowLayout();
    UpdateVisualFrame();
}

bool PlayerWindow::VisualFallbackHit(POINT point) const {
    return visual_runtime_ && visual_runtime_->FallbackHit(point);
}

void PlayerWindow::InvokeVisualAction(POINT point) {
    // FUN_00465057 distinguishes only the clickable no-cover text.  Clicking
    // an actual cover still advances to Type 0, exactly like any other point.
    SendMessageW(window_, WM_COMMAND,
        MAKEWPARAM(kVisualControlId, VisualFallbackHit(point) ? 1 : 0),
        reinterpret_cast<LPARAM>(visual_window_));
}

void PlayerWindow::DetachVisualWindow(const RECT& target) {
    if (!visual_window_ || !IsWindow(visual_window_)) return;
    if (!fullscreen_visual_detached_) {
        fullscreen_visual_saved_exstyle_ =
            GetWindowLongPtrW(visual_window_, GWL_EXSTYLE);
        SetWindowLongPtrW(visual_window_, GWL_EXSTYLE,
            fullscreen_visual_saved_exstyle_ | WS_EX_TOOLWINDOW);
        GetWindowRect(visual_window_, &fullscreen_visual_saved_rect_);
        fullscreen_visual_was_empty_ =
            IsRectEmpty(&fullscreen_visual_saved_rect_) != FALSE;
        if (fullscreen_visual_was_empty_) {
            SetWindowPos(visual_window_, nullptr, 0, 0, 1, 1,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        fullscreen_visual_parent_ = GetParent(visual_window_);
        // FUN_00467735 deliberately retains WS_CHILD.  With a null parent
        // User32 attaches that child-style surface to the desktop window.
        SetParent(visual_window_, nullptr);
        // The saved rectangle is deliberately sampled again after SetParent;
        // FUN_0046786F restores this desktop-coordinate rectangle first.
        GetWindowRect(visual_window_, &fullscreen_visual_saved_rect_);
        fullscreen_visual_detached_ = true;
    }
    const RECT window_target = WindowRectForClientTarget(visual_window_, target);
    SetWindowPos(visual_window_, HWND_TOPMOST,
        window_target.left, window_target.top,
        window_target.right - window_target.left,
        window_target.bottom - window_target.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PlayerWindow::RestoreVisualWindow() {
    if (!fullscreen_visual_detached_ || !visual_window_) return;
    // FUN_0046786F first restores the saved desktop rectangle while the
    // control is still detached, then reparents it.  Its exact
    // hWndInsertAfter == NULL / flags == 0x50 call is retained; the TOPMOST
    // bit introduced by the detach remains observable after reparenting.
    SetWindowPos(visual_window_, nullptr,
        fullscreen_visual_saved_rect_.left,
        fullscreen_visual_saved_rect_.top,
        fullscreen_visual_saved_rect_.right -
            fullscreen_visual_saved_rect_.left,
        fullscreen_visual_saved_rect_.bottom -
            fullscreen_visual_saved_rect_.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetRectEmpty(&fullscreen_visual_saved_rect_);
    SetParent(visual_window_, fullscreen_visual_parent_);
    if (fullscreen_visual_was_empty_) {
        SetWindowPos(visual_window_, nullptr, 0, 0, 0, 0,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetWindowLongPtrW(visual_window_, GWL_EXSTYLE,
        GetWindowLongPtrW(visual_window_, GWL_EXSTYLE) &
        ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW));
    fullscreen_visual_detached_ = false;
    fullscreen_visual_was_empty_ = false;
    fullscreen_visual_parent_ = nullptr;
    UpdateVisualWindowLayout();
}

void PlayerWindow::DetachLyricControl(const RECT& target,
                                      HWND insert_after) {
    if (!lyric_control_ || !IsWindow(lyric_control_)) return;
    if (!fullscreen_lyric_detached_) {
        fullscreen_lyric_saved_style_ =
            GetWindowLongPtrW(lyric_control_, GWL_STYLE);
        fullscreen_lyric_saved_exstyle_ =
            GetWindowLongPtrW(lyric_control_, GWL_EXSTYLE);
        SetWindowLongPtrW(lyric_control_, GWL_EXSTYLE,
            fullscreen_lyric_saved_exstyle_ | WS_EX_TOOLWINDOW);
        GetWindowRect(lyric_control_, &fullscreen_lyric_saved_rect_);
        fullscreen_lyric_was_empty_ =
            IsRectEmpty(&fullscreen_lyric_saved_rect_) != FALSE;
        if (fullscreen_lyric_was_empty_) {
            SetWindowPos(lyric_control_, nullptr, 0, 0, 1, 1,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        fullscreen_lyric_parent_ = GetParent(lyric_control_);
        SetParent(lyric_control_, nullptr);
        GetWindowRect(lyric_control_, &fullscreen_lyric_saved_rect_);
        fullscreen_lyric_detached_ = true;
        if (lyric_window_) ShowWindow(lyric_window_, SW_HIDE);
    }
    const RECT window_target = WindowRectForClientTarget(lyric_control_, target);
    SetWindowPos(lyric_control_, insert_after,
        window_target.left, window_target.top,
        window_target.right - window_target.left,
        window_target.bottom - window_target.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // FUN_0044ABB1 swaps CHILD for POPUP only after the generic detach helper.
    // The stock LyricCtrl already carries CLIPSIBLINGS; retain the bit here as
    // the reconstructed control acquires it on its first full-screen trip.
    LONG_PTR style = GetWindowLongPtrW(lyric_control_, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_CHILD);
    style |= WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS;
    SetWindowLongPtrW(lyric_control_, GWL_STYLE, style);
    RebuildLyricFont(false);
    ApplyFullScreenLyricTransparency();
    UpdateLyricScrollTimer();
    InvalidateRect(lyric_control_, nullptr, FALSE);
}

void PlayerWindow::RestoreLyricControl() {
    if (!fullscreen_lyric_detached_ || !lyric_control_) return;
    // FUN_0044ABFC reverses POPUP/CHILD before invoking the generic restore.
    LONG_PTR style = GetWindowLongPtrW(lyric_control_, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_POPUP);
    style |= WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(lyric_control_, GWL_STYLE, style);

    SetWindowPos(lyric_control_, nullptr,
        fullscreen_lyric_saved_rect_.left,
        fullscreen_lyric_saved_rect_.top,
        fullscreen_lyric_saved_rect_.right - fullscreen_lyric_saved_rect_.left,
        fullscreen_lyric_saved_rect_.bottom - fullscreen_lyric_saved_rect_.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetRectEmpty(&fullscreen_lyric_saved_rect_);
    SetParent(lyric_control_, fullscreen_lyric_parent_);
    if (fullscreen_lyric_was_empty_) {
        SetWindowPos(lyric_control_, nullptr, 0, 0, 0, 0,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    LONG_PTR extended = GetWindowLongPtrW(lyric_control_, GWL_EXSTYLE);
    extended &= ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW | WS_EX_LAYERED);
    SetWindowLongPtrW(lyric_control_, GWL_EXSTYLE, extended);
    fullscreen_lyric_detached_ = false;
    fullscreen_lyric_was_empty_ = false;
    fullscreen_lyric_parent_ = nullptr;
    RebuildLyricFont(false);
    LayoutLyricControls();
    UpdateLyricScrollTimer();
}

void PlayerWindow::UpdateFullScreenLayout() {
    if (fullscreen_mode_ == 0) return;
    fullscreen_lyric_desktop_mode_ = false;
    const int screen_width = GetSystemMetrics(SM_CXSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYSCREEN);
    const RECT screen{0, 0, screen_width, screen_height};

    if (fullscreen_mode_ == 1) {
        RestoreVisualWindow();
        settings_.lyric.fullscreen_transparent =
            fullscreen_saved_lyric_transparent_;
        if (settings_.lyric.fullscreen_transparent &&
            LayeredWindowsAvailable()) {
            fullscreen_lyric_desktop_mode_ = true;
            RECT work_area = screen;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
            DetachLyricControl(work_area, HWND_BOTTOM);
            RedrawWindow(GetDesktopWindow(), nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        } else {
            DetachLyricControl(screen, HWND_TOPMOST);
        }
        return;
    }
    if (fullscreen_mode_ == 2) {
        RestoreLyricControl();
        DetachVisualWindow(screen);
        return;
    }

    int profile = settings_.fullscreen.visual_type != 0
        ? settings_.visual.type : 0;
    profile = std::clamp(profile, 0, 3);
    const int lyric_tenths = std::clamp(
        settings_.fullscreen.lyric_size[static_cast<size_t>(profile)], 0, 10);
    const int relation = std::clamp(
        settings_.fullscreen.position_relation[static_cast<size_t>(profile)],
        0, 1);
    // FUN_0046228D performs these three integer operations in this order;
    // MulDiv rounds to nearest and is one pixel higher for some screen sizes.
    const int boundary =
        ((10 - lyric_tenths) * screen_height * 10) / 100;
    const RECT lyric{0, boundary, screen_width, screen_height};
    if (lyric_tenths == 10 && relation == 0) {
        settings_.lyric.fullscreen_transparent = false;
    } else {
        const bool overlay = relation == 1 && LayeredWindowsAvailable();
        settings_.lyric.fullscreen_transparent = overlay;
        const RECT visual = overlay
            ? screen : RECT{0, 0, screen_width, boundary};
        DetachVisualWindow(visual);
    }
    DetachLyricControl(lyric, HWND_TOPMOST);
}

void PlayerWindow::SetFullScreenMode(int mode) {
    mode = std::clamp(mode, 0, 3);
    if (mode == 0) {
        LeaveFullScreen();
        return;
    }
    const bool entering = fullscreen_mode_ == 0;
    if (entering &&
        audio_.State() != audio::PlaybackState::playing) return;
    if (entering) {
        fullscreen_saved_visual_type_ = settings_.visual.type;
        fullscreen_saved_lyric_transparent_ =
            settings_.lyric.fullscreen_transparent;
        fullscreen_main_was_iconic_ = IsIconic(window_) != FALSE;
        fullscreen_main_was_visible_ = IsWindowVisible(window_) != FALSE;
        fullscreen_lyric_window_was_visible_ =
            lyric_window_ && IsWindowVisible(lyric_window_);
        fullscreen_desktop_lyric_was_visible_ =
            desktop_lyric_mode_ && desktop_lyrics_.Visible();
        if (fullscreen_desktop_lyric_was_visible_)
            desktop_lyrics_.Show(false);
        if (lyric_editor_) LeaveLyricEditor(true);
        if (lyric_window_) ShowWindow(lyric_window_, SW_HIDE);
    }
    if ((mode == 2 || mode == 3) &&
        (settings_.visual.type == 0 || settings_.visual.type == 4))
        settings_.visual.type = 1;
    fullscreen_mode_ = mode;
    if (entering) {
        // FUN_0046228D hides/minimizes CPlayerWnd before either child obtains
        // its non-empty saved full-screen rectangle.  Thus the synchronous
        // WM_ACTIVATEAPP generated here cannot mistake entry for deactivation
        // of an already active full-screen host.
        if (!fullscreen_main_was_iconic_)
            SendMessageW(window_, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                SWP_HIDEWINDOW);
        SetForegroundWindow(window_);
    }
    UpdateFullScreenLayout();
    UpdateVisualWindowLayout();
    UpdateVisualFrame();
    // FUN_0046228D installs Escape only after both detached surfaces have
    // received their final full-screen rectangles.
    if (entering && fullscreen_mode_ != 0)
        RegisterHotKey(window_, kFullscreenEscapeHotkey, 0, VK_ESCAPE);
}

void PlayerWindow::LeaveFullScreen() {
    if (fullscreen_mode_ == 0 && !fullscreen_visual_detached_ &&
        !fullscreen_lyric_detached_) return;
    fullscreen_mode_ = 0;
    // FUN_0046228D restores the two detached controls before publishing the
    // saved FullScreen/TransparentFS value or showing ordinary windows.
    RestoreVisualWindow();
    RestoreLyricControl();
    settings_.lyric.fullscreen_transparent =
        fullscreen_saved_lyric_transparent_;
    if (lyric_window_) {
        if (desktop_lyric_mode_) {
            ShowWindow(lyric_window_, SW_HIDE);
            desktop_lyrics_.Show(fullscreen_desktop_lyric_was_visible_);
        } else {
            ShowWindow(lyric_window_, fullscreen_lyric_window_was_visible_
                ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
    }
    fullscreen_desktop_lyric_was_visible_ = false;
    fullscreen_lyric_desktop_mode_ = false;
    UpdateVisualWindowLayout();
    UpdateVisualFrame();
    if (window_) {
        if (fullscreen_main_was_visible_) {
            SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                    SWP_SHOWWINDOW);
            if (!fullscreen_main_was_iconic_)
                SendMessageW(window_, WM_SYSCOMMAND, SC_RESTORE, 0);
        } else {
            ShowWindow(window_, SW_HIDE);
        }
    }
    // The original keeps the hotkey registered through the complete restore
    // sequence and removes it only after the main window is back in place.
    UnregisterHotKey(window_, kFullscreenEscapeHotkey);
}

void PlayerWindow::HandleFullScreenDeactivate(HWND activated_window) {
    if (fullscreen_mode_ == 0 || !activated_window) return;
    if (fullscreen_lyric_desktop_mode_) return;
    const DWORD active_thread = GetWindowThreadProcessId(activated_window,
                                                         nullptr);
    if (active_thread != GetCurrentThreadId())
        SendMessageW(window_, WM_COMMAND, kCmdFullscreenExit, 0);
}

void PlayerWindow::ShowVisualContextMenu(POINT screen_point) {
    if (context_menu_open_ || !visual_window_ ||
        !IsWindowEnabled(window_)) return;
    const HMODULE resources = ResourceModule();
    HMENU popup = DetachFirstPopup(
        LoadMenuW(resources, MAKEINTRESOURCEW(kMenuVisual)));
    if (!popup) return;
    HMENU fullscreen = DetachFirstPopup(
        LoadMenuW(resources, MAKEINTRESOURCEW(kMenuFullscreen)));
    if (fullscreen) {
        MENUITEMINFOW information{sizeof(information)};
        information.fMask = MIIM_SUBMENU;
        information.hSubMenu = fullscreen;
        if (!SetMenuItemInfoW(popup, kMenuFullscreen, FALSE, &information))
            DestroyMenu(fullscreen);
    }
    const bool may_enter =
        audio_.State() == audio::PlaybackState::playing;
    EnableCommand(popup, kCmdFullscreenLyrics, may_enter);
    EnableCommand(popup, kCmdFullscreenVisual, may_enter);
    EnableCommand(popup, kCmdFullscreenAll, may_enter);
    CheckCommand(popup, kCmdFullscreenLyrics, fullscreen_mode_ == 1);
    CheckCommand(popup, kCmdFullscreenVisual, fullscreen_mode_ == 2);
    CheckCommand(popup, kCmdFullscreenAll, fullscreen_mode_ == 3);

    if (fullscreen_visual_detached_) {
        // FUN_00458020 owns the detached-window path.  Its positional and
        // command deletions produce the compact seven-item menu and it uses
        // the saved main window directly as TrackPopupMenu's command owner.
        DeleteMenu(popup, 3, MF_BYPOSITION);
        DeleteMenu(popup, kCmdVisualCover, MF_BYCOMMAND);
        DeleteMenu(popup, kCmdVisualNone, MF_BYCOMMAND);
        DeleteMenu(popup, kCmdVisualOptions, MF_BYCOMMAND);
        const int last = GetMenuItemCount(popup) - 1;
        if (last >= 0) DeleteMenu(popup, last, MF_BYPOSITION);
        CheckMenuItem(popup,
            static_cast<UINT>(kCmdVisualFirst + settings_.visual.type),
            MF_BYCOMMAND | MF_CHECKED);
        context_menu_open_ = true;
        TrackPopupMenu(popup, TPM_RIGHTBUTTON, screen_point.x, screen_point.y,
                       0, window_, nullptr);
        DestroyMenu(popup);
        context_menu_open_ = false;
        PostMessageW(window_, WM_NULL, 0, 0);
        return;
    }

    // Resource 145 also contains the full-screen host's direct "exit full
    // screen" command.  The embedded parent fallback path removes that item
    // and the separator immediately following it, leaving the ten entries
    // observed in the original main-window VisualCtrl menu.
    DeleteMenu(popup, kCmdFullscreenToggle, MF_BYCOMMAND);
    if (GetMenuItemCount(popup) > 9)
        DeleteMenu(popup, 9, MF_BYPOSITION);

    // FUN_00458020 performs these deletions only when CVisualCtrl+0x24 is a
    // non-empty full-screen rectangle.  The embedded main-window control has
    // an empty rectangle, declines the message, and its parent consequently
    // presents the complete ten-item menu (None/Cover/Options included).
    // Keep the complete resource menu here; the compact seven-item variant is
    // selected by the full-screen host, not merely by right-clicking Visual.
    CheckMenuItem(popup,
        static_cast<UINT>(kCmdVisualFirst + settings_.visual.type),
        MF_BYCOMMAND | MF_CHECKED);

    context_menu_open_ = true;
    SetForegroundWindow(window_);
    BeginPopupMenuStyle(popup, true);
    TrackPopupMenu(popup, TPM_RIGHTBUTTON, screen_point.x, screen_point.y,
                   0, window_, nullptr);
    EndPopupMenuStyle();
    DestroyMenu(popup);
    context_menu_open_ = false;
    PostMessageW(window_, WM_NULL, 0, 0);
}

} // namespace ttplayer::ui
