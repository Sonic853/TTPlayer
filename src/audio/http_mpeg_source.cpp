#include "ttplayer/audio/http_mpeg_source.h"
#include "ttplayer/audio/pcm_output_transform.h"
#include <ttpcomm/client.h>
#include <wrl/client.h>
#include <mmreg.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace ttplayer::audio {
namespace {
struct Frame { unsigned rate{}, channels{}, bytes{}, samples{}; };
bool Header(const unsigned char* p, Frame& f) {
    const unsigned v = unsigned(p[0]) << 24 | unsigned(p[1]) << 16 | unsigned(p[2]) << 8 | p[3];
    if ((v & 0xffe00000) != 0xffe00000) return false;
    const unsigned version = (v >> 19) & 3, layer = 4 - ((v >> 17) & 3), index = (v >> 12) & 15, frequency = (v >> 10) & 3;
    if (version == 1 || layer == 4 || !index || index == 15 || frequency == 3) return false;
    static constexpr unsigned rate[]{44100,48000,32000};
    static constexpr unsigned rates[2][3][15]{
        {{0,32,64,96,128,160,192,224,256,288,320,352,384,416,448},
         {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384},
         {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320}},
        {{0,32,48,56,64,80,96,112,128,144,160,176,192,224,256},
         {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160},
         {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}}};
    f.rate = rate[frequency] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    f.channels = ((v >> 6) & 3) == 3 ? 1 : 2;
    const unsigned bitrate = rates[version != 3][layer - 1][index] * 1000;
    const unsigned padding = (v >> 9) & 1;
    f.samples = layer == 1 ? 384 : layer == 3 && version != 3 ? 576 : 1152;
    f.bytes = layer == 1 ? (12 * bitrate / f.rate + padding) * 4 :
        (layer == 3 && version != 3 ? 72 : 144) * bitrate / f.rate + padding;
    return f.bytes >= 4 && f.bytes < 4096;
}
struct DecoderIo { const unsigned char* input; unsigned size, consumed; double* output; unsigned capacity, produced; };
class Source final : public DecodedAudioSource {
public:
    Source(HMODULE comm, IStream* stream) : module_(comm), stream_(stream) {}
    ~Source() override { if (decoder_) destroy_(decoder_); }
    bool Open(const std::filesystem::path&, const PlaybackOptions&) override {
        auto module = module_.get();
        create_ = reinterpret_cast<decltype(create_)>(GetProcAddress(module, MAKEINTRESOURCEA(11)));
        destroy_ = reinterpret_cast<decltype(destroy_)>(GetProcAddress(module, MAKEINTRESOURCEA(12)));
        process_ = reinterpret_cast<decltype(process_)>(GetProcAddress(module, MAKEINTRESOURCEA(13)));
        if (!create_ || !destroy_ || !process_) return Fail(E_NOINTERFACE);
        if (!Fill(10)) return false;
        if (input_.size() >= 10 && !memcmp(input_.data(), "ID3", 3)) {
            if ((input_[6] | input_[7] | input_[8] | input_[9]) & 0x80) return Fail(E_INVALIDARG);
            const unsigned size = input_[6] << 21 | input_[7] << 14 | input_[8] << 7 | input_[9];
            if (size > 16 * 1024 * 1024) return Fail(E_INVALIDARG);
            LARGE_INTEGER at{}; at.QuadPart = size + 10 + ((input_[3] == 4 && (input_[5] & 0x10)) ? 10 : 0);
            const HRESULT hr = stream_->Seek(at, STREAM_SEEK_SET, nullptr);
            if (FAILED(hr)) return Fail(hr);
            input_.clear();
        }
        for (unsigned skipped = 0; skipped < 65536; ++skipped) {
            if (!Fill(4104)) return false;
            if (input_.size() < 8) return Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            Frame next;
            if (Header(input_.data(), frame_) && input_.size() >= frame_.bytes + 4 &&
                Header(input_.data() + frame_.bytes, next) && next.rate == frame_.rate && next.channels == frame_.channels) {
                wave_ = {WAVE_FORMAT_IEEE_FLOAT, static_cast<WORD>(frame_.channels), frame_.rate,
                    frame_.rate * frame_.channels * 8, static_cast<WORD>(frame_.channels * 8), 64, 0};
                decoder_ = create_(0); return decoder_ ? true : Fail(E_OUTOFMEMORY);
            }
            input_.erase(input_.begin());
        }
        return Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    }
    bool Read(size_t requested, std::vector<std::byte>& output, bool& end) override {
        output.clear(); end = false;
        requested = std::max<size_t>(wave_.nBlockAlign, requested / wave_.nBlockAlign * wave_.nBlockAlign);
        while (output.size() < requested) {
            if (pcm_offset_ < pcm_.size()) {
                const auto n = std::min(requested - output.size(), pcm_.size() - pcm_offset_);
                output.insert(output.end(), pcm_.begin() + pcm_offset_, pcm_.begin() + pcm_offset_ + n); pcm_offset_ += n;
                continue;
            }
            if (!Fill(4)) return false;
            if (eof_ && input_.empty()) {
                if (converter_ && !converter_flushed_) {
                    if(!converter_->Process({},pcm_,true)) return Fail(E_FAIL);
                    converter_flushed_=true;pcm_offset_=0;
                    if(!pcm_.empty()) continue;
                }
                end = true; break;
            }
            Frame current;
            if (input_.size()<4 || !Header(input_.data(),current)) {
                // ID3v1 is a finite trailing tag, not a broken MPEG frame.
                if(input_.size()>=3 && !memcmp(input_.data(),"TAG",3)) {
                    if(!Fill(129)) return false;
                    if(eof_ && input_.size()==128){input_.clear();continue;}
                }
                return Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            }
            if(!Fill(current.bytes+8)) return false;
            if(input_.size()<current.bytes) return Fail(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));
            pcm_.clear();pcm_offset_=0;
            if(current.rate!=frame_.rate || current.channels!=frame_.channels) {
                if(converter_ && !converter_->Process({},pcm_,true)) return Fail(E_FAIL);
                destroy_(decoder_);decoder_=create_(0);if(!decoder_) return Fail(E_OUTOFMEMORY);
                frame_=current;
                WAVEFORMATEX format=wave_;format.nSamplesPerSec=current.rate;
                format.nAvgBytesPerSec=format.nSamplesPerSec*format.nBlockAlign;
                converter_=std::make_unique<PcmOutputTransform>();
                if(!converter_->Open(format,{64,static_cast<int>(wave_.nSamplesPerSec),1,0,true},module_.get())) return Fail(E_FAIL);
                converter_flushed_=false;
            }
            const size_t available = input_.size();
            if (eof_) input_.resize(available + 8); // Original decoder requires eight look-ahead bytes.
            std::array<double, 4096> samples{};
            DecoderIo io{input_.data(), static_cast<unsigned>(input_.size()), 0,
                samples.data(), static_cast<unsigned>(sizeof(samples)), 0};
            const int result = process_(decoder_, &io);
            if (io.consumed > input_.size() || io.produced > sizeof(samples) || io.produced % (current.channels*8))
                return Fail(E_UNEXPECTED);
            input_.resize(available);
            const auto consumed = std::min<size_t>(io.consumed, available);
            input_.erase(input_.begin(), input_.begin() + consumed);
            if (io.produced) {
                std::vector<std::byte> mapped(io.produced/current.channels*wave_.nChannels);
                for(size_t i=0;i<io.produced/8/current.channels;++i) for(unsigned c=0;c<wave_.nChannels;++c) {
                    const double value=current.channels==wave_.nChannels?samples[i*current.channels+c]:
                        current.channels==1?samples[i]:(samples[i*2]+samples[i*2+1])/2;
                    memcpy(mapped.data()+(i*wave_.nChannels+c)*8,&value,8);
                }
                if(converter_) {
                    std::vector<std::byte> converted;
                    if(!converter_->Process(mapped,converted,false)) return Fail(E_FAIL);
                    pcm_.insert(pcm_.end(),converted.begin(),converted.end());
                } else pcm_.insert(pcm_.end(),mapped.begin(),mapped.end());
            }
            if (!consumed && !io.produced) {
                if (eof_) { end = true; break; }
                return Fail(result < 0 ? E_FAIL : HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            }
        }
        return true;
    }
    // Lengthless/live streams have no stable time-to-byte mapping. Do not
    // pretend a seek succeeded or restart the stream at an unrelated offset.
    bool Seek(std::chrono::milliseconds) override { return Fail(E_NOTIMPL); }
    bool CanOverlapPlayback() const override { return false; }
    const WAVEFORMATEX& OutputFormat() const override { return wave_; }
    std::uint32_t PacketBytes() const override { return frame_.samples * wave_.nBlockAlign; }
    AudioFormat DisplayFormat() const override { return {WAVE_FORMAT_MPEGLAYER3,wave_.nChannels,wave_.nSamplesPerSec,0,wave_.nBlockAlign,64,L"MPEG Audio (HTTP)"}; }
    std::chrono::milliseconds Duration() const override { return {}; }
    std::wstring Error() const override { return L"HTTP MPEG decoder"; }
    HRESULT ErrorResult() const override { return error_; }
private:
    bool Fail(HRESULT result) { error_ = result; return false; }
    bool Fill(size_t count) {
        while (!eof_ && input_.size() < count) {
            std::array<unsigned char,16384> bytes{}; ULONG got{};
            const HRESULT hr = stream_->Read(bytes.data(), static_cast<ULONG>(count - input_.size()), &got);
            if (FAILED(hr)) return Fail(hr);
            input_.insert(input_.end(),bytes.begin(),bytes.begin()+got);
            if (hr == S_FALSE || !got) eof_ = true;
        }
        return true;
    }
    ttpcomm::client::ModuleReference module_;
    Microsoft::WRL::ComPtr<IStream> stream_;
    void* (__cdecl* create_)(unsigned){}; void (__cdecl* destroy_)(void*){};
    int (__cdecl* process_)(void*, DecoderIo*){};
    void* decoder_{};
    HRESULT error_{S_OK}; WAVEFORMATEX wave_{}; Frame frame_{};
    std::vector<unsigned char> input_;
    std::vector<std::byte> pcm_; size_t pcm_offset_{}; bool eof_{};
    std::unique_ptr<PcmOutputTransform> converter_;bool converter_flushed_{};
};
}
std::unique_ptr<DecodedAudioSource> CreateHttpMpegSource(HMODULE comm, IStream* stream) {
    return std::make_unique<Source>(comm, stream);
}
}
