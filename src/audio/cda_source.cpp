#include "ttplayer/audio/cda_source.h"
#include "ttplayer/core/text.h"
#include "ttplayer/plugins/plugin_manager.h"
#include <algorithm>
#include <cmath>
#include <system_error>
namespace ttplayer::audio {
namespace {
std::optional<double> MetadataNumber(std::wstring_view text) {
    const std::wstring value(text); wchar_t* end{};
    const double number=wcstod(value.c_str(),&end);
    return end!=value.c_str() && std::isfinite(number) ? std::optional(number) : std::nullopt;
}
class NativeCdDevice final : public CdAudioDevice {
    HANDLE drive_{INVALID_HANDLE_VALUE};
public:
    ~NativeCdDevice() override {if(drive_!=INVALID_HANDLE_VALUE)CloseHandle(drive_);}
    DiscLayout Open(const std::filesystem::path& path) override {
        if(drive_!=INVALID_HANDLE_VALUE) {CloseHandle(drive_);drive_=INVALID_HANDLE_VALUE;}
        drive_=OpenCdDrive(path);
        CDROM_TOC toc{};DWORD returned{};
        if(!DeviceIoControl(drive_,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,sizeof(toc),&returned,nullptr))
            throw std::system_error(GetLastError(),std::system_category(),"CD TOC");
        auto disc=ParseDiscToc(toc,returned);
        const auto root=path.root_name().wstring()+L"\\";
        GetVolumeInformationW(root.c_str(),nullptr,0,&disc.serial,nullptr,nullptr,nullptr,0);
        return disc;
    }
    DWORD Read(unsigned lba,unsigned sectors,std::span<std::byte> output,DWORD& bytes) override {
        RAW_READ_INFO request{};request.DiskOffset.QuadPart=LONGLONG(lba)*2048;
        request.SectorCount=sectors;request.TrackMode=CDDA;bytes=0;
        return DeviceIoControl(drive_,IOCTL_CDROM_RAW_READ,&request,sizeof(request),output.data(),
            static_cast<DWORD>(output.size()),&bytes,nullptr) ? ERROR_SUCCESS : GetLastError();
    }
};
bool ProbeDtsCd(HMODULE comm, const std::byte* bytes, DWORD length, WAVEFORMATEX* format) noexcept {
    const auto entry = comm ? GetProcAddress(comm, MAKEINTRESOURCEA(106)) : nullptr;
    if (!entry) return false;
    using Probe = int(__cdecl*)(const void*, DWORD, WAVEFORMATEX*);
    __try { return reinterpret_cast<Probe>(entry)(bytes, length, format) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

class CdaSource final : public DecodedAudioSource {
public:
    CdaSource(const plugins::PluginManager* manager, HMODULE comm, std::unique_ptr<CdAudioDevice> device)
        : manager_(manager), comm_(comm), device_(std::move(device)) {}
    bool CanOverlapPlayback() const override { return false; }

    bool Open(const std::filesystem::path& path,const PlaybackOptions&) override {
        try {
            error_.clear();
            path_=path;decoder_.reset();disc_=device_->Open(path);const auto number=CdaTrackNumber(path);
            const auto& track=FindDiscTrack(disc_,number);
            start_=track.start-150;end_=track.end-150;cursor_=start_;

            wave_={WAVE_FORMAT_PCM,2,44100,176400,4,16,0};
            format_.format_tag=WAVE_FORMAT_PCM;format_.channels=2;
            format_.sample_rate=44100;format_.bytes_per_second=176400;
            format_.block_align=4;format_.bits_per_sample=16;format_.codec_name=L"CD|CD Audio";
            duration_=std::chrono::milliseconds((uint64_t(end_-start_)*1000+37)/75);
            metadata_={};metadata_.title=L"Track "+std::to_wstring(number);
            try {
                auto cache=ReadDiscMetadata(disc_);metadata_.entries=cache[number];
                const auto title=DiscField(metadata_.entries,L"Title");if(!title.empty())metadata_.title=title;
                metadata_.artist=DiscField(metadata_.entries,L"Artist");metadata_.album=DiscField(metadata_.entries,L"Album");
                metadata_.replay_gain_db=MetadataNumber(DiscField(metadata_.entries,L"replaygain_track_gain"));
                metadata_.replay_peak=MetadataNumber(DiscField(metadata_.entries,L"replaygain_track_peak"));
            } catch(...) { /* A damaged local cache must not prevent audio playback. */ }
            metadata_.entries.emplace_back(L"CDDBSerialNumber", std::to_wstring(disc_.serial));
            // Original reader probes up to 48 sectors before exposing PCM.
            pending_.clear();offset_=0;
            while(cursor_<end_ && pending_.size()<48*2352)Fill();
            WAVEFORMATEX encoded{};
            auto probe = pending_; probe.resize(probe.size() + 16); // Guard the legacy sync parser's lookahead.
            if (ProbeDtsCd(comm_, probe.data(), static_cast<DWORD>(pending_.size()), &encoded)) {
                decoder_ = manager_ ? manager_->OpenDecoder(encoded) : nullptr;
                if (!decoder_) throw std::runtime_error("DTS-CD detected, but no compatible DTS decoder is installed");
                format_.format_tag = encoded.wFormatTag; format_.sample_rate = encoded.nSamplesPerSec;
                format_.channels = encoded.nChannels; format_.bytes_per_second = encoded.nAvgBytesPerSec;
                format_.bits_per_sample = encoded.wBitsPerSample; format_.block_align = encoded.nBlockAlign;
                format_.codec_name = L"DTS-CD";
            } else if (ContainsDtsCd(pending_)) {
                throw std::runtime_error("DTS-CD detected, but its stream format could not be validated");
            }
            seek_position_=std::chrono::milliseconds(0);return true;
        } catch(const std::exception& e) {error_=core::Utf8ToWide(e.what());return false;}
    }
    bool Read(size_t requested,std::vector<std::byte>& output,bool& eof) override {
        output.clear();eof=false;
        try {
            if (decoder_) {
                for (unsigned turn = 0; turn < 256; ++turn) {
                    const auto available = decoder_->OutputAvailable();
                    if (FAILED(available)) throw std::runtime_error("DTS decoder output failed");
                    if (available == S_OK) {
                        const auto hr = decoder_->ReadOutput(requested, output, eof);
                        if (FAILED(hr)) throw std::runtime_error("DTS decoding failed");
                        if (!output.empty() || eof) return true;
                    }
                    const auto needs = decoder_->NeedsInput();
                    if (FAILED(needs)) throw std::runtime_error("DTS decoder input failed");
                    if (needs == S_OK) {
                        if (offset_ == pending_.size()) {
                            pending_.clear(); offset_ = 0;
                            if (cursor_ == end_) { eof = true; return true; }
                            Fill();
                        }
                        DWORD input{}, decoded{};
                        if (FAILED(decoder_->BufferSizes(input, decoded))) throw std::runtime_error("DTS decoder buffer query failed");
                        const auto count = std::min<size_t>(pending_.size()-offset_, std::clamp<DWORD>(input,4704,1024*1024));
                        if (FAILED(decoder_->PushInput(std::span(pending_).subspan(offset_,count)))) throw std::runtime_error("DTS decoder rejected CD data");
                        offset_ += count;
                    }
                }
                throw std::runtime_error("DTS decoder made no progress");
            }
            // Keep excess sectors locally; never exceed caller capacity or repeat a short sector.
            const auto capacity=std::min<size_t>(requested-requested%4,24*2352);
            if(!capacity) {eof=cursor_==end_ && offset_==pending_.size();return true;}
            if(offset_==pending_.size()) {pending_.clear();offset_=0;if(cursor_<end_)Fill();}
            const auto count=std::min(capacity,pending_.size()-offset_);
            output.assign(pending_.begin()+offset_,pending_.begin()+offset_+count);offset_+=count;
            eof=cursor_==end_ && offset_==pending_.size();return true;
        } catch(const std::exception& e){error_=core::Utf8ToWide(e.what());return false;}
    }
    bool Seek(std::chrono::milliseconds position) override {
        if(position.count()<0 || position.count()>duration_.count())return false;
        const uint64_t relative=(uint64_t(position.count())*75+500)/1000;
        if(relative>=end_-start_)return false; // Failed seek leaves the cursor and buffer untouched.
        if (decoder_ && FAILED(decoder_->Reset())) return false;
        cursor_=start_+static_cast<unsigned>(relative);pending_.clear();offset_=0;
        seek_position_=std::chrono::milliseconds((relative*1000+37)/75);return true;
    }
    std::optional<std::chrono::milliseconds> LastSeekPosition() const override {return seek_position_;}
    const WAVEFORMATEX& OutputFormat() const override {return decoder_ ? decoder_->OutputFormat() : wave_;}
    AudioFormat DisplayFormat() const override {return format_;}
    std::chrono::milliseconds Duration() const override {return duration_;}
    std::wstring Error() const override {return error_;}
    AudioMetadata Metadata() const override {return metadata_;}
    std::wstring SourceIdentity() const override {return disc_.Fingerprint();}
private:
    void Fill() {
        unsigned attempted=std::min<unsigned>(24,end_-cursor_);
        bool reopened=false;DWORD returned{};
        std::vector<std::byte> buffer;
        while(attempted) {
            buffer.resize(attempted*2352);
            const DWORD error=device_->Read(cursor_,attempted,buffer,returned);
            if(error==ERROR_SUCCESS) {
                const auto completed=CheckedCdRead(attempted,returned);
                buffer.resize(returned);pending_.insert(pending_.end(),buffer.begin(),buffer.end());cursor_+=completed;return;
            }
            if(!reopened && (error==ERROR_INVALID_HANDLE || error==ERROR_MEDIA_CHANGED)) {
                // Reopen only if the complete TOC/serial still identifies the same disc.
                if(device_->Open(path_)!=disc_)throw std::runtime_error("CD was replaced during playback");
                reopened=true;continue;
            }
            if(error==ERROR_NOT_READY || error==ERROR_NO_MEDIA_IN_DRIVE || error==ERROR_MEDIA_CHANGED || attempted<=1)
                throw std::system_error(error,std::system_category(),"CD raw read");
            attempted=attempted>=8?attempted-4:attempted/2;
        }
        throw std::runtime_error("CD raw read made no progress");
    }
    const plugins::PluginManager* manager_{}; HMODULE comm_{};
    std::unique_ptr<plugins::LegacyDecoderSession> decoder_;
    std::unique_ptr<CdAudioDevice> device_;std::filesystem::path path_;DiscLayout disc_;
    unsigned start_{},end_{},cursor_{};std::vector<std::byte> pending_;size_t offset_{};
    WAVEFORMATEX wave_{};AudioFormat format_{};AudioMetadata metadata_;
    std::chrono::milliseconds duration_{},seek_position_{};std::wstring error_;
};

}
std::unique_ptr<DecodedAudioSource> CreateCdaSource(const plugins::PluginManager* manager,HMODULE comm,
    std::unique_ptr<CdAudioDevice> device) {
    if(!device)device=std::make_unique<NativeCdDevice>();
    return std::make_unique<CdaSource>(manager,comm,std::move(device));
}
}
