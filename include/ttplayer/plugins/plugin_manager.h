#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <mmsystem.h>
#include <objidl.h>

namespace ttplayer::plugins {

// 004C51D3, also exported by the host EXE for old AddIns using temporary WAVs.
HRESULT CreateLegacyFileStream(const wchar_t* path, DWORD mode,
                              IStream** output) noexcept;

// One entry in the sound-reader registry built by CSoundLibrary_Initialize.
// The description is supplied by the successfully instantiated reader
// creator; it is not read directly from the DLL's string table.
struct ReaderFormat {
    std::wstring description;
    std::wstring pattern;
    std::filesystem::path module_path;
};

struct MetadataEntry {
    std::wstring name;
    std::wstring value;
};

// Public value form of the 0x18-byte ISoundThumbnail item used by
// FUN_004AD4C9/FUN_004AD663.  Strings and bytes remain owned by the caller for
// the duration of ReplaceThumbnails; the session builds the pointer-based
// private ABI entry only for each synchronous slot-8 call.
struct ThumbnailData {
    std::wstring mime;
    std::wstring description;
    std::vector<unsigned char> bytes;
    DWORD picture_type{3};
};

struct PluginInfo {
    std::filesystem::path path;
    bool has_legacy_entry{};
    bool instance_created{};
    bool registered{};
    size_t reader_count{};
    size_t decoder_creator_count{};
    size_t encoder_creator_count{};
    size_t lyric_search_provider_count{};
    HRESULT result{E_FAIL};
    std::string error;
};

struct DecoderFactoryInfo {
    std::wstring name;
    std::filesystem::path module_path;
};

struct EncoderFactoryInfo {
    std::wstring name;
    std::wstring extension;
    // IEncoderCreator slot 6 is the original CConvertDlg "配置" button
    // predicate (FUN_004C8625/0047D9C7), not an encoder-availability test.
    bool configurable{};
    std::filesystem::path module_path;
};

struct LyricSearchProviderInfo {
    std::wstring name;
    std::filesystem::path module_path;
};

// Adapter for the private IPlayerSoundReader ABI recovered from the original
// executable and the shipped 32-bit sound add-ins.  The implementation owns
// one reader COM-like object; its creating PluginManager (and therefore the
// code module) must outlive the session.
class LegacyReaderSession {
public:
    ~LegacyReaderSession();
    LegacyReaderSession(const LegacyReaderSession&) = delete;
    LegacyReaderSession& operator=(const LegacyReaderSession&) = delete;

    HRESULT Read(size_t requested_bytes, std::vector<std::byte>& output,
                 bool& end_of_stream) noexcept;
    HRESULT Seek(DWORD& position_ms) noexcept;

    [[nodiscard]] const WAVEFORMATEX& Format() const noexcept;
    [[nodiscard]] DWORD DurationMilliseconds() const noexcept;
    [[nodiscard]] DWORD SuggestedBufferBytes() const noexcept;
    [[nodiscard]] DWORD EncodedBitsPerSecond() const noexcept;
    [[nodiscard]] DWORD Capabilities() const noexcept;
    [[nodiscard]] const std::wstring& CodecName() const noexcept;
    [[nodiscard]] const std::filesystem::path& ModulePath() const noexcept;
    [[nodiscard]] const std::vector<MetadataEntry>& Metadata() const noexcept;
    // ISoundThumbnail {B5E770AF-DFB0-43E5-9B0C-3EE98E7B6248}.  Presence is
    // kept separate so callers do not bypass the reader ABI with an unrelated
    // file parser when the plug-in deliberately reports no first picture.
    [[nodiscard]] bool HasThumbnailInterface() const noexcept;
    [[nodiscard]] const std::vector<unsigned char>& Thumbnail() const noexcept;
    // FUN_004AD663 first requires reader capability bit 4, obtains the
    // current item count (slot 3) and the 60000-byte-per-item limit (slot 4),
    // removes old items through slot 9 in reverse order, then adds each new
    // 0x18-byte entry through slot 8.  Slot 5 reports the plug-in item limit.
    HRESULT ReplaceThumbnails(std::span<const ThumbnailData> thumbnails) noexcept;
    HRESULT ClearThumbnails() noexcept;
    [[nodiscard]] std::optional<std::wstring> MetadataValue(
        std::string_view name) const;
    HRESULT SetMetadataValue(std::string_view name,
                             std::wstring_view value) noexcept;
    // CScanGainDlg/FUN_004A50B5 and FUN_00484009 call metadata slot 6
    // directly.  They deliberately do not use the capability-bit guard of
    // the File Properties save path (FUN_004ADD9C): several shipped comment
    // writers, notably the FLAC reader, report capabilities 0x2 yet accept
    // ReplayGain comment updates through this slot.
    HRESULT SetMetadataValueDirect(std::string_view name,
                                   std::wstring_view value) noexcept;

private:
    struct Impl;
    explicit LegacyReaderSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class PluginManager;
};

// An initialized ISoundDecoder created through IDecoderCreator.  The slot
// contract comes from the decoder wrapper at 004E3A9C..004E3EB5: slot 3
// negotiates encoded/output WAVEFORMATEX values, slot 4 reports the staging
// buffer sizes, slots 5/6 are input/output readiness predicates, slots 7/8
// push/pull an ISoundBuffer and slot 9 resets after a seek.
class LegacyDecoderSession {
public:
    ~LegacyDecoderSession();
    LegacyDecoderSession(const LegacyDecoderSession&) = delete;
    LegacyDecoderSession& operator=(const LegacyDecoderSession&) = delete;

