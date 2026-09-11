#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/audio/playback_clock.h"
#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/cue_sheet.h"
#include "ttplayer/audio/native_output_contract.h"
#include "ttplayer/audio/asio_sink.h"
#include "ttplayer/audio/kernel_streaming_sink.h"
#include "ttplayer/audio/mp3pro_source.h"
#include "ttplayer/audio/pcm_output_transform.h"
#include "ttplayer/audio/replay_gain_policy.h"
#include "ttplayer/audio/replay_gain_scanner.h"
#include "ttplayer/audio/winamp_dsp.h"
#include "ttplayer/plugins/plugin_manager.h"
#include "../ui/output_devices.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmreg.h>
#include <propvarutil.h>
#include <propkey.h>
#include <propsys.h>
#include <shlobj.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <wrl/client.h>
#include <dsound.h>

namespace ttplayer::audio {
namespace {

// Decoder creation is synchronous from the caller's point of view, but it is
// performed on the playback worker because legacy reader plug-ins and Media
// Foundation can enter third-party/file-system code.  These budgets keep that
// synchronous contract from turning a stalled reader into an indefinitely
// ghosted player window.  A timed-out worker remains owned by AudioEngine and
// is joined once it really exits; it is never detached or forcibly killed.
constexpr auto kOpenWaitBudget = std::chrono::seconds(4);
constexpr auto kStopWaitBudget = std::chrono::milliseconds(1500);
using Microsoft::WRL::ComPtr;

constexpr size_t kOutputBufferCount = 4;
constexpr GUID kIidDirectSoundBuffer8{
    0x6825a449, 0x7524, 0x4d82,
    {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}};
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kFirstAudioStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

WORD PcmProcessingTag(const WAVEFORMATEX& format) noexcept {
    if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return format.wFormatTag;
    const auto& extended =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    static constexpr std::array<BYTE, 8> wave_tail{
        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    if (extended.SubFormat.Data2 != 0 ||
        extended.SubFormat.Data3 != 0x0010 ||
        !std::equal(wave_tail.begin(), wave_tail.end(),
                    extended.SubFormat.Data4))
        return format.wFormatTag;
    if (extended.SubFormat.Data1 == WAVE_FORMAT_PCM ||
        extended.SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT)
        return static_cast<WORD>(extended.SubFormat.Data1);
    return format.wFormatTag;
}

const std::wstring kSupportedPattern =
    L"*.mp3;*.mp2;*.mp1;*.mpa;*.mp3pro;"
    L"*.wav;*.wma;*.wmv;*.asf;*.cda;*.cue;*.mid;*.midi;*.rmi;"
    L"*.aif;*.aifc;*.aiff;*.au;*.snd;"
    L"*.m4a;*.mp4;*.aac;*.aa;*.ac3;*.a52;*.dts;*.dtswav;*.ape;*.mac;"
    L"*.flac;*.fla;*.tta;*.mod;*.far;*.it;*.s3m;*.stm;*.mtm;*.umx;*.xm;"
    L"*.mpc;*.mp+;*.ogg;*.ra;*.rm;*.ram;*.rmvb;*.tak;*.vqf";

uint16_t ReadBe16(const std::byte* value) {
    return static_cast<uint16_t>((std::to_integer<uint8_t>(value[0]) << 8) |
                                 std::to_integer<uint8_t>(value[1]));
}

uint32_t ReadBe32(const std::byte* value) {
    return (static_cast<uint32_t>(std::to_integer<uint8_t>(value[0])) << 24) |
           (static_cast<uint32_t>(std::to_integer<uint8_t>(value[1])) << 16) |
           (static_cast<uint32_t>(std::to_integer<uint8_t>(value[2])) << 8) |
           static_cast<uint32_t>(std::to_integer<uint8_t>(value[3]));
}

uint64_t ReadBe64(const std::byte* value) {
    return (static_cast<uint64_t>(ReadBe32(value)) << 32) | ReadBe32(value + 4);
}

bool FourCc(const std::byte* value, const char* expected) {
    return std::memcmp(value, expected, 4) == 0;
}

std::wstring LowerExtension(const std::filesystem::path& path) {
    auto value = path.extension().wstring();
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

bool IsMidiPath(const std::filesystem::path& path) {
    const auto extension = LowerExtension(path);
    return extension == L".mid" || extension == L".midi" || extension == L".rmi";
}

std::wstring HResultMessage(std::wstring_view operation, HRESULT result) {
    wchar_t* system_message{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0,
        reinterpret_cast<wchar_t*>(&system_message), 0, nullptr);
    std::wstring message(operation);
    message += L" failed (0x";
    std::wostringstream code;
    code << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0')
         << static_cast<uint32_t>(result);
    message += code.str();
    message += L")";
    if (length != 0 && system_message) {
        size_t detail_length = std::wcslen(system_message);
        while (detail_length != 0 &&
               (system_message[detail_length - 1] == L'\r' ||
                system_message[detail_length - 1] == L'\n'))
            system_message[--detail_length] = L'\0';
        message += L": ";
        message += system_message;
    }
    if (system_message) LocalFree(system_message);
    return message;
}

ComPtr<IStream> MemoryStream(const std::vector<unsigned char>& bytes) {
    ComPtr<IStream> stream;
    const SIZE_T allocation_size = std::max<size_t>(bytes.size(), 1);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, allocation_size);
    if (!memory) return stream;
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return stream;
    }
    if (!bytes.empty()) std::memcpy(destination, bytes.data(), bytes.size());
    GlobalUnlock(memory);
    IStream* raw{};
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &raw)) || !raw) {
        GlobalFree(memory);
        return stream;
    }
    stream.Attach(raw);
    ULARGE_INTEGER exact_size{};
    exact_size.QuadPart = bytes.size();
    if (FAILED(stream->SetSize(exact_size))) {
        stream.Reset();
        return stream;
    }
    LARGE_INTEGER beginning{};
    if (FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr))) stream.Reset();
    return stream;
}

std::wstring CodecNameFromTag(uint32_t tag, const std::filesystem::path& path) {
    switch (tag) {
    case WAVE_FORMAT_PCM: return L"PCM";
    case WAVE_FORMAT_IEEE_FLOAT: return L"IEEE Float";
    case WAVE_FORMAT_MPEG: return L"MPEG Audio";
    case WAVE_FORMAT_MPEGLAYER3: return L"MPEG Layer-3";
    case 0x0160: return L"Windows Media Audio 1";
    case 0x0161: return L"Windows Media Audio 2";
    case 0x0162: return L"Windows Media Audio 9 Professional";
    case 0x0163: return L"Windows Media Audio 9 Lossless";
    case 0x1610: return L"AAC";
    case 0x2000: return L"AC-3";
    case 0x2001: return L"DTS";
    case 0xf1ac: return L"FLAC";
    default: break;
    }
    const auto extension = LowerExtension(path);
    if (extension == L".aac" || extension == L".m4a" || extension == L".m4b" ||
        extension == L".mp4") return L"AAC";
    if (extension == L".flac") return L"FLAC";
    if (extension == L".ogg" || extension == L".oga") return L"Vorbis/Ogg";
    if (extension == L".wma" || extension == L".wmv" || extension == L".asf")
        return L"Windows Media Audio";
    if (extension == L".mp3" || extension == L".mp2" || extension == L".mp1" ||
        extension == L".mpa" || extension == L".mp3pro") return L"MPEG Audio";
    return L"Audio";
}

bool MetadataKeyEquals(std::wstring_view value, const wchar_t* expected) {
    return _wcsicmp(std::wstring(value).c_str(), expected) == 0;
}

std::optional<double> MetadataNumber(std::wstring_view value) {
    const std::wstring terminated(value);
    wchar_t* end{};
    const double number = wcstod(terminated.c_str(), &end);
    if (end == terminated.c_str() || !std::isfinite(number)) return std::nullopt;
    return number;
}

class MediaFoundationSource final : public DecodedAudioSource {
public:
    explicit MediaFoundationSource(HMODULE ttpcomm = nullptr)
        : ttpcomm_(ttpcomm) {}

    bool Open(const std::filesystem::path& path,
              const PlaybackOptions&) override {
        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(result)) {
            attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
            ArchiveMemberPath member;
            if (ParseArchiveMemberPath(path.native(), member)) {
                try {
                    archive_stream_ = MemoryStream(ReadArchiveMember(member, ttpcomm_));
                    if (!archive_stream_) {
                        result = HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY);
                    } else {
                        result = MFCreateMFByteStreamOnStream(
                            archive_stream_.Get(), &archive_byte_stream_);
                    }
                    if (SUCCEEDED(result)) {
                        result = MFCreateSourceReaderFromByteStream(
                            archive_byte_stream_.Get(), attributes.Get(), &reader_);
                    }
                } catch (const std::exception&) {
                    result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                }
            } else {
                result = MFCreateSourceReaderFromURL(
                    path.c_str(), attributes.Get(), &reader_);
            }
        }
        if (FAILED(result)) return Fail(L"MFCreateSourceReader", result);

        reader_->SetStreamSelection(kAllStreams, FALSE);
        result = reader_->SetStreamSelection(kFirstAudioStream, TRUE);
        if (FAILED(result)) return Fail(L"SetStreamSelection", result);

        ComPtr<IMFMediaType> native_type;
        result = reader_->GetNativeMediaType(kFirstAudioStream, 0,
                                             &native_type);
        if (FAILED(result)) return Fail(L"GetNativeMediaType", result);
        GUID native_subtype{};
        native_type->GetGUID(MF_MT_SUBTYPE, &native_subtype);
        UINT32 encoded_bytes_per_second{};
        native_type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                               &encoded_bytes_per_second);

        ComPtr<IMFMediaType> requested;
        result = MFCreateMediaType(&requested);
        if (FAILED(result)) return Fail(L"MFCreateMediaType", result);
        requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        UINT32 channels{};
        if (SUCCEEDED(native_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)))
            requested->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);

        result = reader_->SetCurrentMediaType(kFirstAudioStream,
                                               nullptr, requested.Get());
        if (FAILED(result)) {
            requested.Reset();
            MFCreateMediaType(&requested);
            requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            result = reader_->SetCurrentMediaType(kFirstAudioStream,
                                                   nullptr, requested.Get());
        }
        if (FAILED(result)) return Fail(L"SetCurrentMediaType(PCM)", result);

        ComPtr<IMFMediaType> output_type;
        result = reader_->GetCurrentMediaType(kFirstAudioStream,
                                              &output_type);
        if (FAILED(result)) return Fail(L"GetCurrentMediaType", result);
        UINT32 sample_rate{}, output_channels{}, bits{}, block_align{}, output_bps{};
        output_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate);
        output_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &output_channels);
        output_type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        output_type->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &block_align);
        output_type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &output_bps);
        if (sample_rate == 0 || output_channels == 0 || bits == 0) {
            error_ = L"Media Foundation returned an incomplete PCM format";
            return false;
        }
        if (block_align == 0) block_align = output_channels * ((bits + 7) / 8);
        if (output_bps == 0) output_bps = sample_rate * block_align;
        wave_format_.wFormatTag = WAVE_FORMAT_PCM;
        wave_format_.nChannels = static_cast<WORD>(output_channels);
        wave_format_.nSamplesPerSec = sample_rate;
        wave_format_.nAvgBytesPerSec = output_bps;
        wave_format_.nBlockAlign = static_cast<WORD>(block_align);
        wave_format_.wBitsPerSample = static_cast<WORD>(bits);
        wave_format_.cbSize = 0;

        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(reader_->GetPresentationAttribute(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                MF_PD_DURATION, &duration)) &&
            duration.vt == VT_UI8) {
            duration_ = std::chrono::milliseconds(duration.uhVal.QuadPart / 10000);
        }
        PropVariantClear(&duration);

        display_format_.format_tag = static_cast<uint16_t>(native_subtype.Data1);
        display_format_.channels = wave_format_.nChannels;
        display_format_.sample_rate = wave_format_.nSamplesPerSec;
        display_format_.bytes_per_second = encoded_bytes_per_second != 0
            ? encoded_bytes_per_second : wave_format_.nAvgBytesPerSec;
        display_format_.block_align = wave_format_.nBlockAlign;
        display_format_.bits_per_sample = wave_format_.wBitsPerSample;
        display_format_.codec_name = CodecNameFromTag(native_subtype.Data1, path);
        ArchiveMemberPath member;
        if (!ParseArchiveMemberPath(path.native(), member)) {
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(SHGetPropertyStoreFromParsingName(
                    path.c_str(), nullptr, GPS_DEFAULT,
                    IID_PPV_ARGS(&properties))) && properties) {
                ReadProperty(properties.Get(), PKEY_Title, L"title", metadata_.title);
                ReadProperty(properties.Get(), PKEY_Music_Artist, L"artist",
                             metadata_.artist);
                ReadProperty(properties.Get(), PKEY_Music_AlbumTitle, L"album",
                             metadata_.album);
            }
        }
        return true;
    }

    bool Read(size_t requested_bytes, std::vector<std::byte>& output,
              bool& end_of_stream) override {
        output.clear();
        end_of_stream = false;
        if (!reader_ || wave_format_.nBlockAlign == 0) {
            error_ = L"Media Foundation source is not open";
            return false;
        }
        requested_bytes = std::max<size_t>(requested_bytes,
                                           wave_format_.nBlockAlign);
        requested_bytes -= requested_bytes % wave_format_.nBlockAlign;

        // IMFSourceReader returns whole samples and ignores the byte count
        // requested by TTPlayer's reader contract.  Retain the excess so CUE
        // frame positioning and offline segment limits never consume audio
        // belonging to the following sub-track.
        unsigned empty_samples{};
        while (output.size() < requested_bytes) {
            if (pending_offset_ < pending_.size()) {
                const size_t count = std::min(
                    requested_bytes - output.size(),
                    pending_.size() - pending_offset_);
                output.insert(output.end(),
                              pending_.begin() +
                                  static_cast<ptrdiff_t>(pending_offset_),
                              pending_.begin() + static_cast<ptrdiff_t>(
                                  pending_offset_ + count));
                pending_offset_ += count;
                if (pending_offset_ == pending_.size()) {
                    pending_.clear();
                    pending_offset_ = 0;
                }
                continue;
            }
            if (source_end_) break;

            DWORD flags{};
            ComPtr<IMFSample> sample;
            const HRESULT result = reader_->ReadSample(
                kFirstAudioStream, 0, nullptr, &flags, nullptr, &sample);
            if (FAILED(result))
                return Fail(L"IMFSourceReader::ReadSample", result);
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                error_ = L"Media Foundation decoder reported a stream error";
                return false;
            }
            source_end_ = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            if (!sample) {
                if (source_end_) break;
                if (++empty_samples >= 64) {
                    error_ = L"Media Foundation returned no audio samples";
                    return false;
                }
                continue;
            }
            empty_samples = 0;
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
                error_ = L"Unable to obtain decoded audio buffer";
                return false;
            }
            BYTE* bytes{};
            DWORD current_length{};
            if (FAILED(buffer->Lock(&bytes, nullptr, &current_length))) {
                error_ = L"Unable to lock decoded audio buffer";
                return false;
            }
            if (current_length % wave_format_.nBlockAlign != 0) {
                buffer->Unlock();
                error_ = L"Media Foundation returned a partial PCM frame";
                return false;
            }
            pending_.resize(current_length);
            if (current_length != 0)
                std::memcpy(pending_.data(), bytes, current_length);
            buffer->Unlock();
            pending_offset_ = 0;
        }
        end_of_stream = source_end_ && pending_.empty();
        return true;
    }

    bool Seek(std::chrono::milliseconds position) override {
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_I8;
        value.hVal.QuadPart = position.count() * 10000;
        const HRESULT result = reader_->SetCurrentPosition(GUID_NULL, value);
        PropVariantClear(&value);
        if (FAILED(result)) return Fail(L"IMFSourceReader::SetCurrentPosition", result);
        pending_.clear();
        pending_offset_ = 0;
        source_end_ = false;
        return true;
    }

    [[nodiscard]] const WAVEFORMATEX& OutputFormat() const override {
        return wave_format_;
    }
    [[nodiscard]] AudioFormat DisplayFormat() const override { return display_format_; }
    [[nodiscard]] std::chrono::milliseconds Duration() const override { return duration_; }
    [[nodiscard]] std::wstring Error() const override { return error_; }
    [[nodiscard]] AudioMetadata Metadata() const override { return metadata_; }

private:
    void ReadProperty(IPropertyStore* store, REFPROPERTYKEY key,
                      const wchar_t* name, std::wstring& destination) {
        PROPVARIANT value;
        PropVariantInit(&value);
        PWSTR text{};
        if (SUCCEEDED(store->GetValue(key, &value)) &&
            SUCCEEDED(PropVariantToStringAlloc(value, &text)) && text && *text) {
            destination = text;
            metadata_.entries.emplace_back(name, destination);
        }
        if (text) CoTaskMemFree(text);
        PropVariantClear(&value);
    }

    bool Fail(std::wstring_view operation, HRESULT result) {
        error_ = HResultMessage(operation, result);
        return false;
    }

    ComPtr<IMFSourceReader> reader_;
    ComPtr<IStream> archive_stream_;
    ComPtr<IMFByteStream> archive_byte_stream_;
    HMODULE ttpcomm_{};
    WAVEFORMATEX wave_format_{};
    AudioFormat display_format_{};
    AudioMetadata metadata_;
    std::chrono::milliseconds duration_{};
    std::vector<std::byte> pending_;
    size_t pending_offset_{};
    bool source_end_{};
    std::wstring error_;
};

