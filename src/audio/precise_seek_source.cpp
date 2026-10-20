#include "ttplayer/audio/precise_seek_source.h"

#include <algorithm>
#include <limits>

namespace ttplayer::audio {
namespace {
class PreciseSeekSource final : public DecodedAudioSource {
public:
    explicit PreciseSeekSource(std::unique_ptr<DecodedAudioSource> source)
        : source_(std::move(source)) {}

    bool Open(const std::filesystem::path& path,
              const PlaybackOptions& options) override {
        ResetPosition();
        return source_->Open(path, options);
    }
    bool Read(size_t requested, std::vector<std::byte>& output, bool& end) override {
        if (FAILED(error_)) {
            output.clear();
            end = true;
            return false;
        }
        if (!source_->Read(requested, output, end)) return false;
        if (!discard_) return true;
        const auto alignment = source_->OutputFormat().nBlockAlign;
        if (!alignment || output.size() % alignment) {
            output.clear();
            end = true;
            error_ = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            return false;
        }
        const auto removed = static_cast<size_t>(
            std::min<std::uint64_t>(discard_, output.size()));
        discard_ -= removed;
        output.erase(output.begin(), output.begin() + removed);
        // EOF before the requested point is an empty natural tail. Do not
        // manufacture PCM or retain a prefix for the next seek/reset.
        if (end) discard_ = 0;
        return true;
    }
    bool Seek(std::chrono::milliseconds requested) override {
        ResetPosition();
        if (!source_->Seek(requested)) return false;
        actual_ = source_->LastSeekPosition();
        if (!actual_ || *actual_ >= requested) return true;
        const auto& format = source_->OutputFormat();
        if (!format.nBlockAlign || !format.nAvgBytesPerSec ||
            actual_->count() < 0 || requested.count() < 0) {
            error_ = E_INVALIDARG;
            return false;
        }
        // Original MulDiv(delta, bytesPerSecond, 1000), rounded to the nearest
        // byte, then down to a complete PCM frame. Split the product so large
        // durations cannot overflow before validation.
        const auto delta = static_cast<std::uint64_t>(requested.count()) -
                           static_cast<std::uint64_t>(actual_->count());
        const auto seconds = delta / 1000;
        const auto fractional = (delta % 1000 * format.nAvgBytesPerSec + 500) / 1000;
        const auto limit = std::numeric_limits<std::uint64_t>::max();
        if (seconds > (limit - fractional) / format.nAvgBytesPerSec) {
            error_ = E_INVALIDARG;
            return false;
        }
        discard_ = seconds * format.nAvgBytesPerSec + fractional;
        discard_ -= discard_ % format.nBlockAlign;
        // The outer playback clock stays at the requested position. Prefix
        // removal happens before ReplayGain, effects and resampling in Read.
        actual_ = requested;
        return true;
    }
    std::optional<std::chrono::milliseconds> LastSeekPosition() const override {
        return actual_;
    }
    void SetPaused(bool paused) override { source_->SetPaused(paused); }
    int DownloadedPercent() override { return source_->DownloadedPercent(); }
    std::shared_ptr<ArchivePlaybackSession> ArchiveSession() const override {
        return source_->ArchiveSession();
    }
    const WAVEFORMATEX& OutputFormat() const override { return source_->OutputFormat(); }
    AudioFormat DisplayFormat() const override { return source_->DisplayFormat(); }
    std::chrono::milliseconds Duration() const override { return source_->Duration(); }
    std::wstring Error() const override {
        return FAILED(error_) ? L"解码器定位后返回了无效的 PCM 格式或数据。" : source_->Error();
    }
    HRESULT ErrorResult() const override {
        return FAILED(error_) ? error_ : source_->ErrorResult();
    }
    AudioMetadata Metadata() const override { return source_->Metadata(); }
    std::uint64_t MetadataRevision() const noexcept override { return source_->MetadataRevision(); }
    std::wstring SourceIdentity() const override { return source_->SourceIdentity(); }
    bool CanOverlapPlayback() const override { return source_->CanOverlapPlayback(); }
    HRESULT WriteLyrics(std::wstring_view lyrics) override { return source_->WriteLyrics(lyrics); }
    HRESULT FinishLyrics() override { return source_->FinishLyrics(); }

private:
    void ResetPosition() noexcept {
        discard_ = 0;
        actual_.reset();
        error_ = S_OK;
    }
    std::unique_ptr<DecodedAudioSource> source_;
    std::uint64_t discard_{};
    std::optional<std::chrono::milliseconds> actual_;
    HRESULT error_{S_OK};
};
} // namespace

std::unique_ptr<DecodedAudioSource> MakePreciseSeekSource(
    std::unique_ptr<DecodedAudioSource> source) {
    return source ? std::make_unique<PreciseSeekSource>(std::move(source)) : nullptr;
}
} // namespace ttplayer::audio