    HRESULT BufferSizes(DWORD& input_bytes,
                        DWORD& output_bytes) const noexcept;
    HRESULT NeedsInput() const noexcept;
    HRESULT OutputAvailable() const noexcept;
    HRESULT PushInput(std::span<const std::byte> encoded) noexcept;
    HRESULT ReadOutput(size_t requested_bytes, std::vector<std::byte>& pcm,
                       bool& end_of_stream) noexcept;
    HRESULT Reset() noexcept;
    HRESULT QueryInterface(REFIID iid, void** result) const noexcept;
    [[nodiscard]] const WAVEFORMATEX& OutputFormat() const noexcept;
    [[nodiscard]] const std::filesystem::path& ModulePath() const noexcept;

private:
    struct Impl;
    explicit LegacyDecoderSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class PluginManager;
};

// ISoundEncoder returned by IEncoderCreator.  Open mirrors FUN_004CD30C:
// encoders with the stream extension receive an IStream opened with 0x1022;
// older encoders receive the destination path through their slot 3.
class LegacyEncoderSession {
public:
    ~LegacyEncoderSession();
    LegacyEncoderSession(const LegacyEncoderSession&) = delete;
    LegacyEncoderSession& operator=(const LegacyEncoderSession&) = delete;

    HRESULT Open(const std::filesystem::path& destination,
                 const WAVEFORMATEX& pcm_format) noexcept;
    // CConvertDlg::CWorkThread::Run (00412723) invokes these in order after
    // FUN_004CD30C opens the destination: slot 4 starts the stream, slot 6
    // consumes an ISoundBuffer, and slot 5 finalizes it.  WritePcm supplies
    // the recovered host buffer ABI so callers never pass an untyped object.
    HRESULT Start() noexcept;
    HRESULT WritePcm(std::span<const std::byte> pcm,
                     DWORD* encoded_bytes = nullptr) noexcept;
    HRESULT Finalize() noexcept;
    // ISoundEncoder slot 7 may change with the creator's configuration (AAC
    // ADTS versus MP4, or an external command-line encoder preset).
    [[nodiscard]] std::wstring FileExtension() const;
    // FUN_004121C4 copies nonempty metadata except replaygain_* before Start.
    HRESULT SetMetadata(std::span<const MetadataEntry> entries) noexcept;
    HRESULT QueryInterface(REFIID iid, void** result) const noexcept;
    [[nodiscard]] const std::filesystem::path& ModulePath() const noexcept;

private:
    struct Impl;
    explicit LegacyEncoderSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class PluginManager;
};

// Session returned by the fourth Sound AddIn category.  Shipped
// ttp_lrcsh*.dll modules implement this category; it is not an audio encoder.
class LegacyLyricSearchSession {
public:
    ~LegacyLyricSearchSession();
    LegacyLyricSearchSession(const LegacyLyricSearchSession&) = delete;
    LegacyLyricSearchSession& operator=(const LegacyLyricSearchSession&) = delete;

    HRESULT QueryInterface(REFIID iid, void** result) const noexcept;
    [[nodiscard]] const std::filesystem::path& ModulePath() const noexcept;

private:
    struct Impl;
    explicit LegacyLyricSearchSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class PluginManager;
};

class PluginManager {
public:
    PluginManager() = default;
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    HRESULT Load(const std::filesystem::path& directory);
    void Shutdown() noexcept;

