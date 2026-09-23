#include "ttplayer/audio/legacy_windows_source.h"
#include "ttplayer/audio/archive_member.h"
#include "ttplayer/platform/optional_windows_api.h"

#include <wmsdk.h>
#include <nserror.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

namespace ttplayer::audio {
namespace {
using Microsoft::WRL::ComPtr;

class WindowsMediaSource final : public DecodedAudioSource {
public:
    explicit WindowsMediaSource(HMODULE ttpcomm) : ttpcomm_(ttpcomm) {}
    ~WindowsMediaSource() override { if (reader_) reader_->Close(); }

    bool Open(const std::filesystem::path& path, const PlaybackOptions&) override {
        HRESULT result = platform::WMCreateSyncReader(nullptr, 0, &reader_);
        if (FAILED(result)) return Fail(L"Windows Media Format runtime is unavailable", result);
        ArchiveMemberPath member;
        if (ParseArchiveMemberPath(path.native(), member)) {
            try {
                const auto bytes = ReadArchiveMember(member, ttpcomm_);
                if (bytes.size() > UINT_MAX) return Fail(L"Archive member is too large", E_INVALIDARG);
                stream_.Attach(platform::SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
            } catch (const std::exception&) {
                return Fail(L"Cannot read archive member", E_FAIL);
            }
            result = stream_ ? reader_->OpenStream(stream_.Get()) : E_OUTOFMEMORY;
        } else result = reader_->Open(path.c_str());
        if (FAILED(result)) return Fail(L"Windows Media cannot decode this format", result);
        DWORD count{};
        result = reader_->GetOutputCount(&count);
        if (FAILED(result)) return Fail(L"GetOutputCount", result);
        bool found{};
        for (DWORD output = 0; output < count && !found; ++output) {
            ComPtr<IWMOutputMediaProps> properties;
            if (FAILED(reader_->GetOutputProps(output, &properties))) continue;
            GUID major{};
            if (FAILED(properties->GetType(&major)) || major != MFMediaType_Audio) continue;
            DWORD size{};
            if (FAILED(properties->GetMediaType(nullptr, &size)) ||
                size < sizeof(WM_MEDIA_TYPE) || size > 1024 * 1024) continue;
            std::vector<BYTE> storage(size);
            auto* type = reinterpret_cast<WM_MEDIA_TYPE*>(storage.data());
            if (FAILED(properties->GetMediaType(type, &size)) || !type->pbFormat ||
                type->cbFormat < 16 || type->cbFormat > sizeof(format_)) continue;
            format_ = {};
            std::memcpy(&format_, type->pbFormat, type->cbFormat);
            const auto& wave = format_.Format;
            if (wave.wFormatTag != WAVE_FORMAT_PCM && wave.wFormatTag != WAVE_FORMAT_IEEE_FLOAT &&
                wave.wFormatTag != WAVE_FORMAT_EXTENSIBLE) continue;
            if (!wave.nChannels || !wave.nSamplesPerSec || !wave.nBlockAlign ||
                !wave.nAvgBytesPerSec || !wave.wBitsPerSample) continue;
            if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                (type->cbFormat < sizeof(format_) || wave.cbSize < 22 ||
                 (format_.SubFormat != MFAudioFormat_PCM && format_.SubFormat != MFAudioFormat_Float))) continue;
            if (wave.nBlockAlign != static_cast<uint64_t>(wave.nChannels) * ((wave.wBitsPerSample + 7U) / 8U) ||
                wave.nAvgBytesPerSec != static_cast<uint64_t>(wave.nSamplesPerSec) * wave.nBlockAlign) continue;
            result = reader_->GetStreamNumberForOutput(output, &stream_number_);
            if (SUCCEEDED(result)) result = reader_->SetReadStreamSamples(stream_number_, FALSE);
            if (FAILED(result)) return Fail(L"Select decoded audio stream", result);
            found = true;
        }
        if (!found) return Fail(L"Windows Media returned no PCM audio output", E_FAIL);
        ComPtr<IWMHeaderInfo> header;
        if (SUCCEEDED(reader_.As(&header))) {
            WORD stream = 0, size = sizeof(QWORD);
            WMT_ATTR_DATATYPE type{};
            QWORD duration{};
            if (SUCCEEDED(header->GetAttributeByName(&stream, L"Duration", &type,
                    reinterpret_cast<BYTE*>(&duration), &size)) && type == WMT_TYPE_QWORD)
                duration_ = std::chrono::milliseconds(duration / 10000);
        }
        return true;
    }