class LegacyPluginSource final : public DecodedAudioSource {
public:
    LegacyPluginSource(const plugins::PluginManager& manager, HMODULE ttpcomm)
        : manager_(manager), ttpcomm_(ttpcomm) {}

    bool Open(const std::filesystem::path& path,
              const PlaybackOptions&) override {
        HRESULT result{};
        std::wstring diagnostic;
        ArchiveMemberPath member;
        if (ParseArchiveMemberPath(path.native(), member)) {
            try {
                const auto bytes = ReadArchiveMember(member, ttpcomm_);
                const auto stream = MemoryStream(bytes);
                if (!stream) {
                    return Fail(L"CreateStreamOnHGlobal",
                                HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY));
                }
                reader_ = manager_.OpenReader(path, stream.Get(), &result,
                                              &diagnostic);
            } catch (const std::exception&) {
                return Fail(L"Archive member reader",
                            HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            }
        } else {
            reader_ = manager_.OpenReader(path, &result, &diagnostic);
        }
        if (!reader_) return Fail(diagnostic.empty()
            ? std::wstring_view(L"IPlayerSoundReader::Open")
            : std::wstring_view(diagnostic), result);
        const WAVEFORMATEX& wave_format = reader_->Format();
        if (wave_format.nChannels == 0 || wave_format.nSamplesPerSec == 0 ||
            wave_format.nAvgBytesPerSec == 0 || wave_format.nBlockAlign == 0) {
            error_ = L"Legacy sound reader returned an invalid PCM format";
            return false;
        }
        display_format_.format_tag = wave_format.wFormatTag;
        display_format_.channels = wave_format.nChannels;
        display_format_.sample_rate = wave_format.nSamplesPerSec;
        const DWORD encoded_bits = reader_->EncodedBitsPerSecond();
        display_format_.bytes_per_second = encoded_bits != 0
            ? encoded_bits / 8 : wave_format.nAvgBytesPerSec;
        display_format_.block_align = wave_format.nBlockAlign;
        display_format_.bits_per_sample = wave_format.wBitsPerSample;
        display_format_.codec_name = reader_->CodecName();
        if (display_format_.codec_name.empty())
            display_format_.codec_name = CodecNameFromTag(
                wave_format.wFormatTag, path);
        duration_ = std::chrono::milliseconds(
            reader_->DurationMilliseconds());

        for (const auto& entry : reader_->Metadata()) {
            metadata_.entries.emplace_back(entry.name, entry.value);
            if (MetadataKeyEquals(entry.name, L"title")) metadata_.title = entry.value;
            else if (MetadataKeyEquals(entry.name, L"artist") ||
                     MetadataKeyEquals(entry.name, L"author")) metadata_.artist = entry.value;
            else if (MetadataKeyEquals(entry.name, L"album")) metadata_.album = entry.value;
            else if (MetadataKeyEquals(entry.name, L"replaygain_track_gain"))
                metadata_.replay_gain_db = MetadataNumber(entry.value);
            else if (MetadataKeyEquals(entry.name, L"replaygain_track_peak"))
                metadata_.replay_peak = MetadataNumber(entry.value);
        }
        const auto read_known = [this](const char* narrow, const wchar_t* wide,
                                       std::wstring& destination) {
            if (!destination.empty()) return;
            if (const auto value = reader_->MetadataValue(narrow); value && !value->empty()) {
                destination = *value;
                metadata_.entries.emplace_back(wide, *value);
            }
        };
        read_known("title", L"title", metadata_.title);
        read_known("artist", L"artist", metadata_.artist);
        read_known("album", L"album", metadata_.album);
        if (!metadata_.replay_gain_db) {
            if (const auto value = reader_->MetadataValue("replaygain_track_gain"))
                metadata_.replay_gain_db = MetadataNumber(*value);
        }
        if (!metadata_.replay_peak) {
            if (const auto value = reader_->MetadataValue("replaygain_track_peak"))
                metadata_.replay_peak = MetadataNumber(*value);
        }
        metadata_.thumbnail_interface = reader_->HasThumbnailInterface();
        metadata_.thumbnail = reader_->Thumbnail();
        return true;
    }

    bool Read(size_t requested_bytes, std::vector<std::byte>& output,
              bool& end_of_stream) override {
        const HRESULT result = reader_->Read(
            requested_bytes, output, end_of_stream);
        return FAILED(result)
            ? Fail(L"IPlayerSoundReader::Read", result) : true;
    }

    bool Seek(std::chrono::milliseconds position) override {
        DWORD requested = static_cast<DWORD>(std::clamp<int64_t>(
            position.count(), 0, std::numeric_limits<DWORD>::max()));
        const HRESULT result = reader_->Seek(requested);
        return FAILED(result)
            ? Fail(L"IPlayerSoundReader::Seek", result) : true;
    }

    [[nodiscard]] const WAVEFORMATEX& OutputFormat() const override {
        static constexpr WAVEFORMATEX empty{};
        return reader_ ? reader_->Format() : empty;
    }
    [[nodiscard]] AudioFormat DisplayFormat() const override {
        return display_format_;
    }
    [[nodiscard]] std::chrono::milliseconds Duration() const override {
        return duration_;
    }
    [[nodiscard]] std::wstring Error() const override { return error_; }
    [[nodiscard]] AudioMetadata Metadata() const override { return metadata_; }

private:
    bool Fail(std::wstring_view operation, HRESULT result) {
        error_ = HResultMessage(operation, result);
        return false;
    }

    const plugins::PluginManager& manager_;
    HMODULE ttpcomm_{};
    std::unique_ptr<plugins::LegacyReaderSession> reader_;
    AudioFormat display_format_{};
    std::chrono::milliseconds duration_{};
    AudioMetadata metadata_;
    std::wstring error_;
};

class BigEndianPcmSource final : public DecodedAudioSource {
public:
    bool Open(const std::filesystem::path& path,
              const PlaybackOptions&) override {
        stream_.open(path, std::ios::binary);
        if (!stream_) {
            error_ = L"Unable to open audio file";
            return false;
        }
        std::array<std::byte, 12> header{};
        stream_.read(reinterpret_cast<char*>(header.data()), header.size());
        if (stream_.gcount() < 12) {
            error_ = L"Audio file header is truncated";
            return false;
        }
        stream_.seekg(0);
        if (FourCc(header.data(), "FORM") &&
            (FourCc(header.data() + 8, "AIFF") || FourCc(header.data() + 8, "AIFC")))
            return OpenAiff(FourCc(header.data() + 8, "AIFC"));
        if (FourCc(header.data(), ".snd")) return OpenAu();
        error_ = L"Not an AIFF/AIFC or AU/SND file";
        return false;
    }

    bool Read(size_t requested_bytes, std::vector<std::byte>& output,
              bool& end_of_stream) override {
        output.clear();
        end_of_stream = bytes_read_ >= data_bytes_;
        if (end_of_stream) return true;
        const uint64_t remaining = data_bytes_ - bytes_read_;
        size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, requested_bytes));
        count -= count % wave_format_.nBlockAlign;
        if (count == 0) count = static_cast<size_t>(std::min<uint64_t>(
            remaining, wave_format_.nBlockAlign));
        output.resize(count);
        stream_.read(reinterpret_cast<char*>(output.data()),
                     static_cast<std::streamsize>(count));
        count = static_cast<size_t>(std::max<std::streamsize>(0, stream_.gcount()));
        count -= count % wave_format_.nBlockAlign;
        output.resize(count);
        bytes_read_ += count;
        if (!little_endian_ && bytes_per_sample_ > 1) {
            for (size_t offset = 0; offset + bytes_per_sample_ <= output.size();
                 offset += bytes_per_sample_) {
                std::reverse(output.begin() + static_cast<ptrdiff_t>(offset),
                             output.begin() + static_cast<ptrdiff_t>(offset + bytes_per_sample_));
            }
        }
        if (signed_eight_bit_ && bytes_per_sample_ == 1) {
            for (auto& value : output) value ^= std::byte{0x80};
        }
        end_of_stream = bytes_read_ >= data_bytes_ || count == 0;
        return true;
    }

    bool Seek(std::chrono::milliseconds position) override {
        uint64_t offset = static_cast<uint64_t>(std::max<int64_t>(0, position.count())) *
                          wave_format_.nAvgBytesPerSec / 1000;
        offset -= offset % wave_format_.nBlockAlign;
        offset = std::min(offset, data_bytes_);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(data_offset_ + offset));
        if (!stream_) {
            error_ = L"Unable to seek audio file";
            return false;
        }
        bytes_read_ = offset;
        return true;
    }

    [[nodiscard]] const WAVEFORMATEX& OutputFormat() const override {
        return wave_format_;
    }
    [[nodiscard]] AudioFormat DisplayFormat() const override { return display_format_; }
    [[nodiscard]] std::chrono::milliseconds Duration() const override { return duration_; }
    [[nodiscard]] std::wstring Error() const override { return error_; }

private:
    bool OpenAiff(bool compressed) {
        stream_.seekg(12);
        bool have_common{}, have_sound{};
        uint32_t frames{};
        while (stream_) {
            std::array<std::byte, 8> chunk{};
            stream_.read(reinterpret_cast<char*>(chunk.data()), chunk.size());
            if (stream_.gcount() != 8) break;
            const uint32_t size = ReadBe32(chunk.data() + 4);
            const auto payload_position = stream_.tellg();
            if (payload_position < 0) break;
            const uint64_t payload = static_cast<uint64_t>(payload_position);
            if (FourCc(chunk.data(), "COMM") && size >= 18) {
                std::vector<std::byte> common(std::min<uint32_t>(size, 32));
                stream_.read(reinterpret_cast<char*>(common.data()), common.size());
                const uint16_t channels = ReadBe16(common.data());
                frames = ReadBe32(common.data() + 2);
                const uint16_t bits = ReadBe16(common.data() + 6);
                const uint16_t exponent = ReadBe16(common.data() + 8) & 0x7fff;
                const uint64_t mantissa = ReadBe64(common.data() + 10);
                const long double rate_value = exponent == 0 || mantissa == 0 ? 0.0L :
                    std::ldexp(static_cast<long double>(mantissa),
                               static_cast<int>(exponent) - 16383 - 63);
                const uint32_t rate = static_cast<uint32_t>(std::llround(rate_value));
                uint16_t tag = WAVE_FORMAT_PCM;
                little_endian_ = false;
                if (compressed) {
                    if (common.size() < 22) {
                        error_ = L"AIFC COMM chunk is truncated";
                        return false;
                    }
                    const auto* compression = common.data() + 18;
                    if (FourCc(compression, "sowt")) little_endian_ = true;
                    else if (FourCc(compression, "fl32") || FourCc(compression, "FL32") ||
                             FourCc(compression, "fl64") || FourCc(compression, "FL64"))
                        tag = WAVE_FORMAT_IEEE_FLOAT;
                    else if (!FourCc(compression, "NONE") && !FourCc(compression, "twos")) {
                        error_ = L"Unsupported AIFC compression";
                        return false;
                    }
                }
                if (channels == 0 || rate == 0 || bits == 0 || bits > 64) {
                    error_ = L"Invalid AIFF audio format";
                    return false;
                }
                bytes_per_sample_ = (bits + 7) / 8;
                wave_format_.wFormatTag = tag;
                wave_format_.nChannels = channels;
                wave_format_.nSamplesPerSec = rate;
                wave_format_.wBitsPerSample = bits;
                wave_format_.nBlockAlign = static_cast<WORD>(channels * bytes_per_sample_);
                wave_format_.nAvgBytesPerSec = rate * wave_format_.nBlockAlign;
                signed_eight_bit_ = tag == WAVE_FORMAT_PCM && bits == 8;
                have_common = true;
            } else if (FourCc(chunk.data(), "SSND") && size >= 8) {
                std::array<std::byte, 8> sound_header{};
                stream_.read(reinterpret_cast<char*>(sound_header.data()), sound_header.size());
                const uint32_t offset = ReadBe32(sound_header.data());
                if (offset > size - 8) {
                    error_ = L"Invalid AIFF SSND offset";
                    return false;
                }
                data_offset_ = payload + 8 + offset;
                data_bytes_ = size - 8 - offset;
                have_sound = true;
            }
            stream_.clear();
            stream_.seekg(static_cast<std::streamoff>(payload + size + (size & 1U)));
        }
        if (!have_common || !have_sound || wave_format_.nAvgBytesPerSec == 0) {
            error_ = L"AIFF file lacks COMM or SSND data";
            return false;
        }
        if (frames != 0)
            data_bytes_ = std::min<uint64_t>(data_bytes_,
                                             static_cast<uint64_t>(frames) *
                                                 wave_format_.nBlockAlign);
        return FinishOpen(L"AIFF");
    }

    bool OpenAu() {
        std::array<std::byte, 24> header{};
        stream_.read(reinterpret_cast<char*>(header.data()), header.size());
        if (stream_.gcount() != 24) {
            error_ = L"AU header is truncated";
            return false;
        }
        data_offset_ = ReadBe32(header.data() + 4);
        uint64_t declared_size = ReadBe32(header.data() + 8);
        const uint32_t encoding = ReadBe32(header.data() + 12);
        const uint32_t rate = ReadBe32(header.data() + 16);
        const uint32_t channels = ReadBe32(header.data() + 20);
        uint16_t bits{};
        uint16_t tag = WAVE_FORMAT_PCM;
        switch (encoding) {
        case 2: bits = 8; break;
        case 3: bits = 16; break;
        case 4: bits = 24; break;
        case 5: bits = 32; break;
        case 6: bits = 32; tag = WAVE_FORMAT_IEEE_FLOAT; break;
        case 7: bits = 64; tag = WAVE_FORMAT_IEEE_FLOAT; break;
        default:
            error_ = L"Unsupported AU encoding (only linear PCM/IEEE float is supported)";
            return false;
        }
        if (data_offset_ < 24 || channels == 0 || channels > 0xffff || rate == 0) {
            error_ = L"Invalid AU audio format";
            return false;
        }
        stream_.seekg(0, std::ios::end);
        const auto file_position = stream_.tellg();
        if (file_position < 0) {
            error_ = L"Unable to determine AU file size";
            return false;
        }
        const uint64_t file_size = static_cast<uint64_t>(file_position);
        if (data_offset_ > file_size) {
            error_ = L"AU data offset is outside the file";
            return false;
        }
        if (declared_size == 0xffffffffU || declared_size > file_size - data_offset_)
            declared_size = file_size - data_offset_;
        data_bytes_ = declared_size;
        bytes_per_sample_ = (bits + 7) / 8;
        little_endian_ = false;
        signed_eight_bit_ = tag == WAVE_FORMAT_PCM && bits == 8;
        wave_format_.wFormatTag = tag;
        wave_format_.nChannels = static_cast<WORD>(channels);
        wave_format_.nSamplesPerSec = rate;
        wave_format_.wBitsPerSample = bits;
        wave_format_.nBlockAlign = static_cast<WORD>(channels * bytes_per_sample_);
        wave_format_.nAvgBytesPerSec = rate * wave_format_.nBlockAlign;
        return FinishOpen(L"AU");
    }

    bool FinishOpen(const wchar_t* container) {
        data_bytes_ -= data_bytes_ % wave_format_.nBlockAlign;
        duration_ = std::chrono::milliseconds(
            data_bytes_ * 1000 / wave_format_.nAvgBytesPerSec);
        display_format_.format_tag = wave_format_.wFormatTag;
        display_format_.channels = wave_format_.nChannels;
        display_format_.sample_rate = wave_format_.nSamplesPerSec;
        display_format_.bytes_per_second = wave_format_.nAvgBytesPerSec;
        display_format_.block_align = wave_format_.nBlockAlign;
        display_format_.bits_per_sample = wave_format_.wBitsPerSample;
        display_format_.codec_name = wave_format_.wFormatTag == WAVE_FORMAT_IEEE_FLOAT
            ? std::wstring(L"IEEE Float ") + container
            : std::wstring(L"PCM ") + container;
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(data_offset_));
        bytes_read_ = 0;
        return static_cast<bool>(stream_);
    }

    std::ifstream stream_;
    WAVEFORMATEX wave_format_{};
    AudioFormat display_format_{};
    std::chrono::milliseconds duration_{};
    uint64_t data_offset_{};
    uint64_t data_bytes_{};
    uint64_t bytes_read_{};
    size_t bytes_per_sample_{};
    bool little_endian_{};
    bool signed_eight_bit_{};
    std::wstring error_;
};

class CdaSource final : public DecodedAudioSource {
public:
    ~CdaSource() override {
        if (drive_ != INVALID_HANDLE_VALUE) CloseHandle(drive_);
    }