    [[nodiscard]] const std::vector<PluginInfo>& Plugins() const noexcept {
        return plugins_;
    }
    [[nodiscard]] const std::vector<ReaderFormat>& ReaderFormats() const noexcept {
        return reader_formats_;
    }
    [[nodiscard]] const std::vector<DecoderFactoryInfo>&
        DecoderFactories() const noexcept { return decoder_factory_info_; }
    [[nodiscard]] const std::vector<EncoderFactoryInfo>&
        EncoderFactories() const noexcept { return encoder_factory_info_; }
    [[nodiscard]] const std::vector<LyricSearchProviderInfo>&
        LyricSearchProviders() const noexcept { return lyric_provider_info_; }
    [[nodiscard]] size_t RegisteredPluginCount() const noexcept {
        return loaded_modules_.size();
    }
    // Retains the loaded add-ins, reader creators and their DLL references in
    // an independent manager.  Long-running modeless jobs use this snapshot
    // so closing their window never has to join a potentially blocked private
    // decoder call, while application shutdown can still release the primary
    // sound library without invalidating code or interface pointers in use by
    // the worker.
    [[nodiscard]] std::shared_ptr<PluginManager>
        RetainForBackground() const noexcept;
    [[nodiscard]] bool HasReaderForPath(const std::filesystem::path& path) const;
    [[nodiscard]] std::unique_ptr<LegacyReaderSession> OpenReader(
        const std::filesystem::path& path, HRESULT* result = nullptr,
        std::wstring* diagnostic = nullptr) const;
    // Opens the same recovered reader ABI around a read/write IStream.  The
    // metadata slot-6 implementations in the shipped FLAC/APE readers write
    // through the stream and return STG_E_ACCESSDENIED when handed the normal
    // playback-only stream.
    [[nodiscard]] std::unique_ptr<LegacyReaderSession> OpenReaderForMetadata(
        const std::filesystem::path& path, HRESULT* result = nullptr,
        std::wstring* diagnostic = nullptr) const;
    // Opens a reader selected by logical_path around an already-created
    // stream.  Archive members use this recovered FUN_004E323B boundary: the
    // member extension selects the creator, while reader slot 3 consumes the
    // in-memory IStream exactly as it does for a physical file.
    [[nodiscard]] std::unique_ptr<LegacyReaderSession> OpenReader(
        const std::filesystem::path& logical_path, IStream* stream,
        HRESULT* result = nullptr, std::wstring* diagnostic = nullptr) const;

    // FUN_004CD045 selects the first IDecoderCreator accepting the encoded
    // WAVE subtype, creates its decoder and invokes decoder slot 3 with the
    // encoded and requested-output formats.  The session owns the original
    // fixed 0x28-byte output buffer and exposes the negotiated PCM/IEEE-float
    // format through OutputFormat().
    [[nodiscard]] bool HasDecoderForFormat(
        const WAVEFORMATEX& format) const noexcept;
    [[nodiscard]] std::unique_ptr<LegacyDecoderSession> OpenDecoder(
        const WAVEFORMATEX& encoded_format,
        WORD requested_output_tag = WAVE_FORMAT_PCM,
        HRESULT* result = nullptr,
        std::wstring* diagnostic = nullptr) const;

    // FUN_004CD21E and FUN_004CD28F: show a creator's configuration UI or
    // create the selected encoder instance.
    HRESULT ConfigureEncoder(size_t index, HWND parent,
                             std::wstring* diagnostic = nullptr) const noexcept;
    [[nodiscard]] std::unique_ptr<LegacyEncoderSession> CreateEncoder(
        size_t index, HRESULT* result = nullptr,
        std::wstring* diagnostic = nullptr) const;

    // FUN_004CD44E.  sound_library_host is the private host interface passed
    // to the shipped lyric provider; nullptr is permitted for providers that
    // do not require it.
    [[nodiscard]] std::unique_ptr<LegacyLyricSearchSession> CreateLyricSearch(
        size_t index, void* context, void* sound_library_host,
        HRESULT* result = nullptr,
        std::wstring* diagnostic = nullptr) const;

private:
    [[nodiscard]] std::unique_ptr<LegacyReaderSession> OpenReaderWithFlags(
        const std::filesystem::path& logical_path, IStream* stream,
        DWORD open_flags, HRESULT* result,
        std::wstring* diagnostic) const;

    struct ReaderFactory {
        ReaderFormat format;
        size_t module_index{};
        DWORD enumeration_index{};
        mutable void* provider{};
    };

    struct DecoderFactory {
        DecoderFactoryInfo info;
        size_t module_index{};
        DWORD enumeration_index{};
        mutable void* provider{};
    };

    struct EncoderFactory {
        EncoderFactoryInfo info;
        size_t module_index{};
        DWORD enumeration_index{};
        mutable void* provider{};
    };

    struct LyricProvider {
        LyricSearchProviderInfo info;
        size_t module_index{};
        DWORD enumeration_index{};
        mutable void* provider{};
    };

    struct EncoderDependencyState;
    struct LoadedModule {
        HMODULE module{};
        void* addin{};
        std::shared_ptr<EncoderDependencyState> encoder_dependencies;
    };

    HRESULT PrepareEncoderDependencies(size_t module_index,
                                       std::wstring* diagnostic = nullptr) const noexcept;

    [[nodiscard]] void* ResolveCategoryInterface(
        size_t module_index, DWORD enumeration_index,
        REFIID category, void*& cached) const noexcept;

    std::vector<PluginInfo> plugins_;
    std::vector<ReaderFormat> reader_formats_;
    std::vector<ReaderFactory> reader_factories_;
    std::vector<DecoderFactoryInfo> decoder_factory_info_;
    std::vector<DecoderFactory> decoder_factories_;
    std::vector<EncoderFactoryInfo> encoder_factory_info_;
    std::vector<EncoderFactory> encoder_factories_;
    std::vector<LyricSearchProviderInfo> lyric_provider_info_;
    std::vector<LyricProvider> lyric_providers_;
    std::vector<LoadedModule> loaded_modules_;
    mutable std::mutex registry_mutex_;
};

} // namespace ttplayer::plugins