    bool Read(size_t requested, std::vector<std::byte>& output, bool& end) override {
        output.clear();
        requested = std::max<size_t>(requested - requested % format_.Format.nBlockAlign,
                                      format_.Format.nBlockAlign);
        while (output.size() < requested) {
            if (pending_offset_ < pending_.size()) {
                const size_t count = std::min(requested - output.size(), pending_.size() - pending_offset_);
                output.insert(output.end(), pending_.begin() + pending_offset_,
                              pending_.begin() + pending_offset_ + count);
                pending_offset_ += count;
                continue;
            }
            if (ended_) break;
            ComPtr<INSSBuffer> sample;
            QWORD time{}, duration{};
            DWORD flags{}, output_number{};
            WORD stream{};
            const HRESULT result = reader_->GetNextSample(stream_number_, &sample,
                &time, &duration, &flags, &output_number, &stream);
            if (result == NS_E_NO_MORE_SAMPLES) { ended_ = true; break; }
            if (FAILED(result)) return Fail(L"Windows Media GetNextSample", result);
            BYTE* bytes{};
            DWORD size{};
            if (!sample || FAILED(sample->GetBufferAndLength(&bytes, &size)) ||
                (size && !bytes) || size % format_.Format.nBlockAlign)
                return Fail(L"Windows Media returned invalid PCM", E_FAIL);
            pending_.resize(size);
            if (size) std::memcpy(pending_.data(), bytes, size);
            pending_offset_ = 0;
        }
        end = ended_ && pending_offset_ == pending_.size();
        return true;
    }
    bool Seek(std::chrono::milliseconds position) override {
        const HRESULT result = reader_->SetRange(static_cast<QWORD>(std::max<int64_t>(0, position.count())) * 10000, 0);
        if (FAILED(result)) return Fail(L"Windows Media SetRange", result);
        pending_.clear(); pending_offset_ = 0; ended_ = false;
        return true;
    }
    const WAVEFORMATEX& OutputFormat() const override { return format_.Format; }
    AudioFormat DisplayFormat() const override {
        const auto& wave = format_.Format;
        AudioFormat result{};
        result.format_tag = wave.wFormatTag;
        result.channels = wave.nChannels;
        result.sample_rate = wave.nSamplesPerSec;
        result.bytes_per_second = wave.nAvgBytesPerSec;
        result.block_align = wave.nBlockAlign;
        result.bits_per_sample = wave.wBitsPerSample;
        result.codec_name = L"Windows Media / PCM";
        return result;
    }
    std::chrono::milliseconds Duration() const override { return duration_; }
    std::wstring Error() const override { return error_; }
    HRESULT ErrorResult() const override { return error_result_; }
private:
    bool Fail(const wchar_t* operation, HRESULT result) {
        error_result_ = result;
        error_ = std::wstring(operation) + L" (HRESULT " + std::to_wstring(result) + L")";
        return false;
    }
    HMODULE ttpcomm_{};
    ComPtr<IWMSyncReader> reader_;
    ComPtr<IStream> stream_;
    WAVEFORMATEXTENSIBLE format_{};
    WORD stream_number_{};
    std::chrono::milliseconds duration_{};
    std::vector<std::byte> pending_;
    size_t pending_offset_{};
    bool ended_{};
    std::wstring error_;
    HRESULT error_result_{E_FAIL};
};
} // namespace

std::unique_ptr<DecodedAudioSource> CreateLegacyWindowsSource(HMODULE ttpcomm) {
    return std::make_unique<WindowsMediaSource>(ttpcomm);
}
} // namespace ttplayer::audio