    bool Open(const std::filesystem::path& path,
              const PlaybackOptions&) override {
        const auto root = path.root_name().wstring();
        const auto filename = path.filename().wstring();
        if (root.size() != 2 || root[1] != L':' || filename.size() < 10 ||
            _wcsnicmp(filename.c_str(), L"track", 5) != 0 ||
            _wcsicmp(path.extension().c_str(), L".cda") != 0) {
            error_ = L"Invalid CDA path (expected X:\\TrackNN.cda)";
            return false;
        }
        wchar_t* end{};
        const long track = wcstol(filename.c_str() + 5, &end, 10);
        if (track <= 0 || !end || _wcsicmp(end, L".cda") != 0) {
            error_ = L"Invalid CDA track number";
            return false;
        }

        std::wstring device = L"\\\\.\\";
        device += static_cast<wchar_t>(towupper(root[0]));
        device += L":";
        drive_ = CreateFileW(device.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (drive_ == INVALID_HANDLE_VALUE)
            return FailWin32(L"CreateFile(CD drive)");

        CDROM_TOC toc{};
        DWORD returned{};
        if (!DeviceIoControl(drive_, IOCTL_CDROM_READ_TOC, nullptr, 0,
                             &toc, sizeof(toc), &returned, nullptr))
            return FailWin32(L"IOCTL_CDROM_READ_TOC");
        if (track < toc.FirstTrack || track > toc.LastTrack) {
            error_ = L"CDA track is outside the disc TOC";
            return false;
        }
        const size_t index = static_cast<size_t>(track - toc.FirstTrack);
        if ((toc.TrackData[index].Control & 4U) != 0) {
            error_ = L"The selected CD track contains data, not CD-DA audio";
            return false;
        }
        start_lba_ = AddressToLba(toc.TrackData[index].Address);
        end_lba_ = AddressToLba(toc.TrackData[index + 1].Address);
        if (end_lba_ <= start_lba_) {
            error_ = L"Invalid CD track extent in TOC";
            return false;
        }
        current_lba_ = start_lba_;

        wave_format_.wFormatTag = WAVE_FORMAT_PCM;
        wave_format_.nChannels = 2;
        wave_format_.nSamplesPerSec = 44100;
        wave_format_.nAvgBytesPerSec = 176400;
        wave_format_.nBlockAlign = 4;
        wave_format_.wBitsPerSample = 16;
        display_format_.format_tag = WAVE_FORMAT_PCM;
        display_format_.channels = 2;
        display_format_.sample_rate = 44100;
        display_format_.bytes_per_second = 176400;
        display_format_.block_align = 4;
        display_format_.bits_per_sample = 16;
        display_format_.codec_name = L"CD-DA";
        duration_ = std::chrono::milliseconds(
            static_cast<int64_t>(end_lba_ - start_lba_) * 2352 * 1000 /
            wave_format_.nAvgBytesPerSec);
        metadata_.title = L"Track " + std::to_wstring(track);
        return true;
    }

    bool Read(size_t requested_bytes, std::vector<std::byte>& output,
              bool& end_of_stream) override {
        output.clear();
        end_of_stream = current_lba_ >= end_lba_;
        if (end_of_stream) return true;
        constexpr DWORD sector_bytes = 2352;
        DWORD sectors = static_cast<DWORD>(std::max<size_t>(
            1, (requested_bytes + sector_bytes - 1) / sector_bytes));
        sectors = std::min<DWORD>(24, sectors);
        sectors = std::min<DWORD>(sectors,
            static_cast<DWORD>(end_lba_ - current_lba_));
        DWORD returned{};
        DWORD attempted = sectors;
        for (;;) {
            output.resize(static_cast<size_t>(attempted) * sector_bytes);
            RAW_READ_INFO request{};
            // This is the unusual unit required by IOCTL_CDROM_RAW_READ and
            // used by FUN_004DEE53: the byte offset is LBA*2048 even for
            // 2352-byte CDDA output sectors.
            request.DiskOffset.QuadPart =
                static_cast<LONGLONG>(current_lba_) * 2048;
            request.SectorCount = attempted;
            request.TrackMode = CDDA;
            if (DeviceIoControl(drive_, IOCTL_CDROM_RAW_READ, &request,
                                sizeof(request), output.data(),
                                static_cast<DWORD>(output.size()), &returned,
                                nullptr)) break;
            // FUN_004E778B starts at 24 sectors and backs off by four while
            // the request is at least eight sectors. Some drives reject the
            // larger raw transfer even though four-sector reads work.
            if (attempted < 8) {
                output.clear();
                return FailWin32(L"IOCTL_CDROM_RAW_READ");
            }
            attempted -= 4;
        }
        returned -= returned % wave_format_.nBlockAlign;
        output.resize(returned);
        const DWORD completed = returned / sector_bytes;
        current_lba_ += completed;
        end_of_stream = current_lba_ >= end_lba_ || completed == 0;
        return true;
    }

    bool Seek(std::chrono::milliseconds position) override {
        const auto milliseconds = std::max<int64_t>(0, position.count());
        const int64_t sector = milliseconds * 75 / 1000;
        current_lba_ = start_lba_ + static_cast<LONG>(std::min<int64_t>(
            sector, end_lba_ - start_lba_));
        return true;
    }

    [[nodiscard]] const WAVEFORMATEX& OutputFormat() const override {
        return wave_format_;
    }
    [[nodiscard]] AudioFormat DisplayFormat() const override { return display_format_; }
    [[nodiscard]] std::chrono::milliseconds Duration() const override { return duration_; }
    [[nodiscard]] std::wstring Error() const override { return error_; }
    [[nodiscard]] AudioMetadata Metadata() const override { return metadata_; }

private:
    static LONG AddressToLba(const UCHAR address[4]) noexcept {
        const LONG absolute =
            (static_cast<LONG>(address[1]) * 60 + address[2]) * 75 + address[3];
        return std::max<LONG>(0, absolute - 150);
    }

    bool FailWin32(std::wstring_view operation) {
        error_ = HResultMessage(operation, HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    HANDLE drive_{INVALID_HANDLE_VALUE};
    LONG start_lba_{};
    LONG end_lba_{};
    LONG current_lba_{};
    WAVEFORMATEX wave_format_{};
    AudioFormat display_format_{};
    AudioMetadata metadata_;
    std::chrono::milliseconds duration_{};
    std::wstring error_;
};

std::unique_ptr<DecodedAudioSource> MakeBaseSource(
    const std::filesystem::path& path,
    const plugins::PluginManager* plugin_manager, HMODULE ttpcomm) {
    // CPlayList::OpenURL retains a URL as a network stream.  Extension-based
    // AddIn creators expect SHCreateStreamOnFileEx and must never intercept
    // values such as https://host/song.flac or http://host/song.ape.
    if (AudioEngine::IsNetworkMediaLocation(path))
        return std::make_unique<MediaFoundationSource>(ttpcomm);
    const auto extension = LowerExtension(path);
    if (extension == L".cda") return std::make_unique<CdaSource>();
    // FUN_004CBC1F walks registered creators before every built-in fallback.
    if (plugin_manager && plugin_manager->HasReaderForPath(path))
        return std::make_unique<LegacyPluginSource>(*plugin_manager, ttpcomm);
    ArchiveMemberPath archive_member;
    if (ParseArchiveMemberPath(path.native(), archive_member))
        return std::make_unique<MediaFoundationSource>(ttpcomm);
    if (extension == L".aif" || extension == L".aifc" ||
        extension == L".aiff" || extension == L".au" || extension == L".snd")
        return std::make_unique<BigEndianPcmSource>();
    return WrapMp3ProSource(std::make_unique<MediaFoundationSource>(ttpcomm));
}

class CueSegmentSource final : public DecodedAudioSource {
public:
    void SetPaused(bool paused) override { if (inner_) inner_->SetPaused(paused); }
    CueSegmentSource(const plugins::PluginManager* manager, HMODULE ttpcomm,
                     int subtrack)
        : manager_(manager), ttpcomm_(ttpcomm), subtrack_(subtrack) {}

    bool Open(const std::filesystem::path& path,
              const PlaybackOptions& options) override {
        try {
            ArchiveMemberPath member;
            if (ParseArchiveMemberPath(path.native(), member)) {
                sheet_ = CueSheet::LoadFromMemory(
                    ReadArchiveMember(member, ttpcomm_), path);
            } else {
                sheet_ = CueSheet::Load(path);
            }
        } catch (const std::exception&) {
            error_ = L"Unable to parse the CUE sheet";
            return false;
        }
        const CueTrack* selected = subtrack_ > 0
            ? sheet_->FindTrack(subtrack_) : &sheet_->Tracks().front();
        if (!selected) {
            error_ = L"The requested CUE sub-track does not exist";
            return false;
        }
        track_ = *selected;
        inner_ = MakeBaseSource(track_.audio_path, manager_, ttpcomm_);
        if (!inner_->Open(track_.audio_path, options)) {
            error_ = inner_->Error();
            return false;
        }
        wave_format_ = inner_->OutputFormat();
        if (wave_format_.nAvgBytesPerSec == 0 ||
            wave_format_.nBlockAlign == 0) {
            error_ = L"CUE audio source returned an invalid PCM format";
            return false;
        }
        display_format_ = inner_->DisplayFormat();
        metadata_ = inner_->Metadata();
        if (!track_.title.empty()) metadata_.title = track_.title;
        if (!track_.performer.empty()) metadata_.artist = track_.performer;
        if (!sheet_->Title().empty()) metadata_.album = sheet_->Title();
        const auto start = std::chrono::milliseconds(
            static_cast<int64_t>(track_.start_frame * 1000 / 75));
        if (track_.end_frame) {
            if (*track_.end_frame < track_.start_frame) {
                error_ = L"CUE sub-track has an invalid frame range";
                return false;
            }
            duration_frames_ = *track_.end_frame - track_.start_frame;
            duration_ = std::chrono::milliseconds(
                static_cast<int64_t>(*duration_frames_ * 1000 / 75));
        } else {
            if (inner_->Duration() > start)
                duration_ = inner_->Duration() - start;
        }
        if (duration_frames_) {
            if (!FramesToBytes(*duration_frames_, byte_limit_)) {
                error_ = L"CUE sub-track byte length overflow";
                return false;
            }
        } else {
            byte_limit_ = std::numeric_limits<uint64_t>::max();
        }
        bytes_read_ = 0;
        return PositionAtFrame(track_.start_frame);
    }

    bool Read(size_t requested_bytes, std::vector<std::byte>& output,
              bool& end_of_stream) override {
        output.clear();
        if (bytes_read_ >= byte_limit_) {
            end_of_stream = true;
            return true;
        }
        const size_t request = static_cast<size_t>(std::min<uint64_t>(
            requested_bytes, byte_limit_ - bytes_read_));
        const size_t aligned_request = request - request % wave_format_.nBlockAlign;
        if (aligned_request == 0) {
            end_of_stream = true;
            return true;
        }
        while (output.size() < aligned_request) {
            if (pending_offset_ < pending_.size()) {
                const size_t count = std::min(
                    aligned_request - output.size(),
                    pending_.size() - pending_offset_);
                output.insert(output.end(),
                              pending_.begin() +
                                  static_cast<ptrdiff_t>(pending_offset_),
                              pending_.begin() + static_cast<ptrdiff_t>(
                                  pending_offset_ + count));
                pending_offset_ += count;
                if (pending_offset_ == pending_.size()) {
                    pending_.clear();
                    pending_offset_ = 0;
                }
                continue;
            }
            if (inner_end_) break;
            std::vector<std::byte> decoded;
            bool reached_end{};
            if (!inner_->Read(aligned_request - output.size(), decoded,
                              reached_end)) {
                error_ = inner_->Error();
                return false;
            }
            if (decoded.size() % wave_format_.nBlockAlign != 0) {
                error_ = L"CUE audio source returned a partial PCM frame";
                return false;
            }
            inner_end_ = reached_end;
            const size_t count = std::min(
                aligned_request - output.size(), decoded.size());
            output.insert(output.end(), decoded.begin(),
                          decoded.begin() + static_cast<ptrdiff_t>(count));
            if (count != decoded.size()) {
                pending_.assign(
                    decoded.begin() + static_cast<ptrdiff_t>(count),
                    decoded.end());
                pending_offset_ = 0;
            }
            if (decoded.empty() && !reached_end) {
                error_ = L"CUE audio source returned an empty non-final buffer";
                return false;
            }
        }
        bytes_read_ += output.size();
        end_of_stream = bytes_read_ >= byte_limit_ ||
                        (inner_end_ && pending_.empty());
        return true;
    }

    bool Seek(std::chrono::milliseconds position) override {
        position = std::max(position, std::chrono::milliseconds(0));
        std::uint64_t relative_frame =
            static_cast<std::uint64_t>(position.count()) * 75U / 1000U;
        if (duration_frames_)
            relative_frame = std::min(relative_frame, *duration_frames_);
        if (!PositionAtFrame(track_.start_frame + relative_frame))
            return false;
        if (!FramesToBytes(relative_frame, bytes_read_)) {
            error_ = L"CUE seek position overflow";
            return false;
        }
        return true;
    }

    [[nodiscard]] const WAVEFORMATEX& OutputFormat() const override {
        static constexpr WAVEFORMATEX empty{};
        return inner_ ? inner_->OutputFormat() : empty;
    }
    [[nodiscard]] AudioFormat DisplayFormat() const override { return display_format_; }
    [[nodiscard]] std::chrono::milliseconds Duration() const override { return duration_; }
    [[nodiscard]] std::wstring Error() const override { return error_; }
    [[nodiscard]] AudioMetadata Metadata() const override { return metadata_; }

private:
    bool FramesToBytes(std::uint64_t frames, std::uint64_t& bytes) const {
        if (frames > std::numeric_limits<std::uint64_t>::max() /
                         wave_format_.nAvgBytesPerSec)
            return false;
        bytes = frames * wave_format_.nAvgBytesPerSec / 75U;
        bytes -= bytes % wave_format_.nBlockAlign;
        return true;
    }

    bool PositionAtFrame(std::uint64_t absolute_frame) {
        const std::uint64_t whole_seconds = absolute_frame / 75U;
        if (whole_seconds > static_cast<std::uint64_t>(
                                std::numeric_limits<int64_t>::max()) / 1000U) {
            error_ = L"CUE sub-track start exceeds source seek range";
            return false;
        }
        if (!inner_->Seek(std::chrono::milliseconds(
                static_cast<int64_t>(whole_seconds * 1000U)))) {
            error_ = inner_->Error();
            return false;
        }
        pending_.clear();
        pending_offset_ = 0;
        inner_end_ = false;

        std::uint64_t discard{};
        if (!FramesToBytes(absolute_frame % 75U, discard)) {
            error_ = L"CUE sub-track byte offset overflow";
            return false;
        }
        while (discard != 0) {
            std::vector<std::byte> decoded;
            bool reached_end{};
            const size_t request = static_cast<size_t>(std::min<std::uint64_t>(
                discard, std::max<std::uint64_t>(
                    16384U, wave_format_.nBlockAlign)));
            if (!inner_->Read(request, decoded, reached_end)) {
                error_ = inner_->Error();
                return false;
            }
            if (decoded.empty()) {
                error_ = L"CUE sub-track starts beyond the audio stream";
                return false;
            }
            if (decoded.size() % wave_format_.nBlockAlign != 0) {
                error_ = L"CUE audio source returned a partial PCM frame";
                return false;
            }
            const size_t consumed = static_cast<size_t>(
                std::min<std::uint64_t>(discard, decoded.size()));
            discard -= consumed;
            if (consumed != decoded.size()) {
                pending_.assign(
                    decoded.begin() + static_cast<ptrdiff_t>(consumed),
                    decoded.end());
                pending_offset_ = 0;
            }
            inner_end_ = reached_end;
            if (reached_end && discard != 0) {
                error_ = L"CUE sub-track starts beyond the audio stream";
                return false;
            }
        }
        return true;
    }

    const plugins::PluginManager* manager_{};
    HMODULE ttpcomm_{};
    int subtrack_{};
    std::optional<CueSheet> sheet_;
    CueTrack track_;
    std::unique_ptr<DecodedAudioSource> inner_;
    WAVEFORMATEX wave_format_{};
    AudioFormat display_format_{};
    AudioMetadata metadata_;
    std::chrono::milliseconds duration_{};
    std::optional<std::uint64_t> duration_frames_;
    uint64_t byte_limit_{};
    uint64_t bytes_read_{};
    std::vector<std::byte> pending_;
    size_t pending_offset_{};
    bool inner_end_{};
    std::wstring error_;
};

class PlaybackReplayGainCommit {
public:
    bool Prepare(const plugins::PluginManager* manager,
                 const std::filesystem::path& path, int subtrack,
                 bool auto_scan_gain) {
        if (!ShouldAnalyzeReplayGainOnPlayback(auto_scan_gain) ||
            !manager || path.empty() || subtrack != 0 ||
            AudioEngine::IsNetworkMediaLocation(path) ||
            path.native().find(L'|') != std::wstring::npos ||
            _wcsicmp(path.extension().c_str(), L".cda") == 0 ||
            !manager->HasReaderForPath(path)) {
            return false;
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return false;
        library_ = manager->RetainForBackground();
        if (!library_) return false;
        path_ = path;
        return true;
    }

    void MarkPlaybackAccepted() noexcept { playback_accepted_ = true; }

    void Complete(std::optional<ReplayGainScanResult> analysis) noexcept {
        if (analysis && analysis->status == ReplayGainScanStatus::completed)
            analysis_ = std::move(*analysis);
    }

    ~PlaybackReplayGainCommit() {
        if (playback_accepted_ && analysis_ && library_) {
            QueueReplayGainCommit(
                std::move(library_), std::move(path_), std::move(*analysis_));
        }
    }

private:
    std::shared_ptr<plugins::PluginManager> library_;
    std::filesystem::path path_;
    std::optional<ReplayGainScanResult> analysis_;
    bool playback_accepted_{};
};

class RecoveredProcessorChain {
public:
    RecoveredProcessorChain(const WAVEFORMATEX& format,
                            const AudioMetadata& metadata,
                            const PlaybackOptions& options,
                            HMODULE ttpcomm_module,
                            WinampDspChain& winamp_dsp,
                            bool analyze_replay_gain)
        : format_(format), replay_gain_(metadata.replay_gain_db,
                                       metadata.replay_peak),
          ttpcomm_module_(ttpcomm_module),
          winamp_dsp_(winamp_dsp) {
        // The source retains the complete extensible object for format
        // negotiation; this internal scalar copy records only the underlying
        // sample encoding used by the recovered double-PCM processor math.
        format_.wFormatTag = PcmProcessingTag(format);
        if (analyze_replay_gain) {
            replay_gain_analyzer_ = PlaybackReplayGainAnalyzer::Create(
                ttpcomm_module_, format_.nSamplesPerSec, format_.nChannels);
        }
        Update(options);
    }

    // FUN_004B182C is a distinct x86 call boundary in 5.7.9.  Preserve that
    // boundary: VS 18 /O2 otherwise folds this setup into the constructor and
    // the legacy ordinal-103 object subsequently reports zero output frames.
    // Debug and /Ob0 mask the problem, which is why it only appeared in the
    // packaged Release executable.
    __declspec(noinline) void Update(const PlaybackOptions& options) {
        // Playback/@AutoGain is a live processor option. The metadata belongs
        // to this opened sound, so retain it across processor-revision updates
        // and rebuild the multiplier when the checkbox changes. AutoScanGain
        // is fixed when FUN_004B107E constructs this processor;
        // SkipScanGain belongs only to CScanGainDlg::CWorkThread and is never
        // interpreted by a live playback revision.
        replay_gain_.Update(options.auto_gain);
#if defined(_MSC_VER) && defined(_M_IX86)
        if (options.equalizer_profile == -2) {
            if (eq_) DestroyObject(eq_);
            eq_ = nullptr;
        } else if (ttpcomm_module_) {
            bool initialized = true;
            if (!eq_) {
                eq_ = CreateObject(ttpcomm_module_, 103);
                initialized = eq_ && EqInitialize(eq_, format_.nSamplesPerSec,
                                                  format_.nChannels);
            }
            if (eq_ && (!initialized || !EqSet(eq_, options.equalizer_values.data()))) {
                DestroyObject(eq_);
                eq_ = nullptr;
            }
        }
        if (options.surround != surround_amount_) {
            if (surround_) DestroyObject(surround_);
            surround_ = nullptr;
            surround_amount_ = options.surround;
        }
        if (!surround_ && ttpcomm_module_ && options.surround != 0) {
            surround_ = CreateObject(ttpcomm_module_, 104);
            if (surround_ && !SurroundInitialize(
                    surround_, format_.nSamplesPerSec, format_.nChannels,
                    options.surround)) {
                DestroyObject(surround_);
                surround_ = nullptr;
            }
        }
#else
        static_cast<void>(options);
#endif
        winamp_dsp_.Update(options.dsp_folder, options.dsp_modules,
                           options.dsp_parent_window);
    }

    ~RecoveredProcessorChain() {
#if defined(_MSC_VER) && defined(_M_IX86)
        if (surround_) DestroyObject(surround_);
        if (eq_) DestroyObject(eq_);
#endif
    }

    bool Active() const noexcept {
        return replay_gain_.Scale() != 1.0 || eq_ || surround_ ||
               winamp_dsp_.ActiveCount() != 0 || replay_gain_analyzer_;
    }

    bool Process(std::vector<std::byte>& bytes) {
        if (!Active() || bytes.empty()) return true;
        std::vector<double> samples;
        if (!Decode(bytes, samples)) return false;

        // FUN_004B1950 invokes ordinal-101 slot 2 before applying the current
        // ReplayGain multiplier and the remaining processor chain.  Analyzer
        // failure disables only automatic scanning; it must never interrupt
        // audible playback.
        if (replay_gain_analyzer_ && !replay_gain_analyzer_->Analyze(
                samples.data(), samples.size())) {
            replay_gain_analyzer_.reset();
        }

        // FUN_004B1950 precedes FUN_004B1B81. The original double stream is
        // normalized to [-0.5,+0.5], hence these thresholds are not ordinary
        // full-scale ±1 clipping limits.
        if (replay_gain_.Scale() != 1.0) {
            for (auto& sample : samples) {
                double value = sample * replay_gain_.Scale();
                if (value > 0.5)
                    value = (std::tan((value - 0.5) * 2.0) + 1.0) * 0.5;
                else if (value < -0.5)
                    value = std::tan((value + 0.5) * 2.0) * 0.5 - 0.5;
                sample = value;
            }
        }
#if defined(_MSC_VER) && defined(_M_IX86)
        int count = static_cast<int>(std::min<size_t>(
            samples.size(), static_cast<size_t>(std::numeric_limits<int>::max())));
        if (eq_ && !EqProcess(eq_, samples.data(), &count)) return false;
        count = std::clamp(count, 0, static_cast<int>(samples.size()));
        if (format_.nChannels > 1)
            count -= count % format_.nChannels;
        samples.resize(static_cast<size_t>(count));
        if (surround_ && !SurroundProcess(
                surround_, samples.data(), count)) return false;
#endif
        if (winamp_dsp_.ActiveCount() != 0) {
            // FUN_004AC166 converts the processor stream to signed 16-bit
            // immediately before FUN_0042898D, then converts it back when the
            // output device uses another width. Keep ReplayGain/EQ/surround
            // in the recovered double domain and reproduce that ABI boundary.
            std::vector<std::int16_t> pcm16(samples.size());
            for (size_t index = 0; index < samples.size(); ++index) {
                const double sample = std::isfinite(samples[index])
                    ? samples[index] : 0.0;
                pcm16[index] = static_cast<std::int16_t>(std::llround(
                    std::clamp(sample, -0.5, 32767.0 / 65536.0) * 65536.0));
            }
            winamp_dsp_.Process(pcm16, format_.nChannels,
                                static_cast<int>(format_.nSamplesPerSec));
            for (size_t index = 0; index < samples.size(); ++index)
                samples[index] = pcm16[index] / 65536.0;
        }
        return Encode(samples, bytes);
    }

    void Reset() noexcept {
        // FUN_004B16FE destroys +0x38 on a seek.  A partial-track histogram
        // must not be committed as a full-track ReplayGain value.
        if (replay_gain_analyzer_) replay_gain_analyzer_->Cancel();
        replay_gain_analyzer_.reset();
#if defined(_MSC_VER) && defined(_M_IX86)
        if (eq_) EqReset(eq_);
        if (surround_) SurroundReset(surround_);
#endif
    }

    std::vector<std::wstring> TakeDiagnostics() {
        return winamp_dsp_.TakeDiagnostics();
    }

    std::optional<ReplayGainScanResult> FinishReplayGain() noexcept {
        if (!replay_gain_analyzer_) return std::nullopt;
        auto result = replay_gain_analyzer_->Finish();
        replay_gain_analyzer_.reset();
        return result;
    }

private:
    bool Decode(const std::vector<std::byte>& bytes,
                std::vector<double>& samples) const {
        const size_t sample_bytes = (format_.wBitsPerSample + 7U) / 8U;
        if (sample_bytes == 0 || bytes.size() % sample_bytes != 0) return false;
        samples.resize(bytes.size() / sample_bytes);
        const auto* input = reinterpret_cast<const uint8_t*>(bytes.data());
        for (size_t index = 0; index < samples.size(); ++index) {
            const uint8_t* value = input + index * sample_bytes;
            if (format_.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
                format_.wBitsPerSample == 32) {
                float number{};
                std::memcpy(&number, value, sizeof(number));
                samples[index] = static_cast<double>(number) * 0.5;
            } else if (format_.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
                       format_.wBitsPerSample == 64) {
                double number{};
                std::memcpy(&number, value, sizeof(number));
                samples[index] = number * 0.5;
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 8) {
                samples[index] = (static_cast<int>(value[0]) - 128) / 256.0;
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 16) {
                int16_t number{};
                std::memcpy(&number, value, sizeof(number));
                samples[index] = number / 65536.0;
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 24) {
                int32_t number = static_cast<int32_t>(value[0]) |
                    (static_cast<int32_t>(value[1]) << 8) |
                    (static_cast<int32_t>(value[2]) << 16);
                if ((number & 0x800000) != 0) number |= ~0xffffff;
                samples[index] = number / 16777216.0;
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 32) {
                int32_t number{};
                std::memcpy(&number, value, sizeof(number));
                samples[index] = number / 4294967296.0;
            } else {
                return false;
            }
        }
        return true;
    }

    bool Encode(const std::vector<double>& samples,
                std::vector<std::byte>& bytes) const {
        const size_t sample_bytes = (format_.wBitsPerSample + 7U) / 8U;
        bytes.resize(samples.size() * sample_bytes);
        auto* output = reinterpret_cast<uint8_t*>(bytes.data());
        for (size_t index = 0; index < samples.size(); ++index) {
            uint8_t* value = output + index * sample_bytes;
            double sample = std::isfinite(samples[index]) ? samples[index] : 0.0;
            if (format_.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
                format_.wBitsPerSample == 32) {
                const float number = static_cast<float>(std::clamp(sample * 2.0,
                                                                   -1.0, 1.0));
                std::memcpy(value, &number, sizeof(number));
            } else if (format_.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
                       format_.wBitsPerSample == 64) {
                const double number = std::clamp(sample * 2.0, -1.0, 1.0);
                std::memcpy(value, &number, sizeof(number));
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 8) {
                const auto number = static_cast<int>(std::llround(
                    std::clamp(sample, -0.5, 127.0 / 256.0) * 256.0 + 128.0));
                value[0] = static_cast<uint8_t>(std::clamp(number, 0, 255));
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 16) {
                const auto number = static_cast<int16_t>(std::llround(
                    std::clamp(sample, -0.5, 32767.0 / 65536.0) * 65536.0));
                std::memcpy(value, &number, sizeof(number));
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 24) {
                const auto number = static_cast<int32_t>(std::llround(
                    std::clamp(sample, -0.5, 8388607.0 / 16777216.0) *
                    16777216.0));
                value[0] = static_cast<uint8_t>(number);
                value[1] = static_cast<uint8_t>(number >> 8);
                value[2] = static_cast<uint8_t>(number >> 16);
            } else if (format_.wFormatTag == WAVE_FORMAT_PCM &&
                       format_.wBitsPerSample == 32) {
                const auto number = static_cast<int32_t>(std::llround(
                    std::clamp(sample, -0.5, 2147483647.0 / 4294967296.0) *
                    4294967296.0));
                std::memcpy(value, &number, sizeof(number));
            } else {
                return false;
            }
        }
        return true;
    }

#if defined(_MSC_VER) && defined(_M_IX86)
    static void* CreateObject(HMODULE module, WORD ordinal) noexcept {
        const auto entry = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
        if (!entry) return nullptr;
        __try {
            return reinterpret_cast<void* (__cdecl*)()>(entry)();
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static void DestroyObject(void* object) noexcept {
        if (!object) return;
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, int)>(table[0])(object, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static bool EqInitialize(void* object, DWORD rate, WORD channels) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            return reinterpret_cast<unsigned char (__thiscall*)(
                void*, DWORD, WORD)>(table[1])(object, rate, channels) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool EqSet(void* object, const int* values) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, const int*)>(
                table[2])(object, values);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool EqProcess(void* object, double* samples, int* count) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, double*, int)>(
                table[4])(object, samples, *count);
            reinterpret_cast<void (__thiscall*)(void*, double*, int*)>(
                table[5])(object, samples, count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void EqReset(void* object) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*)>(table[6])(object);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static bool SurroundInitialize(void* object, DWORD rate, WORD channels,
                                   int amount) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, DWORD, WORD, int)>(
                table[1])(object, rate, channels, amount);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool SurroundProcess(void* object, double* samples, int count) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*, double*, int)>(
                table[2])(object, samples, count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void SurroundReset(void* object) noexcept {
        __try {
            auto table = *static_cast<void***>(object);
            reinterpret_cast<void (__thiscall*)(void*)>(table[3])(object);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
#endif

    WAVEFORMATEX format_{};
    ReplayGainRuntime replay_gain_;
    std::unique_ptr<PlaybackReplayGainAnalyzer> replay_gain_analyzer_;
    void* eq_{};
    void* surround_{};
    HMODULE ttpcomm_module_{};
    int surround_amount_{};
    WinampDspChain& winamp_dsp_;
};

struct OutputBuffer {
    std::vector<std::byte> data;
    WAVEHDR header{};
    bool queued{};
    uint64_t stream_byte_offset{};
};

int16_t VisualizationPcmSample(const uint8_t* value,
                               const WAVEFORMATEX& format) noexcept {
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
        format.wBitsPerSample == 32) {
        float sample{};
        std::memcpy(&sample, value, sizeof(sample));
        if (!std::isfinite(sample)) return 0;
        if (sample <= -1.0F) return std::numeric_limits<int16_t>::min();
        if (sample >= 1.0F) return std::numeric_limits<int16_t>::max();
        return static_cast<int16_t>(std::lround(sample * 32767.0F));
    }
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
        format.wBitsPerSample == 64) {
        double sample{};
        std::memcpy(&sample, value, sizeof(sample));
        if (!std::isfinite(sample)) return 0;
        if (sample <= -1.0) return std::numeric_limits<int16_t>::min();
        if (sample >= 1.0) return std::numeric_limits<int16_t>::max();
        return static_cast<int16_t>(std::lround(sample * 32767.0));
    }
    if (format.wFormatTag != WAVE_FORMAT_PCM) return 0;
    switch (format.wBitsPerSample) {
    case 8:
        return static_cast<int16_t>((static_cast<int>(value[0]) - 128) << 8);
    case 16: {
        int16_t sample{};
        std::memcpy(&sample, value, sizeof(sample));
        return sample;
    }
    case 24: {
        int32_t sample = static_cast<int32_t>(value[0]) |
            (static_cast<int32_t>(value[1]) << 8) |
            (static_cast<int32_t>(value[2]) << 16);
        if ((sample & 0x800000) != 0) sample |= ~0xffffff;
        return static_cast<int16_t>(sample >> 8);
    }
    case 32: {
        int32_t sample{};
        std::memcpy(&sample, value, sizeof(sample));
        return static_cast<int16_t>(sample >> 16);
    }
    default:
        return 0;
    }
}

void AppendVisualizationFrames(const std::byte* bytes, size_t byte_count,
                               const WAVEFORMATEX& format,
                               VisualizationSamples& samples) {
    const size_t sample_bytes = (format.wBitsPerSample + 7U) / 8U;
    if (!bytes || sample_bytes == 0 || format.nChannels == 0 ||
        format.nBlockAlign < sample_bytes * format.nChannels)
        return;
    if (format.wFormatTag != WAVE_FORMAT_PCM &&
        format.wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
        return;

    const size_t frames = byte_count / format.nBlockAlign;
    const auto* input = reinterpret_cast<const uint8_t*>(bytes);
    for (size_t frame = 0;
         frame < frames && samples.count < VisualizationSamples::capacity;
         ++frame) {
        const uint8_t* value = input + frame * format.nBlockAlign;
        const int16_t left = VisualizationPcmSample(value, format);
        const int16_t right = format.nChannels > 1
            ? VisualizationPcmSample(value + sample_bytes, format) : left;
        samples.left[samples.count] = left;
        samples.right[samples.count] = right;
        ++samples.count;
    }
}

void CALLBACK WaveOutCallback(HWAVEOUT, UINT message, DWORD_PTR instance,
                              DWORD_PTR, DWORD_PTR) {
    if (message == WOM_DONE && instance != 0)
        SetEvent(reinterpret_cast<HANDLE>(instance));
}

bool MciStatus(MCIDEVICEID device, DWORD item, DWORD& value) {
    MCI_STATUS_PARMS status{};
    status.dwItem = item;
    const MCIERROR result = mciSendCommandW(
        device, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT,
        reinterpret_cast<DWORD_PTR>(&status));
    value = status.dwReturn;
    return result == 0;
}

std::wstring MciErrorMessage(std::wstring_view operation, MCIERROR result) {
    wchar_t detail[256]{};
    mciGetErrorStringW(result, detail, static_cast<UINT>(std::size(detail)));
    std::wstring message(operation);
    message += L" failed";
    if (*detail) {
        message += L": ";
        message += detail;
    }
    return message;
}

std::wstring WaveOutErrorMessage(std::wstring_view operation,
                                 MMRESULT result) {
    wchar_t detail[MAXERRORLENGTH]{};
    std::wstring message(operation);
    message += L" failed (";
    message += std::to_wstring(result);
    message += L")";
    if (waveOutGetErrorTextW(result, detail,
                             static_cast<UINT>(std::size(detail))) ==
            MMSYSERR_NOERROR && *detail) {
        message += L": ";
        message += detail;
    }
    return message;
}

using LegacyOutputBackend = OutputBackend;

struct WaveOutSelection {
    UINT device_id{WAVE_MAPPER};
    LegacyOutputBackend requested_backend{LegacyOutputBackend::wave_out};
    GUID direct_sound_guid{};
    bool default_direct_sound{};
    std::wstring error;
};

std::wstring BackendName(LegacyOutputBackend backend) {
    switch (backend) {
    case LegacyOutputBackend::wave_out: return L"waveOut";
    case LegacyOutputBackend::direct_sound: return L"DirectSound";
    case LegacyOutputBackend::kernel_streaming: return L"Kernel Streaming";
    case LegacyOutputBackend::asio: return L"ASIO";
    case LegacyOutputBackend::unknown: return L"unknown legacy";
    }
    return L"unknown legacy";
}

WaveOutSelection ResolveWaveOutSelection(const PlaybackOptions& options) {
    WaveOutSelection result;
    const auto key = ParseOutputDeviceKey(options.device_type);
    if (!key) {
        result.requested_backend = LegacyOutputBackend::unknown;
        result.error =
            L"The configured DeviceType is not a valid 16-byte device key";
        return result;
    }

    result.requested_backend = key->backend;
    if (key->backend == OutputBackend::direct_sound) {
        result.direct_sound_guid = key->identifier;
        result.default_direct_sound = key->default_direct_sound;
        return result;
    }
    if (key->backend == OutputBackend::wave_out) {
        result.device_id = key->ordinal == 0
            ? WAVE_MAPPER : static_cast<UINT>(key->ordinal - 1);
        return result;
    }
    if (key->backend == OutputBackend::kernel_streaming ||
        key->backend == OutputBackend::asio) return result;

    result.error = BackendName(result.requested_backend) +
        L" output cannot be opened because its private TTPlayer ABI has not "
        L"been recovered safely";
    return result;
}
} // namespace

std::unique_ptr<DecodedAudioSource> CreateDecodedAudioSource(
    const std::filesystem::path& path, int subtrack,
    const plugins::PluginManager* plugin_manager, HMODULE ttpcomm_module) {
    const auto extension = LowerExtension(path);
    if (!AudioEngine::IsNetworkMediaLocation(path) && extension == L".cue") {
        return std::make_unique<CueSegmentSource>(
            plugin_manager, ttpcomm_module, subtrack);
    }
    return MakeBaseSource(path, plugin_manager, ttpcomm_module);
}

AudioEngine::AudioEngine() : dsp_chain_(std::make_unique<WinampDspChain>()) {
    // CSound owns a separate CSoundFadeOut worker (004ACD95/004ACE17).
    // Keep one cancellable worker for the engine lifetime so UI fade commands
    // never sleep the window thread and never race a detached device pointer.
    fade_worker_ = std::jthread(
        [this](std::stop_token stop_token) { FadeWorker(stop_token); });
}

AudioEngine::~AudioEngine() {
    {
        std::scoped_lock lock(mutex_);
        fade_shutdown_ = true;
        fade_pending_ = false;
        stop_fade_pending_.store(false, std::memory_order_release);
        ++fade_generation_;
    }
    fade_condition_.notify_all();
    if (fade_worker_.joinable()) {
        fade_worker_.request_stop();
        fade_worker_.join();
    }
    Stop();
    // Stop is deliberately bounded for commands dispatched by the UI thread.
    // Destruction is the ownership boundary: the worker captures `this` and
    // legacy reader sessions borrow PluginManager/module state, so allowing it
    // to outlive AudioEngine would be a use-after-free/unloaded-code hazard.
    if (worker_.joinable()) worker_.join();
}

const std::wstring& AudioEngine::RecoveredFilePattern() { return kSupportedPattern; }

bool AudioEngine::IsRecoveredFormatPath(const std::filesystem::path& path) {
    const auto extension = LowerExtension(path);
    if (extension.empty()) return false;
    const auto needle = L"*" + extension;
    size_t offset = 0;
    while (offset < kSupportedPattern.size()) {
        const size_t end = kSupportedPattern.find(L';', offset);
        const auto item = kSupportedPattern.substr(offset,
            end == std::wstring::npos ? end : end - offset);
        if (_wcsicmp(item.c_str(), needle.c_str()) == 0) return true;
        if (end == std::wstring::npos) break;
        offset = end + 1;
    }
    return false;
}

bool AudioEngine::IsNetworkMediaLocation(
    const std::filesystem::path& path) noexcept {
    const std::wstring_view value = path.native();
    const size_t delimiter = value.find(L"://");
    return delimiter != std::wstring_view::npos && delimiter != 0;
}

void AudioEngine::Configure(const PlaybackOptions& options) {
    std::scoped_lock lock(mutex_);
    const HWND existing_dsp_parent = options_.dsp_parent_window;
    options_ = options;
    // hwndParent is a runtime attachment made after WM_CREATE. Aggregate
    // settings updates must not clear it merely because it is not persisted.
    if (!options_.dsp_parent_window) options_.dsp_parent_window = existing_dsp_parent;
    options_.file_buffer_bytes = std::clamp(options_.file_buffer_bytes, 4096, 4 * 1024 * 1024);
    options_.output_buffer_ms = std::clamp(options_.output_buffer_ms, 100, 10000);
    options_.balance = std::clamp(options_.balance, -100, 100);
    options_.surround = std::clamp(options_.surround, 0, 16);
    options_.ssrc_mode = std::clamp(options_.ssrc_mode, 0, 2);
    options_.dither = std::clamp(options_.dither, 0, 4);
    options_.sound_fade_mode &= 0x1f;
    for (auto& duration : options_.fade_duration)
        duration = std::clamp(duration, 0, 10000);
    options_.track_fade_duration = std::clamp(
        options_.track_fade_duration, 0, 10000);
    for (auto& value : options_.equalizer_values)
        value = std::clamp(value, -12, 12);
    balance_ = options_.balance;
    ++processor_revision_;
    ApplyVolumeLocked();
}

void AudioEngine::SetDspParentWindow(HWND window) {
    std::scoped_lock lock(mutex_);
    if (options_.dsp_parent_window == window) return;
    options_.dsp_parent_window = window;
    ++processor_revision_;
    if (completion_event_) SetEvent(completion_event_);
}

bool AudioEngine::Play(const std::filesystem::path& path, int subtrack) {
    Stop();
    // The worker may have returned immediately after Stop exhausted its
    // bounded wait.  Reap that completed thread before deciding that a new
    // decoder must be rejected.
    static_cast<void>(ReapWorker(std::chrono::milliseconds::zero()));
    // A reader which ignored cancellation is still owned by worker_.  Never
    // assign over that jthread (its assignment operator would wait without a
    // bound), and never start a second decoder against the same borrowed
    // plug-in manager while the first one is still unwinding.
    if (worker_.joinable()) {
        SetError(L"The previous audio decoder is still stopping");
        return false;
    }
    position_ms_ = 0;
    duration_ms_ = 0;
    seek_request_ms_ = -1;
    stop_requested_ = false;
    state_ = PlaybackState::opening;
    backend_ = Backend::none;
    {
        std::scoped_lock lock(mutex_);
        error_.clear();
        diagnostic_.clear();
        format_ = {};
        metadata_ = {};
        open_complete_ = false;
        fade_pending_ = false;
        ++fade_generation_;
        transition_gain_ = (options_.sound_fade_mode & 0x01) != 0 &&
                           options_.fade_duration[
                               RecoveredFadeInDurationIndex(false)] > 0
            ? 0.0F : 1.0F;
        track_gain_ = 1.0F;
    }
    try {
        worker_ = std::jthread([this, path, subtrack] {
            PlaybackWorker(path, subtrack);
        });
    } catch (const std::system_error&) {
        SetError(L"Unable to create the audio playback worker");
        return false;
    }

    // Decoder/reader creation is synchronous in the original play command.
    // Waiting here also makes Duration/Format valid before PlayerWindow
    // updates the playlist row and scrolling skin text.  A private reader is
    // not, however, allowed to keep the window thread here forever.
    std::unique_lock lock(mutex_);
    if (!open_condition_.wait_for(
            lock, kOpenWaitBudget, [this] { return open_complete_; })) {
        lock.unlock();
        RequestStop();
        SetError(L"The audio decoder did not open within 4 seconds");
        static_cast<void>(ReapWorker(std::chrono::milliseconds::zero()));
        return false;
    }
    return state_.load() == PlaybackState::playing ||
           state_.load() == PlaybackState::paused;
}

void AudioEngine::RestoreAfterOutputRestart(
    std::chrono::milliseconds position, bool paused) {
    ClearVisualization();
    HANDLE completion{};
    {
        std::scoped_lock lock(mutex_);
        const auto current = state_.load();
        if (current != PlaybackState::playing &&
            current != PlaybackState::paused) {
            return;
        }

        const int64_t target = std::clamp<int64_t>(
            position.count(), 0, duration_ms_.load());
        QueueSeekLocked(target);

        if (paused) {
            // Stop the audible clock promptly. The owning worker repeats the
            // paused transition after it has reset/refilled at `target`, which
            // also covers native KS/ASIO sinks not stored on AudioEngine.
            if (backend_.load() == Backend::wave_out && device_)
                static_cast<void>(waveOutPause(device_));
            else if (backend_.load() == Backend::direct_sound &&
                     direct_sound_buffer_)
                static_cast<void>(direct_sound_buffer_->Stop());
            else if (backend_.load() == Backend::mci && mci_device_)
                static_cast<void>(mciSendCommandW(mci_device_, MCI_PAUSE, 0, 0));
            state_ = PlaybackState::paused;
        } else {
            state_ = PlaybackState::playing;
        }
        completion = completion_event_;
    }
    if (completion) SetEvent(completion);
}

void AudioEngine::PlaybackWorker(std::filesystem::path path, int subtrack) {
    int priority = THREAD_PRIORITY_HIGHEST;
    {
        std::scoped_lock lock(mutex_);
        priority = options_.thread_priority;
    }
    SetThreadPriority(GetCurrentThread(), std::clamp(
        priority, static_cast<int>(THREAD_PRIORITY_IDLE),
        static_cast<int>(THREAD_PRIORITY_TIME_CRITICAL)));
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (IsMidiPath(path)) {
        MciWorker(path);
    } else {
        const HRESULT media_result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(media_result)) {
            if (!stop_requested_)
                SetError(HResultMessage(L"MFStartup", media_result));
        }
        else {
            WaveOutWorker(path, subtrack);
            MFShutdown();
        }
    }
    if (SUCCEEDED(com_result)) CoUninitialize();
    {
        std::scoped_lock lock(mutex_);
        // EOF, cancellation and every failed output/decoder path must release
        // a target that can no longer be acknowledged by this worker.
        CancelSeekLocked();
    }
    SignalOpenComplete();
}

void AudioEngine::WaveOutWorker(const std::filesystem::path& path,
                                int subtrack) {
    // Declared before source/processors so its destructor runs after both:
    // the 004B1A5C writer may only be queued once the playback reader has
    // released its deny-write IStream.
    PlaybackReplayGainCommit replay_gain_commit;
    auto source = CreateDecodedAudioSource(
        path, subtrack, plugin_manager_, ttpcomm_module_);

    PlaybackOptions options;
    {
        std::scoped_lock lock(mutex_);
        options = options_;
    }
    WaveOutSelection output = ResolveWaveOutSelection(options);
    if (!output.error.empty()) {
        if (!stop_requested_) SetError(std::move(output.error));
        return;
    }
    if (!source->Open(path, options)) {
        if (!stop_requested_) SetError(source->Error());
        return;
    }
    if (stop_requested_) return;
    const WAVEFORMATEX& source_format = source->OutputFormat();
    if (source_format.nAvgBytesPerSec == 0 || source_format.nBlockAlign == 0) {
        if (!stop_requested_)
            SetError(L"Decoder returned an invalid output format");
        return;
    }
    const auto source_metadata = source->Metadata();
    const bool analyze_replay_gain = replay_gain_commit.Prepare(
        plugin_manager_, path, subtrack, options.auto_scan_gain);
    RecoveredProcessorChain processors(
        source_format, source_metadata, options, ttpcomm_module_, *dsp_chain_,
        analyze_replay_gain);
    for (auto& diagnostic : processors.TakeDiagnostics())
        RecordDiagnostic(std::move(diagnostic));
    uint64_t processor_revision = processor_revision_.load();

    // FUN_004B0D1A creates the floating-point processing stream first and
    // only then applies Device/@ResampleRate, @SsrcMode, @OutputBits and
    // @Dither.  Keeping this output transform outside every reader is what
    // makes APE/TAK/VQF, AIFF/AU, CDA and Media Foundation obey one contract.
    PcmOutputTransform output_transform;
    if (!output_transform.Open(
            source_format,
            {options.output_bits, options.resample_rate,
             options.ssrc_mode, options.dither},
            ttpcomm_module_)) {
        if (!stop_requested_) SetError(output_transform.Error());
        return;
    }
    WAVEFORMATEX wave_format = output_transform.OutputFormat();

    if (output.requested_backend == LegacyOutputBackend::direct_sound) {
        // FUN_004C3170 creates the DirectSound device selected by the
        // persisted GUID.  The synthetic DEF00000 key represents the null
        // DirectSound GUID, not a real COM class/device identifier.
        ComPtr<IDirectSound8> direct_sound;
        const GUID* identifier = output.default_direct_sound
            ? nullptr : &output.direct_sound_guid;
        HRESULT result = DirectSoundCreate8(identifier, &direct_sound, nullptr);
        if ((FAILED(result) || !direct_sound) && identifier) {
            // 004C31AA..004C31CC overwrites an unavailable selected GUID with
            // DSDEVID_DefaultPlayback and retries device construction.
            direct_sound.Reset();
            result = DirectSoundCreate8(nullptr, &direct_sound, nullptr);
            if (SUCCEEDED(result) && direct_sound) {
                RecordDiagnostic(
                    L"Configured DirectSound device is unavailable; using "
                    L"the default playback device");
            }
        }
        if (FAILED(result) || !direct_sound) {
            if (!stop_requested_)
                SetError(HResultMessage(L"DirectSoundCreate8", result));
            return;
        }
        const HWND cooperative_window = options.dsp_parent_window &&
                                         IsWindow(options.dsp_parent_window)
            ? options.dsp_parent_window : GetDesktopWindow();
        result = direct_sound->SetCooperativeLevel(
            cooperative_window,
            options.create_primary ? DSSCL_PRIORITY : DSSCL_NORMAL);
        if (FAILED(result)) {
            if (!stop_requested_)
                SetError(HResultMessage(L"IDirectSound::SetCooperativeLevel",
                                        result));
            return;
        }

        ComPtr<IDirectSoundBuffer> primary;
        if (options.create_primary) {
            DSBUFFERDESC primary_description{};
            primary_description.dwSize = sizeof(primary_description);
            primary_description.dwFlags = DSBCAPS_PRIMARYBUFFER;
            result = direct_sound->CreateSoundBuffer(
                &primary_description, &primary, nullptr);
            if (FAILED(result) || !primary) {
                if (!stop_requested_)
                    SetError(HResultMessage(
                        L"IDirectSound::CreateSoundBuffer(primary)", result));
                return;
            }
            result = primary->SetFormat(&wave_format);
            if (FAILED(result)) {
                if (!stop_requested_)
                    SetError(HResultMessage(
                        L"IDirectSoundBuffer::SetFormat(primary)", result));
                return;
            }
        }

        size_t ring_bytes = std::max<size_t>(
            static_cast<size_t>(options.file_buffer_bytes) * kOutputBufferCount,
            static_cast<size_t>(wave_format.nAvgBytesPerSec) *
                static_cast<size_t>(options.output_buffer_ms) / 1000);
        ring_bytes = std::clamp<size_t>(
            ring_bytes, static_cast<size_t>(wave_format.nBlockAlign) * 4,
            16U * 1024U * 1024U);
        size_t segment_bytes = ring_bytes / kOutputBufferCount;
        segment_bytes -= segment_bytes % wave_format.nBlockAlign;
        if (segment_bytes == 0) segment_bytes = wave_format.nBlockAlign;
        ring_bytes = segment_bytes * kOutputBufferCount;

        DSBUFFERDESC description{};
        description.dwSize = sizeof(description);
        description.dwFlags = DSBCAPS_GETCURRENTPOSITION2 |
                              DSBCAPS_GLOBALFOCUS |
                              DSBCAPS_CTRLVOLUME |
                              DSBCAPS_CTRLPAN |
                              (options.hardware_buffer
                                   ? DSBCAPS_LOCHARDWARE
                                   : DSBCAPS_LOCSOFTWARE);
        description.dwBufferBytes = static_cast<DWORD>(ring_bytes);
        description.lpwfxFormat = &wave_format;
        ComPtr<IDirectSoundBuffer> base_buffer;
        result = direct_sound->CreateSoundBuffer(
            &description, &base_buffer, nullptr);
        if ((FAILED(result) || !base_buffer) && options.hardware_buffer) {
            // FUN_004C3170 004C3240..004C325C retries FUN_004C3C86 with its
            // hardware flag cleared.  HardwareBuffer is a preference, not a
            // reason for the default configuration to become unplayable.
            base_buffer.Reset();
            description.dwFlags &= ~DSBCAPS_LOCHARDWARE;
            description.dwFlags |= DSBCAPS_LOCSOFTWARE;
            result = direct_sound->CreateSoundBuffer(
                &description, &base_buffer, nullptr);
            if (SUCCEEDED(result) && base_buffer) {
                RecordDiagnostic(
                    L"DirectSound hardware buffer is unavailable; using a "
                    L"software buffer");
            }
        }
        if (FAILED(result) || !base_buffer) {
            if (!stop_requested_)
                SetError(HResultMessage(
                    L"IDirectSound::CreateSoundBuffer(stream)", result));
            return;
        }
        ComPtr<IDirectSoundBuffer8> sound_buffer;
        // DirectSound's legacy headers expose IDirectSoundBuffer8 through
        // DECLARE_INTERFACE_ rather than __declspec(uuid), so WRL's As()
        // cannot use __uuidof here.  Query the documented IID explicitly.
        result = base_buffer->QueryInterface(
            kIidDirectSoundBuffer8,
            reinterpret_cast<void**>(sound_buffer.GetAddressOf()));
        if (FAILED(result) || !sound_buffer) {
            if (!stop_requested_)
                SetError(HResultMessage(
                    L"IDirectSoundBuffer::QueryInterface(IDirectSoundBuffer8)",
                    result));
            return;
        }

        const std::byte silence = wave_format.wBitsPerSample == 8
            ? std::byte{0x80} : std::byte{0};
        std::vector<std::byte> ring_shadow(ring_bytes, silence);
        auto write_ring = [&](size_t offset, const std::byte* bytes,
                              size_t count) -> bool {
            void* first{};
            void* second{};
            DWORD first_bytes{};
            DWORD second_bytes{};
            const HRESULT locked = sound_buffer->Lock(
                static_cast<DWORD>(offset), static_cast<DWORD>(count),
                &first, &first_bytes, &second, &second_bytes, 0);
            if (FAILED(locked)) {
                if (!stop_requested_)
                    SetError(HResultMessage(L"IDirectSoundBuffer::Lock", locked));
                return false;
            }
            if (first_bytes != 0) std::memcpy(first, bytes, first_bytes);
            if (second_bytes != 0)
                std::memcpy(second, bytes + first_bytes, second_bytes);
            sound_buffer->Unlock(first, first_bytes, second, second_bytes);
            std::memcpy(ring_shadow.data() + offset, bytes,
                        std::min(count, ring_bytes - offset));
            if (count > ring_bytes - offset)
                std::memcpy(ring_shadow.data(),
                            bytes + (ring_bytes - offset),
                            count - (ring_bytes - offset));
            return true;
        };

        std::vector<std::byte> decoded;
        size_t decoded_offset{};
        bool decoder_eof{};
        bool stream_eof{};
        uint64_t direct_processor_revision = processor_revision;
        auto decode_more = [&](size_t requested) -> bool {
            decoded.clear();
            decoded_offset = 0;
            std::vector<std::byte> native;
            const size_t native_request = std::max<size_t>(
                options.file_buffer_bytes,
                requested * static_cast<size_t>(source_format.nAvgBytesPerSec) /
                    std::max<DWORD>(1, wave_format.nAvgBytesPerSec) +
                    source_format.nBlockAlign);
            if (!source->Read(native_request, native, decoder_eof)) {
                if (!stop_requested_) SetError(source->Error());
                return false;
            }
            const uint64_t latest_revision = processor_revision_.load();
            if (latest_revision != direct_processor_revision) {
                PlaybackOptions current;
                {
                    std::scoped_lock lock(mutex_);
                    current = options_;
                }
                processors.Update(current);
                direct_processor_revision = latest_revision;
            }
            if (!native.empty() && !processors.Process(native)) {
                if (!stop_requested_)
                    SetError(L"The recovered audio processor chain failed");
                return false;
            }
            if (!output_transform.Process(native, decoded, decoder_eof)) {
                if (!stop_requested_) SetError(output_transform.Error());
                return false;
            }
            stream_eof = decoder_eof;
            return true;
        };

        std::optional<uint64_t> final_stream_byte;
        uint64_t stream_write_byte{};
        auto fill_segment = [&]() -> bool {
            std::vector<std::byte> segment(segment_bytes, silence);
            size_t written{};
            while (!stop_requested_ && written < segment.size()) {
                if (decoded_offset == decoded.size()) {
                    if (stream_eof) break;
                    if (!decode_more(segment.size() - written)) return false;
                    if (decoded.empty() && stream_eof) break;
                    if (decoded.empty()) continue;
                }
                const size_t copy = std::min(segment.size() - written,
                    decoded.size() - decoded_offset);
                std::memcpy(segment.data() + written,
                            decoded.data() + decoded_offset, copy);
                written += copy;
                decoded_offset += copy;
            }
            written -= written % wave_format.nBlockAlign;
            if (stream_eof && decoded_offset == decoded.size() &&
                !final_stream_byte) {
                final_stream_byte = stream_write_byte + written;
            }
            if (!write_ring(static_cast<size_t>(stream_write_byte % ring_bytes),
                            segment.data(), segment.size())) {
                return false;
            }
            stream_write_byte += segment_bytes;
            return true;
        };

        for (size_t index = 0;
             index < kOutputBufferCount && !final_stream_byte; ++index) {
            if (!fill_segment()) return;
        }
        if (final_stream_byte && *final_stream_byte == 0) {
            if (!stop_requested_)
                SetError(L"The audio stream contains no decodable samples");
            return;
        }

        {
            std::scoped_lock lock(mutex_);
            direct_sound_buffer_ = sound_buffer.Get();
            backend_ = Backend::direct_sound;
            ApplyVolumeLocked();
            metadata_ = source->Metadata();
        }
        sound_buffer->SetCurrentPosition(0);
        result = sound_buffer->Play(0, 0, DSBPLAY_LOOPING);
        if (FAILED(result)) {
            {
                std::scoped_lock lock(mutex_);
                direct_sound_buffer_ = nullptr;
                backend_ = Backend::none;
            }
            if (!stop_requested_)
                SetError(HResultMessage(L"IDirectSoundBuffer::Play", result));
            return;
        }
        bool usable = PublishOpened(
            Backend::direct_sound, source->DisplayFormat(), source->Duration());
        if (usable) replay_gain_commit.MarkPlaybackAccepted();
        uint64_t played_bytes{};
        DWORD last_cursor{};
        int64_t position_base_ms{};
        uint64_t last_visual_byte = std::numeric_limits<uint64_t>::max();

        bool natural_replay_gain_end{};
        while (usable && !stop_requested_) {
            source->SetPaused(state_.load() == PlaybackState::paused);
            const auto request = TakeSeekRequest();
            const int64_t requested = request.position_ms;
            if (requested >= 0) {
                const auto previous_state = state_.load();
                sound_buffer->Stop();
                sound_buffer->SetCurrentPosition(0);
                decoded.clear();
                decoded_offset = 0;
                decoder_eof = false;
                stream_eof = false;
                final_stream_byte.reset();
                stream_write_byte = 0;
                played_bytes = 0;
                last_cursor = 0;
                position_base_ms = std::clamp<int64_t>(
                    requested, 0, duration_ms_.load());
                processors.Reset();
                output_transform.Reset();
                if (requested >= duration_ms_.load()) {
                    CompleteSeek(request, position_base_ms);
                    state_ = PlaybackState::stopped;
                    break;
                }
                if (!source->Seek(std::chrono::milliseconds(position_base_ms))) {
                    if (!stop_requested_) SetError(source->Error());
                    break;
                }
                std::fill(ring_shadow.begin(), ring_shadow.end(), silence);
                if (!write_ring(0, ring_shadow.data(), ring_shadow.size())) break;
                for (size_t index = 0;
                     index < kOutputBufferCount && !final_stream_byte; ++index) {
                    if (!fill_segment()) { usable = false; break; }
                }
                if (!usable) break;
                if (previous_state == PlaybackState::playing) {
                    const HRESULT replay = sound_buffer->Play(
                        0, 0, DSBPLAY_LOOPING);
                    if (FAILED(replay)) {
                        if (!stop_requested_)
                            SetError(HResultMessage(
                                L"IDirectSoundBuffer::Play(seek)", replay));
                        break;
                    }
                }
                last_visual_byte = std::numeric_limits<uint64_t>::max();
                ClearVisualization();
                CompleteSeek(request, position_base_ms);
            }

            DWORD cursor{};
            DWORD write_cursor{};
            const HRESULT positioned = sound_buffer->GetCurrentPosition(
                &cursor, &write_cursor);
            if (FAILED(positioned)) {
                if (!stop_requested_)
                    SetError(HResultMessage(
                        L"IDirectSoundBuffer::GetCurrentPosition", positioned));
                break;
            }
            const DWORD delta = cursor >= last_cursor
                ? cursor - last_cursor
                : static_cast<DWORD>(ring_bytes - last_cursor + cursor);
            DWORD buffer_status{};
            if (SUCCEEDED(sound_buffer->GetStatus(&buffer_status)) &&
                (buffer_status & DSBSTATUS_PLAYING) != 0) {
                // Pause publishes its logical state before the 10 ms fade
                // worker stops the device (004AC7DD).  Account that audible
                // transition from the actual DirectSound clock, not State().
                played_bytes += delta;
            }
            last_cursor = cursor;
            while (!final_stream_byte &&
                   stream_write_byte - played_bytes <=
                       ring_bytes - segment_bytes) {
                if (!fill_segment()) { usable = false; break; }
            }
            if (!usable) break;
            position_ms_ = BoundPlaybackClock(
                position_base_ms +
                    static_cast<int64_t>(played_bytes * 1000 /
                                         wave_format.nAvgBytesPerSec),
                duration_ms_.load());
            UpdateTrackFade(position_ms_.load());

            if (state_.load() == PlaybackState::playing &&
                played_bytes != last_visual_byte) {
                VisualizationSamples samples;
                samples.sample_rate = wave_format.nSamplesPerSec;
                const size_t offset = cursor - cursor % wave_format.nBlockAlign;
                AppendVisualizationFrames(
                    ring_shadow.data() + offset, ring_bytes - offset,
                    wave_format, samples);
                if (samples.count < VisualizationSamples::capacity && offset != 0)
                    AppendVisualizationFrames(
                        ring_shadow.data(), offset, wave_format, samples);
                if (samples.count != 0)
                    PublishVisualization(std::move(samples));
                last_visual_byte = played_bytes;
            }
            if (final_stream_byte && played_bytes >= *final_stream_byte) {
                if (const auto duration = duration_ms_.load(); duration > 0)
                    position_ms_ = duration;
                state_ = PlaybackState::stopped;
                natural_replay_gain_end = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        sound_buffer->Stop();
        {
            std::scoped_lock lock(mutex_);
            if (direct_sound_buffer_ == sound_buffer.Get())
                direct_sound_buffer_ = nullptr;
            backend_ = Backend::none;
        }
        ClearVisualization();
        if (natural_replay_gain_end)
            replay_gain_commit.Complete(processors.FinishReplayGain());
        if (state_.load() != PlaybackState::failed)
            state_ = PlaybackState::stopped;
        return;
    }

    size_t buffer_bytes = std::max<size_t>(
        static_cast<size_t>(options.file_buffer_bytes),
        static_cast<size_t>(wave_format.nAvgBytesPerSec) *
            static_cast<size_t>(options.output_buffer_ms) /
            (1000 * kOutputBufferCount));
    buffer_bytes = std::clamp<size_t>(buffer_bytes, wave_format.nBlockAlign,
                                     4 * 1024 * 1024);
    buffer_bytes -= buffer_bytes % wave_format.nBlockAlign;
    if (buffer_bytes == 0) buffer_bytes = wave_format.nBlockAlign;

    std::unique_ptr<KernelStreamingSink> ks_sink;
    if (output.requested_backend == LegacyOutputBackend::kernel_streaming) {
        const auto catalog = ui::detail::EnumerateKernelStreamingOutputDevices();
        const auto selected = ui::detail::ResolveLegacyNativeOutputDevice(options.device_type, catalog);
        if (!selected.device) {
            if (!stop_requested_) SetError(L"The configured Kernel Streaming device is unavailable");
            return;
        }
        if (stop_requested_) return;
        ks_sink = std::make_unique<KernelStreamingSink>();
        if (!ks_sink->OpenDevice(selected.device->module, wave_format,
                                 kOutputBufferCount, buffer_bytes)) {
            if (!stop_requested_) SetError(ks_sink->Error());
            return;
        }
        if (ks_sink->DeviceFormat().wBitsPerSample != wave_format.wBitsPerSample)
            RecordDiagnostic(L"The selected KS pin uses the original 16-bit PCM fallback");
    }
    std::unique_ptr<AsioSink> asio_sink;
    if (output.requested_backend == LegacyOutputBackend::asio) {
        const auto catalog = ui::detail::EnumerateAsioOutputDevices();
        const auto selected = ui::detail::ResolveLegacyNativeOutputDevice(
            options.device_type, catalog);
        if (!selected.device || !selected.device->has_class_id) {
            if (!stop_requested_) SetError(L"The configured ASIO device is unavailable");
            return;
        }
        if (stop_requested_) return;
        asio_sink = std::make_unique<AsioSink>();
        if (!asio_sink->OpenDevice(selected.device->name, selected.device->module,
                                   selected.device->class_id, wave_format,
                                   static_cast<std::uint32_t>(buffer_bytes))) {
            if (!stop_requested_) SetError(asio_sink->Error());
            return;
        }
        // IASIO owns a fixed period; the worker keeps exactly four source
        // packets in the recovered bounded queue rather than passing these
        // buffers to waveOut.
        buffer_bytes = static_cast<size_t>(asio_sink->PeriodFrames()) *
                       wave_format.nBlockAlign;
    }

    HANDLE completion = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completion) {
        if (!stop_requested_)
            SetError(L"Unable to create waveOut completion event");
        return;
    }
    HWAVEOUT opened_device{};
    UINT output_device = output.device_id;
    if (!ks_sink && !asio_sink && output_device != WAVE_MAPPER) {
        WAVEOUTCAPSW capabilities{};
        const MMRESULT capabilities_result = waveOutGetDevCapsW(
            output_device, &capabilities, sizeof(capabilities));
        if (capabilities_result != MMSYSERR_NOERROR) {
            CloseHandle(completion);
            if (!stop_requested_)
                SetError(WaveOutErrorMessage(
                    L"Configured waveOut device " +
                        std::to_wstring(output_device) + L" is unavailable",
                    capabilities_result));
            return;
        }
    }
    MMRESULT open_result = (ks_sink || asio_sink) ? MMSYSERR_NOERROR : waveOutOpen(
        &opened_device, output_device, &wave_format,
        reinterpret_cast<DWORD_PTR>(&WaveOutCallback),
        reinterpret_cast<DWORD_PTR>(completion), CALLBACK_FUNCTION);
    if (open_result != MMSYSERR_NOERROR) {
        CloseHandle(completion);
        if (!stop_requested_)
            SetError(WaveOutErrorMessage(L"waveOutOpen", open_result));
        return;
    }
    {
        std::scoped_lock lock(mutex_);
        device_ = opened_device;
        completion_event_ = completion;
        backend_ = asio_sink ? Backend::asio :
                   ks_sink ? Backend::kernel_streaming : Backend::wave_out;
        ApplyVolumeLocked();
    }

    // FUN_004E27AD allocates one WAVEHDR per decoded block and returns it to a
    // free list from the WOM_DONE callback. Four stable buffers reproduce
    // that bounded streaming behaviour without retaining the entire track.
    std::array<OutputBuffer, kOutputBufferCount> buffers;
    for (auto& buffer : buffers) buffer.data.resize(buffer_bytes);
    std::vector<std::byte> decoded;
    size_t decoded_offset{};
    bool decoder_eof{};
    size_t queued{};
    int64_t position_base_ms{};
    uint64_t next_stream_byte{};
    uint64_t last_visual_byte = std::numeric_limits<uint64_t>::max();

    auto fill = [&](OutputBuffer& output) -> bool {
        size_t written{};
        while (!stop_requested_ && written < output.data.size() &&
               (decoded_offset < decoded.size() || !decoder_eof)) {
            if (decoded_offset == decoded.size()) {
                decoded.clear();
                decoded_offset = 0;
                if (!source->Read(output.data.size() - written, decoded, decoder_eof)) {
                    if (!stop_requested_) SetError(source->Error());
                    return false;
                }
                if (stop_requested_) return false;
                const uint64_t latest_revision = processor_revision_.load();
                if (latest_revision != processor_revision) {
                    PlaybackOptions current;
                    {
                        std::scoped_lock lock(mutex_);
                        current = options_;
                    }
                    processors.Update(current);
                    for (auto& diagnostic : processors.TakeDiagnostics())
                        RecordDiagnostic(std::move(diagnostic));
                    processor_revision = latest_revision;
                }
                if (!decoded.empty() && !processors.Process(decoded)) {
                    if (!stop_requested_)
                        SetError(L"The recovered audio processor chain failed");
                    return false;
                }
                if (!decoded.empty() || decoder_eof) {
                    std::vector<std::byte> converted;
                    if (!output_transform.Process(decoded, converted,
                                                  decoder_eof)) {
                        if (!stop_requested_)
                            SetError(output_transform.Error());
                        return false;
                    }
                    decoded = std::move(converted);
                }
                for (auto& diagnostic : processors.TakeDiagnostics())
                    RecordDiagnostic(std::move(diagnostic));
                if (decoded.empty()) continue;
            }
            const size_t copy = std::min(output.data.size() - written,
                                         decoded.size() - decoded_offset);
            std::memcpy(output.data.data() + written,
                        decoded.data() + decoded_offset, copy);
            written += copy;
            decoded_offset += copy;
        }
        written -= written % wave_format.nBlockAlign;
        if (written == 0) return true;
        output.stream_byte_offset = next_stream_byte;
        next_stream_byte += written;
        output.header = {};
        output.header.lpData = reinterpret_cast<LPSTR>(output.data.data());
        output.header.dwBufferLength = static_cast<DWORD>(written);
        if (ks_sink || asio_sink) {
            float left{1.0F}, right{1.0F};
            if (ks_sink) {
                std::scoped_lock lock(mutex_);
                // KS has no universally exposed per-pin volume node.  Unlike
                // waveOut/DirectSound, its output gain is applied when a
                // packet is copied.  Do not bake the opening transition
                // (initially zero) into the four packets queued before
                // PublishOpened: that would turn the first several hundred
                // milliseconds into silence and make a 10 ms fade start late.
                // Explicit play/pause/seek fades are intentionally gated away
                // from KS until the original topology-volume-node ABI is
                // recovered; user volume/balance and end-of-track gain take
                // effect at the next real KS packet boundary.
                left = right = std::clamp(volume_ * track_gain_, 0.0F, 1.0F);
                if (balance_ > 0) left *= (100 - balance_) / 100.0F;
                else if (balance_ < 0) right *= (100 + balance_) / 100.0F;
            } else {
                // The original ASIO output vtable has the generic no-op stub
                // at both SetVolume/SetBalance slots (+0x20/+0x24).  It is a
                // bit-perfect path: do not invent software volume, balance or
                // transition fades by premultiplying submitted samples.
                left = right = 1.0F;
            }
            const bool submitted = ks_sink
                ? ks_sink->Submit(static_cast<size_t>(&output - buffers.data()),
                    std::span<const std::byte>(output.data.data(), written), left, right)
                : asio_sink->Submit(std::span<const std::byte>(output.data.data(), written),
                                    left, right);
            if (!submitted) {
                if (!stop_requested_) SetError(ks_sink ? ks_sink->Error() : asio_sink->Error());
                return false;
            }
            output.header.dwFlags = WHDR_PREPARED;
        } else if (waveOutPrepareHeader(opened_device, &output.header,
                                 sizeof(output.header)) != MMSYSERR_NOERROR ||
            waveOutWrite(opened_device, &output.header,
                         sizeof(output.header)) != MMSYSERR_NOERROR) {
            if (!stop_requested_) SetError(L"waveOutWrite failed");
            return false;
        }
        output.queued = true;
        ++queued;
        return true;
    };

    // The four waveOut buffers cover noticeably more time than one original
    // visual frame.  Locate the real play cursor inside that queue and stitch
    // up to 512 frames across adjacent buffers.  This preserves the original
    // two-channel sample contract without exposing mutable WAVEHDR storage to
    // the UI thread.
    auto publish_at = [&](uint64_t played_bytes, bool force) {
        played_bytes -= played_bytes % wave_format.nBlockAlign;
        if (!force && played_bytes == last_visual_byte) return;

        std::array<const OutputBuffer*, kOutputBufferCount> ordered{};
        size_t ordered_count{};
        for (const auto& buffer : buffers) {
            if (buffer.queued && buffer.header.dwBufferLength != 0)
                ordered[ordered_count++] = &buffer;
        }
        std::sort(ordered.begin(), ordered.begin() + ordered_count,
                  [](const OutputBuffer* left, const OutputBuffer* right) {
                      return left->stream_byte_offset < right->stream_byte_offset;
                  });

        VisualizationSamples samples;
        samples.sample_rate = wave_format.nSamplesPerSec;
        uint64_t cursor = played_bytes;
        for (size_t index = 0;
             index < ordered_count &&
             samples.count < VisualizationSamples::capacity;
             ++index) {
            const OutputBuffer& buffer = *ordered[index];
            const uint64_t begin = buffer.stream_byte_offset;
            const uint64_t end = begin + buffer.header.dwBufferLength;
            if (end <= cursor) continue;
            if (cursor < begin) {
                if (samples.count != 0) break;
                cursor = begin;
            }
            const size_t offset = static_cast<size_t>(cursor - begin);
            AppendVisualizationFrames(
                buffer.data.data() + offset,
                static_cast<size_t>(buffer.header.dwBufferLength) - offset,
                wave_format, samples);
            cursor = end;
        }
        if (samples.count != 0) PublishVisualization(std::move(samples));
        last_visual_byte = played_bytes;
    };

    bool usable = true;
    for (auto& buffer : buffers) {
        if (!fill(buffer)) { usable = false; break; }
        if (decoder_eof && decoded_offset == decoded.size()) break;
    }
    if (usable && queued != 0) {
        {
            std::scoped_lock lock(mutex_);
            metadata_ = source->Metadata();
        }
        if ((ks_sink && !ks_sink->SetPaused(false)) ||
            (asio_sink && !asio_sink->SetPaused(false))) {
            SetError(ks_sink ? ks_sink->Error() : asio_sink->Error());
            usable = false;
        }
        if (usable) usable = PublishOpened(
            asio_sink ? Backend::asio :
            ks_sink ? Backend::kernel_streaming : Backend::wave_out,
            source->DisplayFormat(), source->Duration());
        if (usable) replay_gain_commit.MarkPlaybackAccepted();
        if (usable) publish_at(0, true);
    } else if (usable) {
        if (!stop_requested_)
            SetError(L"The audio stream contains no decodable samples");
        usable = false;
    }

    bool natural_replay_gain_end{};
    while (usable && !stop_requested_) {
        source->SetPaused(state_.load() == PlaybackState::paused);
        const auto request = TakeSeekRequest();
        const int64_t requested = request.position_ms;
        if (requested >= 0) {
            const auto previous_state = state_.load();
            if (ks_sink) {
                if (!ks_sink->Reset()) { SetError(ks_sink->Error()); break; }
            } else if (asio_sink) {
                if (!asio_sink->Reset()) { SetError(asio_sink->Error()); break; }
            } else {
                waveOutReset(opened_device);
                // Reset clears waveOut's pause state. A push decoder can
                // take several callbacks to prefill; pause BEFORE the first
                // waveOutWrite or a paused seek audibly advances 10-20 ms.
                if (previous_state == PlaybackState::paused)
                    waveOutPause(opened_device);
            }
            for (auto& buffer : buffers) {
                if (!ks_sink && !asio_sink &&
                    (buffer.header.dwFlags & WHDR_PREPARED))
                    waveOutUnprepareHeader(opened_device, &buffer.header,
                                           sizeof(buffer.header));
                buffer.header = {};
                buffer.queued = false;
            }
            queued = 0;
            next_stream_byte = 0;
            last_visual_byte = std::numeric_limits<uint64_t>::max();
            ClearVisualization();
            decoded.clear();
            decoded_offset = 0;
            decoder_eof = false;
            position_base_ms = std::clamp<int64_t>(requested, 0, duration_ms_.load());
            if (requested >= duration_ms_.load()) {
                CompleteSeek(request, position_base_ms);
                state_ = PlaybackState::stopped;
                break;
            }
            if (!source->Seek(std::chrono::milliseconds(position_base_ms))) {
                if (!stop_requested_) SetError(source->Error());
                break;
            }
            processors.Reset();
            output_transform.Reset();
            for (auto& buffer : buffers) {
                if (!fill(buffer)) { usable = false; break; }
                if (decoder_eof && decoded_offset == decoded.size()) break;
            }
            if (ks_sink) {
                if (!ks_sink->SetPaused(previous_state == PlaybackState::paused)) {
                    SetError(ks_sink->Error()); break;
                }
            } else if (asio_sink) {
                if (!asio_sink->SetPaused(previous_state == PlaybackState::paused)) {
                    SetError(asio_sink->Error()); break;
                }
            } else if (previous_state == PlaybackState::paused)
                waveOutPause(opened_device);
            else if (usable && queued != 0)
                publish_at(0, true);
            if (usable) CompleteSeek(request, position_base_ms);
        }

        if ((ks_sink && !ks_sink->SetPaused(state_.load() == PlaybackState::paused)) ||
            (asio_sink && !asio_sink->SetPaused(state_.load() == PlaybackState::paused))) {
            SetError(ks_sink ? ks_sink->Error() : asio_sink->Error()); break;
        }
        for (auto& buffer : buffers) {
            if (ks_sink && buffer.queued && ks_sink->IsComplete(
                    static_cast<size_t>(&buffer - buffers.data())))
                buffer.header.dwFlags |= WHDR_DONE;
            if (asio_sink && buffer.queued && asio_sink->ConsumedSourceBytes() >=
                    buffer.stream_byte_offset + buffer.header.dwBufferLength)
                buffer.header.dwFlags |= WHDR_DONE;
            if (ks_sink && !ks_sink->Error().empty()) {
                SetError(ks_sink->Error()); usable = false; break;
            }
            if (!buffer.queued || (buffer.header.dwFlags & WHDR_DONE) == 0) continue;
            if (!ks_sink && !asio_sink)
                waveOutUnprepareHeader(opened_device, &buffer.header, sizeof(buffer.header));
            buffer.header = {};
            buffer.queued = false;
            --queued;
            if ((!decoder_eof || decoded_offset < decoded.size()) &&
                !fill(buffer)) { usable = false; break; }
        }
        if (!usable || state_.load() == PlaybackState::failed) break;
        if (stop_requested_) break;

        MMTIME position{};
        position.wType = TIME_BYTES;
        const std::uint64_t native_position = ks_sink ? ks_sink->PositionSourceBytes() :
            asio_sink ? asio_sink->ConsumedSourceBytes() : 0;
        if (ks_sink || asio_sink || waveOutGetPosition(opened_device, &position, sizeof(position)) == MMSYSERR_NOERROR) {
            int64_t relative{};
            uint64_t played_bytes{};
            if (ks_sink || asio_sink) {
                played_bytes = native_position;
                relative = static_cast<int64_t>(native_position * 1000 / wave_format.nAvgBytesPerSec);
            } else if (position.wType == TIME_BYTES) {
                relative = static_cast<int64_t>(position.u.cb) * 1000 /
                           wave_format.nAvgBytesPerSec;
                played_bytes = position.u.cb;
            } else if (position.wType == TIME_SAMPLES) {
                relative = static_cast<int64_t>(position.u.sample) * 1000 /
                           wave_format.nSamplesPerSec;
                played_bytes = static_cast<uint64_t>(position.u.sample) *
                               wave_format.nBlockAlign;
            } else if (position.wType == TIME_MS) {
                relative = position.u.ms;
                played_bytes = static_cast<uint64_t>(position.u.ms) *
                               wave_format.nAvgBytesPerSec / 1000;
            }
            position_ms_ = BoundPlaybackClock(position_base_ms + relative, duration_ms_.load());
            UpdateTrackFade(position_ms_.load());
            if (state_.load() == PlaybackState::playing)
                publish_at(played_bytes, false);
        }
        if (decoder_eof && decoded_offset == decoded.size() && queued == 0) {
            if (const auto duration = duration_ms_.load(); duration > 0)
                position_ms_ = duration;
            state_ = PlaybackState::stopped;
            natural_replay_gain_end = true;
            break;
        }
        if (asio_sink) {
            const HANDLE events[]{completion, asio_sink->ActivityEvent()};
            WaitForMultipleObjects(2, events, FALSE, 20);
        } else {
            WaitForSingleObject(completion, 20);
        }
    }

    if (asio_sink) asio_sink->Close();
    else if (ks_sink) ks_sink->Close();
    else waveOutReset(opened_device);
    for (auto& buffer : buffers) {
        if (!ks_sink && !asio_sink && (buffer.header.dwFlags & WHDR_PREPARED))
            waveOutUnprepareHeader(opened_device, &buffer.header, sizeof(buffer.header));
    }
    if (!ks_sink && !asio_sink) waveOutClose(opened_device);
    CloseHandle(completion);
    {
        std::scoped_lock lock(mutex_);
        if (device_ == opened_device) device_ = nullptr;
        if (completion_event_ == completion) completion_event_ = nullptr;
        backend_ = Backend::none;
    }
    ClearVisualization();
    if (natural_replay_gain_end)
        replay_gain_commit.Complete(processors.FinishReplayGain());
    if (state_.load() != PlaybackState::failed) state_ = PlaybackState::stopped;
}

void AudioEngine::MciWorker(const std::filesystem::path& path) {
    MCI_OPEN_PARMSW open{};
    open.lpstrElementName = path.c_str();
    MCIERROR result = mciSendCommandW(0, MCI_OPEN, MCI_OPEN_ELEMENT | MCI_WAIT,
                                      reinterpret_cast<DWORD_PTR>(&open));
    if (result != 0) {
        if (!stop_requested_) SetError(MciErrorMessage(L"MCI_OPEN", result));
        return;
    }
    const MCIDEVICEID device = open.wDeviceID;
    if (stop_requested_) {
        mciSendCommandW(device, MCI_CLOSE, MCI_WAIT, 0);
        return;
    }
    MCI_SET_PARMS set{};
    set.dwTimeFormat = MCI_FORMAT_MILLISECONDS;
    result = mciSendCommandW(device, MCI_SET, MCI_SET_TIME_FORMAT | MCI_WAIT,
                             reinterpret_cast<DWORD_PTR>(&set));
    if (result != 0) {
        mciSendCommandW(device, MCI_CLOSE, MCI_WAIT, 0);
        if (!stop_requested_)
            SetError(MciErrorMessage(L"MCI_SET_TIME_FORMAT", result));
        return;
    }
    DWORD duration{};
    MciStatus(device, MCI_STATUS_LENGTH, duration);
    {
        std::scoped_lock lock(mutex_);
        mci_device_ = device;
        backend_ = Backend::mci;
        ApplyVolumeLocked();
    }
    MCI_PLAY_PARMS play{};
    result = mciSendCommandW(device, MCI_PLAY, 0,
                             reinterpret_cast<DWORD_PTR>(&play));
    if (result != 0) {
        mciSendCommandW(device, MCI_CLOSE, MCI_WAIT, 0);
        {
            std::scoped_lock lock(mutex_);
            mci_device_ = 0;
            backend_ = Backend::none;
        }
        if (!stop_requested_) SetError(MciErrorMessage(L"MCI_PLAY", result));
        return;
    }
    AudioFormat format{};
    format.format_tag = 0xffff;
    format.codec_name = L"MIDI";
    if (!PublishOpened(Backend::mci, format,
                       std::chrono::milliseconds(duration))) {
        mciSendCommandW(device, MCI_STOP, MCI_WAIT, 0);
        mciSendCommandW(device, MCI_CLOSE, MCI_WAIT, 0);
        {
            std::scoped_lock lock(mutex_);
            if (mci_device_ == device) mci_device_ = 0;
            backend_ = Backend::none;
        }
        return;
    }

    while (!stop_requested_) {
        const auto request = TakeSeekRequest();
        const int64_t requested = request.position_ms;
        if (requested >= 0) {
            const auto previous_state = state_.load();
            mciSendCommandW(device, MCI_STOP, MCI_WAIT, 0);
            MCI_PLAY_PARMS from{};
            from.dwFrom = static_cast<DWORD>(std::clamp<int64_t>(
                requested, 0, duration_ms_.load()));
            if (from.dwFrom >= static_cast<DWORD>(duration_ms_.load())) {
                CompleteSeek(request, duration_ms_.load());
                state_ = PlaybackState::stopped;
                break;
            }
            result = mciSendCommandW(device, MCI_PLAY, MCI_FROM,
                                     reinterpret_cast<DWORD_PTR>(&from));
            if (result != 0) {
                if (!stop_requested_)
                    SetError(MciErrorMessage(L"MCI_PLAY(MCI_FROM)", result));
                break;
            }
            if (previous_state == PlaybackState::paused)
                mciSendCommandW(device, MCI_PAUSE, 0, 0);
            CompleteSeek(request, from.dwFrom);
        }
        DWORD position{};
        if (MciStatus(device, MCI_STATUS_POSITION, position)) {
            position_ms_ = position;
            UpdateTrackFade(position_ms_.load());
        }
        DWORD mode{};
        if (MciStatus(device, MCI_STATUS_MODE, mode) && mode == MCI_MODE_STOP) {
            if (position_ms_.load() + 20 >= duration_ms_.load())
                position_ms_ = duration_ms_.load();
            state_ = PlaybackState::stopped;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    mciSendCommandW(device, MCI_STOP, MCI_WAIT, 0);
    mciSendCommandW(device, MCI_CLOSE, MCI_WAIT, 0);
    {
        std::scoped_lock lock(mutex_);
        if (mci_device_ == device) mci_device_ = 0;
        backend_ = Backend::none;
    }
    if (state_.load() != PlaybackState::failed) state_ = PlaybackState::stopped;
}

bool AudioEngine::PublishOpened(Backend backend, const AudioFormat& format,
                                std::chrono::milliseconds duration) {
    bool published{};
    {
        std::scoped_lock lock(mutex_);
        if (!stop_requested_) {
            backend_ = backend;
            format_ = format;
            duration_ms_ = std::max<int64_t>(0, duration.count());
            error_.clear();
            state_ = PlaybackState::playing;
            track_gain_ = 1.0F;
            if ((options_.sound_fade_mode & 0x01) != 0 &&
                options_.fade_duration[
                    RecoveredFadeInDurationIndex(false)] > 0) {
                // local_38 == 5 in CSound::ReadThreadProc: ordinary open
                // uses FadeDuration[0], stepping the output every 10 ms.
                QueueFadeLocked(
                    1.0F,
                    std::chrono::milliseconds(options_.fade_duration[
                        RecoveredFadeInDurationIndex(false)]),
                    FadeCompletion::none);
            } else {
                transition_gain_ = 1.0F;
                ApplyVolumeLocked();
            }
            published = true;
        }
        open_complete_ = true;
    }
    open_condition_.notify_all();
    return published;
}

void AudioEngine::SignalOpenComplete() {
    {
        std::scoped_lock lock(mutex_);
        open_complete_ = true;
    }
    open_condition_.notify_all();
}

void AudioEngine::Pause() {
    std::scoped_lock lock(mutex_);
    if (state_.load() != PlaybackState::playing) return;
    if ((options_.sound_fade_mode & 0x02) != 0 &&
        options_.fade_duration[1] > 0 &&
        (backend_.load() == Backend::wave_out ||
         backend_.load() == Backend::direct_sound)) {
        // local_38 == 6 / FadeDuration[1] at 004AC7F0..004AC845.
        state_ = PlaybackState::paused;
        QueueFadeLocked(
            0.0F, std::chrono::milliseconds(options_.fade_duration[1]),
            FadeCompletion::pause);
        return;
    }
    bool success{};
    if (backend_.load() == Backend::wave_out && device_)
        success = waveOutPause(device_) == MMSYSERR_NOERROR;
    else if (backend_.load() == Backend::direct_sound && direct_sound_buffer_)
        success = SUCCEEDED(direct_sound_buffer_->Stop());
    else if (backend_.load() == Backend::kernel_streaming ||
             backend_.load() == Backend::asio)
        success = true; // The owning worker performs the native pin transition.
    else if (backend_.load() == Backend::mci && mci_device_)
        success = mciSendCommandW(mci_device_, MCI_PAUSE, 0, 0) == 0;
    if (success) state_ = PlaybackState::paused;
}

void AudioEngine::Resume() {
    std::scoped_lock lock(mutex_);
    if (state_.load() != PlaybackState::paused) return;
    bool success{};
    if (backend_.load() == Backend::wave_out && device_)
        success = waveOutRestart(device_) == MMSYSERR_NOERROR;
    else if (backend_.load() == Backend::direct_sound && direct_sound_buffer_)
        success = SUCCEEDED(direct_sound_buffer_->Play(0, 0, DSBPLAY_LOOPING));
    else if (backend_.load() == Backend::kernel_streaming ||
             backend_.load() == Backend::asio)
        success = true;
    else if (backend_.load() == Backend::mci && mci_device_) {
        success = mciSendCommandW(mci_device_, MCI_RESUME, 0, 0) == 0;
        if (!success) {
            DWORD position{};
            MciStatus(mci_device_, MCI_STATUS_POSITION, position);
            MCI_PLAY_PARMS play{};
            play.dwFrom = position;
            success = mciSendCommandW(mci_device_, MCI_PLAY, MCI_FROM,
                                      reinterpret_cast<DWORD_PTR>(&play)) == 0;
        }
    }
    if (success) {
        state_ = PlaybackState::playing;
        if ((options_.sound_fade_mode & 0x01) != 0 &&
            options_.fade_duration[
                RecoveredFadeInDurationIndex(false)] > 0 &&
            (backend_.load() == Backend::wave_out ||
             backend_.load() == Backend::direct_sound)) {
            transition_gain_ = 0.0F;
            // 004AC605 state=1 with the seek-transition flag clear reads
            // CSound+0xB4, FadeDuration[0].  FadeDuration[2] belongs only to
            // the seek flag branch below.
            QueueFadeLocked(
                1.0F, std::chrono::milliseconds(options_.fade_duration[
                    RecoveredFadeInDurationIndex(false)]),
                FadeCompletion::none);
        } else {
            transition_gain_ = 1.0F;
            ApplyVolumeLocked();
        }
    }
}

void AudioEngine::Seek(std::chrono::milliseconds position) {
    SeekImpl(position, true);
}

PlaybackClockSnapshot AudioEngine::ClockSnapshot() const {
    std::scoped_lock lock(mutex_);
    const auto pending = pending_seek_position_ms_.load();
    return {state_.load(),
            std::chrono::milliseconds(pending >= 0 ? pending : position_ms_.load()),
            std::chrono::milliseconds(duration_ms_.load()), seek_revision_, pending >= 0,
            std::chrono::steady_clock::now()};
}

void AudioEngine::SeekWithoutFade(std::chrono::milliseconds position) {
    SeekImpl(position, false);
}

void AudioEngine::QueueSeekLocked(int64_t position_ms) {
    const auto state = state_.load();
    if (state != PlaybackState::playing && state != PlaybackState::paused) return;
    ++seek_revision_;
    pending_seek_position_ms_ = position_ms;
    seek_request_ms_ = position_ms;
}

AudioEngine::SeekRequest AudioEngine::TakeSeekRequest() {
    if (seek_request_ms_.load() < 0) return {};
    std::scoped_lock lock(mutex_);
    return {seek_request_ms_.exchange(-1), seek_revision_};
}

void AudioEngine::CompleteSeek(const SeekRequest& request, int64_t position_ms) {
    std::scoped_lock lock(mutex_);
    if (request.position_ms < 0 || request.revision != seek_revision_ ||
        pending_seek_position_ms_.load() < 0) return;
    // Publish the new output clock before removing the display override.
    // Repeated seeks to the same millisecond still have distinct revisions.
    position_ms_ = position_ms;
    pending_seek_position_ms_ = -1;
}

void AudioEngine::CancelSeekLocked() {
    ++seek_revision_;
    seek_request_ms_ = -1;
    pending_seek_position_ms_ = -1;
}

void AudioEngine::SeekImpl(std::chrono::milliseconds position,
                           bool allow_fade) {
    const auto state = state_.load();
    if (state != PlaybackState::playing && state != PlaybackState::paused) return;
    ClearVisualization();
    const int64_t target = std::clamp<int64_t>(
        position.count(), 0, duration_ms_.load());
    HANDLE completion{};
    {
        std::scoped_lock lock(mutex_);
        if (state_.load() != PlaybackState::playing &&
            state_.load() != PlaybackState::paused) return;
        const bool supported = backend_.load() == Backend::wave_out ||
                               backend_.load() == Backend::direct_sound;
        if (ShouldBeginRecoveredSeekFade(
                allow_fade, options_.sound_fade_mode,
                options_.fade_duration[RecoveredFadeInDurationIndex(true)],
                state, supported)) {
            // The seek-transition flag at CSound +0xB0 makes the subsequent
            // local_38 == 5 branch choose FadeDuration[2].
            QueueFadeLocked(
                0.0F, std::chrono::milliseconds(options_.fade_duration[
                    RecoveredFadeInDurationIndex(true)]),
                FadeCompletion::seek, target);
            return;
        }
        // A progress/lyric release may interrupt either half of an earlier
        // ordinary seek fade. Cancel only that seek transition and restore
        // unity gain, otherwise the stale target would be committed later and
        // the supposedly direct drag would remain partly attenuated.
        if (!allow_fade && fade_pending_ &&
            (fade_completion_ == FadeCompletion::seek ||
             fade_completion_ == FadeCompletion::seek_resume)) {
            fade_pending_ = false;
            fade_completion_ = FadeCompletion::none;
            fade_seek_target_ms_ = -1;
            ++fade_generation_;
            transition_gain_ = 1.0F;
            fade_condition_.notify_all();
        }
        QueueSeekLocked(target);
        track_gain_ = 1.0F;
        ApplyVolumeLocked();
        completion = completion_event_;
    }
    if (completion) SetEvent(completion);
}

void AudioEngine::RequestStop() noexcept {
    stop_fade_pending_.store(false, std::memory_order_release);
    stop_requested_ = true;
    state_ = PlaybackState::stopped;
    ClearVisualization();
    if (worker_.joinable()) {
        worker_.request_stop();
        // This cancels cancellable synchronous file/device I/O issued by the
        // worker.  Legacy decoder callbacks which do not enter such I/O still
        // remain safely owned and are handled by the bounded reap below.
        static_cast<void>(CancelSynchronousIo(worker_.native_handle()));
    }
    HANDLE completion{};
    {
        std::scoped_lock lock(mutex_);
        CancelSeekLocked();
        fade_pending_ = false;
        ++fade_generation_;
        transition_gain_ = 1.0F;
        track_gain_ = 1.0F;
        completion = completion_event_;
    }
    // waveOutReset and MCI_STOP can themselves block in a driver.  Wake the
    // worker and let it perform those teardown calls off the UI thread.
    if (completion) SetEvent(completion);
}

bool AudioEngine::ReapWorker(std::chrono::milliseconds timeout) noexcept {
    if (!worker_.joinable()) return true;
    const auto count = std::clamp<int64_t>(timeout.count(), 0, MAXDWORD - 1);
    const DWORD result = WaitForSingleObject(
        worker_.native_handle(), static_cast<DWORD>(count));
    if (result != WAIT_OBJECT_0) return false;
    try {
        worker_.join();
        return true;
    } catch (const std::system_error&) {
        // Keep ownership if the CRT could not reap an otherwise-signalled
        // thread.  A later Stop/destructor remains the lifetime barrier.
        return false;
    }
}

void AudioEngine::Stop() {
    RequestStop();
    // Wake a caller which is synchronously awaiting decoder creation before
    // spending the bounded teardown budget.
    SignalOpenComplete();
    if (!ReapWorker(kStopWaitBudget)) {
        // The worker remains joinable and owned.  Play refuses to replace it,
        // while subsequent Stop calls can reap it after the decoder returns.
    }
    position_ms_ = 0;
    backend_ = Backend::none;
}

void AudioEngine::StopWithFade() {
    if (BeginStopFade()) return;
    Stop();
}

bool AudioEngine::BeginStopFade() {
    std::unique_lock lock(mutex_);
    const bool supported = backend_.load() == Backend::wave_out ||
                           backend_.load() == Backend::direct_sound;
    if (ShouldBeginRecoveredStopFade(
            options_.sound_fade_mode, options_.fade_duration[3],
            state_.load(), supported)) {
        // local_38 == 7 at 004AC8BD and CSoundFadeOut::Run both retain the
        // output object until its 10 ms volume ramp reaches zero.
        state_ = PlaybackState::stopped;
        stop_fade_pending_.store(true, std::memory_order_release);
        QueueFadeLocked(
            0.0F, std::chrono::milliseconds(options_.fade_duration[3]),
            FadeCompletion::stop);
        return true;
    }
    return false;
}

void AudioEngine::SetVolume(float volume) {
    std::scoped_lock lock(mutex_);
    volume_ = std::clamp(volume, 0.0F, 1.0F);
    ApplyVolumeLocked();
}

void AudioEngine::SetBalance(int balance) {
    std::scoped_lock lock(mutex_);
    balance_ = std::clamp(balance, -100, 100);
    options_.balance = balance_;
    ApplyVolumeLocked();
}

void AudioEngine::SetEqualizer(int profile, int surround,
                               const std::array<int, 11>& values) {
    std::scoped_lock lock(mutex_);
    options_.equalizer_profile = profile;
    options_.surround = std::clamp(surround, 0, 16);
    options_.equalizer_values = values;
    for (auto& value : options_.equalizer_values)
        value = std::clamp(value, -12, 12);
    // CPlayerWnd sends private message 0x7EE after every equalizer mutation.
    // The worker observes this generation before decoding the next block and
    // reconfigures ordinal 103/104 on its owning thread.
    ++processor_revision_;
    if (completion_event_) SetEvent(completion_event_);
}

void AudioEngine::QueueFadeLocked(float target,
                                  std::chrono::milliseconds duration,
                                  FadeCompletion completion,
                                  int64_t seek_target_ms) {
    fade_from_ = std::clamp(transition_gain_, 0.0F, 1.0F);
    fade_target_ = std::clamp(target, 0.0F, 1.0F);
    fade_started_ = std::chrono::steady_clock::now();
    fade_duration_ = std::max(duration, std::chrono::milliseconds(1));
    fade_completion_ = completion;
    fade_seek_target_ms_ = seek_target_ms;
    fade_pending_ = true;
    ++fade_generation_;
    ApplyVolumeLocked();
    fade_condition_.notify_all();
}

void AudioEngine::FadeWorker(std::stop_token stop_token) {
    std::unique_lock lock(mutex_);
    while (!stop_token.stop_requested() && !fade_shutdown_) {
        fade_condition_.wait(lock, [this, &stop_token] {
            return stop_token.stop_requested() || fade_shutdown_ ||
                   fade_pending_;
        });
        if (stop_token.stop_requested() || fade_shutdown_) break;

        const uint64_t generation = fade_generation_;
        while (fade_pending_ && fade_generation_ == generation &&
               !stop_token.stop_requested() && !fade_shutdown_) {
            const auto next_step = std::min(
                fade_started_ + fade_duration_,
                std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(10));
            fade_condition_.wait_until(lock, next_step, [this, generation,
                                                         &stop_token] {
                return stop_token.stop_requested() || fade_shutdown_ ||
                       !fade_pending_ || fade_generation_ != generation;
            });
            if (stop_token.stop_requested() || fade_shutdown_ ||
                !fade_pending_ || fade_generation_ != generation) {
                break;
            }

            const auto elapsed = std::chrono::steady_clock::now() -
                                 fade_started_;
            const double amount = std::clamp(
                std::chrono::duration<double>(elapsed).count() /
                    std::chrono::duration<double>(fade_duration_).count(),
                0.0, 1.0);
            transition_gain_ = static_cast<float>(
                fade_from_ + (fade_target_ - fade_from_) * amount);
            ApplyVolumeLocked();
            if (amount < 1.0) continue;

            fade_pending_ = false;
            const FadeCompletion completion = fade_completion_;
            const int64_t seek_target = fade_seek_target_ms_;
            const auto completed_duration = fade_duration_;
            fade_completion_ = FadeCompletion::none;
            fade_seek_target_ms_ = -1;

            if (completion == FadeCompletion::pause) {
                bool success{};
                if (backend_.load() == Backend::wave_out && device_)
                    success = waveOutPause(device_) == MMSYSERR_NOERROR;
                else if (backend_.load() == Backend::direct_sound &&
                         direct_sound_buffer_)
                    success = SUCCEEDED(direct_sound_buffer_->Stop());
                if (!success) {
                    state_ = PlaybackState::playing;
                    transition_gain_ = 1.0F;
                    ApplyVolumeLocked();
                }
            } else if (completion == FadeCompletion::seek) {
                QueueSeekLocked(seek_target);
                track_gain_ = 1.0F;
                HANDLE completion_event = completion_event_;
                if (completion_event) SetEvent(completion_event);
                // The +0xB0 transition flag makes the post-seek start use
                // FadeDuration[2].  Re-arm this same worker; no UI sleep and
                // no second thread is required.
                fade_from_ = 0.0F;
                transition_gain_ = 0.0F;
                fade_target_ = 1.0F;
                fade_started_ = std::chrono::steady_clock::now();
                fade_duration_ = completed_duration;
                fade_completion_ = FadeCompletion::seek_resume;
                fade_pending_ = true;
                ++fade_generation_;
                ApplyVolumeLocked();
                fade_condition_.notify_all();
            } else if (completion == FadeCompletion::stop) {
                // RequestStop acquires mutex_ to wake the decoder.  Release
                // it here, preserving the fade worker's single owner model.
                lock.unlock();
                RequestStop();
                lock.lock();
            }
        }
    }
}

void AudioEngine::UpdateTrackFade(int64_t position_ms) {
    std::scoped_lock lock(mutex_);
    float gain = 1.0F;
    if ((options_.sound_fade_mode & 0x10) != 0 &&
        options_.track_fade_duration > 0 &&
        duration_ms_.load() > 0 &&
        state_.load() == PlaybackState::playing) {
        const int64_t remaining = duration_ms_.load() - position_ms;
        if (remaining <= options_.track_fade_duration) {
            gain = static_cast<float>(std::clamp(
                static_cast<double>(remaining) /
                    static_cast<double>(options_.track_fade_duration),
                0.0, 1.0));
        }
    }
    if (std::abs(gain - track_gain_) < 0.0005F) return;
    track_gain_ = gain;
    ApplyVolumeLocked();
}

void AudioEngine::ApplyVolumeLocked() {
    const float effective = std::clamp(
        volume_ * transition_gain_ * track_gain_, 0.0F, 1.0F);
    const DWORD base = static_cast<DWORD>(std::lround(effective * 65535.0F));
    DWORD left = base;
    DWORD right = base;
    // FUN_004E263A: positive balance attenuates left; negative attenuates
    // right, then packs right in the high word and left in the low word.
    if (balance_ > 0) left = static_cast<DWORD>(MulDiv(base, 100 - balance_, 100));
    else if (balance_ < 0) right = static_cast<DWORD>(MulDiv(base, 100 + balance_, 100));
    if (device_) waveOutSetVolume(device_, (right << 16) | (left & 0xffff));
    if (direct_sound_buffer_) {
        const float audible = std::max(effective, 0.00001F);
        const LONG attenuation = effective <= 0.0F ? DSBVOLUME_MIN
            : static_cast<LONG>(std::clamp(
                2000.0 * std::log10(static_cast<double>(audible)),
                static_cast<double>(DSBVOLUME_MIN),
                static_cast<double>(DSBVOLUME_MAX)));
        direct_sound_buffer_->SetVolume(attenuation);
        direct_sound_buffer_->SetPan(static_cast<LONG>(
            std::clamp(balance_, -100, 100) * 100));
    }
}

std::wstring AudioEngine::LastError() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

std::wstring AudioEngine::LastDiagnostic() const {
    std::scoped_lock lock(mutex_);
    return diagnostic_;
}

AudioFormat AudioEngine::Format() const {
    std::scoped_lock lock(mutex_);
    return format_;
}

AudioMetadata AudioEngine::Metadata() const {
    std::scoped_lock lock(mutex_);
    return metadata_;
}

VisualizationSamples AudioEngine::Visualization() const {
    std::scoped_lock lock(visualization_mutex_);
    return visualization_;
}

void AudioEngine::PublishVisualization(VisualizationSamples samples) {
    std::scoped_lock lock(visualization_mutex_);
    samples.revision = visualization_.revision + 1;
    visualization_ = std::move(samples);
}

void AudioEngine::ClearVisualization() {
    std::scoped_lock lock(visualization_mutex_);
    const uint64_t revision = visualization_.revision + 1;
    visualization_ = {};
    visualization_.revision = revision;
}

void AudioEngine::RecordDiagnostic(std::wstring message) {
    std::wstring output = L"[TTPlayer audio] ";
    output += message;
    output += L"\r\n";
    OutputDebugStringW(output.c_str());

    std::scoped_lock lock(mutex_);
    if (!diagnostic_.empty()) diagnostic_ += L"\r\n";
    diagnostic_ += std::move(message);
}

void AudioEngine::SetError(std::wstring message) {
    {
        std::scoped_lock lock(mutex_);
        error_ = std::move(message);
        CancelSeekLocked();
        state_ = PlaybackState::failed;
        open_complete_ = true;
    }
    ClearVisualization();
    open_condition_.notify_all();
}
} // namespace ttplayer::audio
