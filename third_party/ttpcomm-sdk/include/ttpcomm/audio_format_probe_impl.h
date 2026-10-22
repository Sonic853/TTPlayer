#pragma once
#include "audio_format_probe.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>

// Shared bounded recognizers. Plugins may implement Probe themselves for new
// formats; the host does not require this implementation or its family enum.
namespace ttp::format {
template<class C,size_t N> inline void Copy(C (&to)[N],const C* from) noexcept {
    size_t n=0;if(from)while(n+1<N && from[n]){to[n]=from[n];++n;}to[n]=0;
}
inline bool Valid(const Result& r) noexcept {
    if(r.size!=sizeof(Result)||r.version!=abi_version||r.confidence>Opened)return false;
    if(!memchr(r.container,0,sizeof(r.container))||!memchr(r.codec,0,sizeof(r.codec))||
       !wmemchr(r.extension,0,16)||!wmemchr(r.aliases,0,160)||!wmemchr(r.display,0,80))return false;
    if(r.confidence<Tentative)return true;
    if(r.extension[0]!=L'.'||!r.extension[1])return false;
    for(size_t i=1;r.extension[i];++i)if(!((r.extension[i]>=L'a'&&r.extension[i]<=L'z')||
        (r.extension[i]>=L'0'&&r.extension[i]<=L'9')||r.extension[i]==L'+'))return false;
    return true;
}
inline bool Accepts(const Result& r,const wchar_t* extension) noexcept {
    if(!Valid(r)||!extension)return false;
    if(!_wcsicmp(r.extension,extension))return true;
    for(const wchar_t* p=r.aliases;*p;) {
        const wchar_t* e=wcschr(p,L';');const size_t n=e?size_t(e-p):wcslen(p);
        if(wcslen(extension)==n&&!_wcsnicmp(p,extension,n))return true;
        if(!e)break;p=e+1;
    }
    return false;
}
struct View {
    const BYTE* p;size_t n;
    bool is(size_t at,const char* value,size_t count) const noexcept {
        return at<=n&&count<=n-at&&!memcmp(p+at,value,count);
    }
    bool is(size_t at,const char* value) const noexcept{return is(at,value,strlen(value));}
    unsigned be32(size_t at)const noexcept{return (unsigned(p[at])<<24)|(unsigned(p[at+1])<<16)|(unsigned(p[at+2])<<8)|p[at+3];}
    unsigned le16(size_t at)const noexcept{return p[at]|(unsigned(p[at+1])<<8);}
};
inline size_t MpegFrame(View b,size_t at,unsigned& identity,unsigned& layer) noexcept {
    if(at>b.n||b.n-at<4||b.p[at]!=255||(b.p[at+1]&0xe0)!=0xe0)return 0;
    const unsigned v=(b.p[at+1]>>3)&3,rate=(b.p[at+2]>>2)&3,bit=b.p[at+2]>>4;
    layer=4-((b.p[at+1]>>1)&3);
    if(v==1||layer==4||rate==3||bit==0||bit==15)return 0;
    constexpr unsigned rates[]{44100,48000,32000};
    constexpr unsigned high[3][14]{{32,64,96,128,160,192,224,256,288,320,352,384,416,448},
        {32,48,56,64,80,96,112,128,160,192,224,256,320,384},{32,40,48,56,64,80,96,112,128,160,192,224,256,320}};
    constexpr unsigned low[2][14]{{32,48,56,64,80,96,112,128,144,160,176,192,224,256},{8,16,24,32,40,48,56,64,80,96,112,128,144,160}};
    const unsigned kbps=v==3?high[layer-1][bit-1]:low[layer==1?0:1][bit-1];
    const unsigned hz=rates[rate]/(v==3?1:v==2?2:4),pad=(b.p[at+2]>>1)&1;
    identity=(v<<8)|(layer<<4)|rate;
    return layer==1?(12000*kbps/hz+pad)*4:((layer==3&&v!=3)?72000:144000)*kbps/hz+pad;
}
inline size_t AdtsFrame(View b,size_t at) noexcept {
    if(at>b.n||b.n-at<7||b.p[at]!=255||(b.p[at+1]&0xf6)!=0xf0||((b.p[at+2]>>2)&15)>=13)return 0;
    const size_t n=((b.p[at+3]&3u)<<11)|(unsigned(b.p[at+4])<<3)|(b.p[at+5]>>5);
    return n>=((b.p[at+1]&1)?7u:9u)?n:0;
}
// DTS core header normalized from 16/14-bit, big/little-endian words.
// A sync word alone is not evidence that PCM contains compressed audio.
inline size_t DtsFrame(View b,size_t at,unsigned& identity) noexcept {
    if(at>b.n||b.n-at<16)return 0;
    const bool little=b.is(at,"\xfe\x7f\x01\x80",4)||b.is(at,"\xff\x1f\x00\xe8",4);
    const bool packed=b.is(at,"\x1f\xff\xe8\x00",4)||b.is(at,"\xff\x1f\x00\xe8",4);
    if(!little&&!packed&&!b.is(at,"\x7f\xfe\x80\x01",4))return 0;
    BYTE h[12]{};unsigned bit=0;
    for(unsigned i=0;i<16&&bit<96;i+=2){
        const unsigned word=little?b.le16(at+i):(unsigned(b.p[at+i])<<8)|b.p[at+i+1];
        for(int j=packed?13:15;j>=0&&bit<96;--j,++bit)h[bit/8]|=BYTE(((word>>j)&1)<<(7-bit%8));
    }
    const size_t bytes=(((h[5]&3u)<<12)|(unsigned(h[6])<<4)|(h[7]>>4))+1;
    const unsigned blocks=(((h[4]&1u)<<6)|(h[5]>>2))+1;
    const unsigned rate=(h[8]>>2)&15,channels=((h[7]&15u)<<2)|(h[8]>>6);
    constexpr unsigned rates[]{0,8000,16000,32000,0,0,11025,22050,44100,0,0,12000,24000,48000,96000,192000};
    if(bytes<96||blocks<6||!rates[rate]||channels>15)return 0;
    identity=(rate<<8)|channels;
    return packed?((bytes*8+13)/14)*2:bytes;
}
inline bool HasDtsFrames(View b) noexcept {
    for(size_t at=0;at+16<=b.n&&at<8192;++at){
        unsigned first{},second{};const size_t n=DtsFrame(b,at,first);
        if(!n||n>b.n-at)continue;
        const size_t next=DtsFrame(b,at+n,second);
        if(next&&first==second&&next<=b.n-at-n)return true;
    }
    return false;
}
inline bool WaveDts(View b) noexcept {
    if(b.n<12||!(b.is(0,"RIFF")||b.is(0,"RF64")||b.is(0,"BW64"))||!b.is(8,"WAVE"))return false;
    bool pcm=false;
    for(size_t at=12;at+8<=b.n;){
        const size_t n=unsigned(b.le16(at+4))|(unsigned(b.le16(at+6))<<16);
        const size_t payload=at+8;
        if(b.is(at,"fmt ")&&n>=16&&b.n-payload>=16){
            const auto tag=b.le16(payload);
            pcm=(tag==1||tag==0x2001||tag==0xfffe)&&b.le16(payload+2)==2&&b.le16(payload+14)==16;
        }
        if(b.is(at,"data"))return pcm&&HasDtsFrames({b.p+payload,std::min(n,b.n-payload)});
        if(n>b.n-payload)return false;
        at=payload+n+(n&1);
    }
    return false;
}
inline Result Identify(const Request& q,DWORD mask=All) noexcept {
    Result r;
    if(q.size!=sizeof(q)||q.version!=abi_version||q.bytes>prefix_limit||(!q.data&&q.bytes))return r;
    View b{q.data,q.bytes};
    const auto set=[&](DWORD f,const char* c,const char* codec,const wchar_t* ext,const wchar_t* aliases,const wchar_t* name,DWORD confidence=Strong){
        if(!(f&mask))return;
        r.family=f;r.confidence=confidence;r.flags=q.data_offset?HasLeadingTag:0;
        Copy(r.container,c);Copy(r.codec,codec);Copy(r.extension,ext);Copy(r.aliases,aliases);Copy(r.display,name);
    };
    if(b.n<16){r.confidence=NeedMore;r.required_bytes=16;return r;}
    if((b.is(0,"RIFF")||b.is(0,"RF64")||b.is(0,"BW64"))&&b.is(8,"WAVE"))set(Wave,"wave",WaveDts(b)?"dts":"",L".wav",L".wave;.bwf;.dtswav",L"WAVE");
    else if(b.is(0,"DSD ")&&b.le16(4)==28)set(Dsd,"dsf","dsd",L".dsf",L"",L"DSD Stream File");
    else if(b.is(0,"FRM8")&&b.is(12,"DSD "))set(Dsd,"dsdiff","dsd",L".dff",L".dsdiff",L"DSDIFF");
    else if(b.is(0,"RIFF")&&b.is(8,"RMID"))set(Midi,"rmid","midi",L".rmi",L"",L"RIFF MIDI");
    else if(b.is(0,"FORM")&&(b.is(8,"AIFF")||b.is(8,"AIFC")))set(Aiff,"aiff","",L".aiff",L".aif;.aifc",L"AIFF");
    else if(b.is(0,".snd")&&b.be32(4)>=24&&b.be32(4)<=q.file_size)set(Au,"au","",L".au",L".snd",L"Sun / NeXT Audio");
    else if(b.is(0,"fLaC")&&(b.p[4]&127)==0&&b.p[5]==0&&b.p[6]==0&&b.p[7]==34&&q.file_size>=42)set(Flac,"flac","flac",L".flac",L".fla",L"FLAC");
    else if(b.is(0,"TTA1"))set(Tta,"tta","tta",L".tta",L"",L"True Audio");
    else if((b.is(0,"MAC ")||b.is(0,"MACF"))&&b.le16(4)>=3000&&b.le16(4)<=5000)set(Ape,"ape","ape",L".ape",L".mac",L"Monkey's Audio");
    else if(b.is(0,"MPCK")||b.is(0,"MP+"))set(Mpc,"mpc","mpc",L".mpc",L".mp+;.mpp",L"Musepack");
    else if(b.is(0,"tBaK"))set(Tak,"tak","tak",L".tak",L"",L"TAK");
    else if(b.is(0,"CTENFDAM"))set(Ncm,"ncm","",L".ncm",L"",L"NCM");
    else if(b.is(0,".RMF"))set(Real,"rm","",L".rm",L".ra;.ram;.rmvb",L"RealMedia");
    else if(b.is(0,".ra\xfd",4))set(Real,"ra","",L".ra",L"",L"RealAudio");
    else if(b.is(0,"TWIN"))set(Vqf,"vqf","twinvq",L".vqf",L"",L"TwinVQ");
    else if(b.is(4,"ftyp")&&b.be32(0)>=16&&b.be32(0)<=q.file_size)set(Mp4,"mp4","",L".mp4",L".m4a;.m4b;.m4p;.m4v;.3gp;.3g2;.mov",L"MP4 / ISO Base Media");
    else if(b.is(0,"\x30\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\x00\xaa\x00\x62\xce\x6c",16))set(Asf,"asf","",L".asf",L".wma;.wmv",L"ASF / Windows Media");
    else if(b.is(0,"MThd")&&b.be32(4)==6)set(Midi,"midi","midi",L".mid",L".midi;.kar",L"MIDI");
    else if(b.is(0,"ADIF"))set(Aac,"adif","aac",L".aac",L".adif",L"AAC (ADIF)",Tentative);
    else if(b.is(0,"OggS")) {
        for(size_t at=0;at+27<=b.n;) {
            if(!b.is(at,"OggS")||b.p[at+4]!=0)break;
            const size_t segments=b.p[at+26],body=at+27+segments;
            if(body>b.n)break;
            size_t length=0,packet=0;bool ended=false;
            for(size_t i=0;i<segments;++i){length+=b.p[at+27+i];if(!ended){packet+=b.p[at+27+i];ended=b.p[at+27+i]<255;}}
            if(length>b.n-body){r.confidence=NeedMore;r.required_bytes=DWORD(body+length);return r;}
            if((b.p[at+5]&3)==2&&ended) {
                if(packet>=19&&b.is(body,"OpusHead"))set(Ogg,"ogg","opus",L".opus",L".ogg;.oga",L"Ogg Opus");
                else if(packet>=30&&b.is(body,"\1vorbis",7))set(Ogg,"ogg","vorbis",L".ogg",L".oga",L"Ogg Vorbis");
                if(r.family)break;
            }
            at=body+length;
        }
    } else if(b.is(0,"\x1a\x45\xdf\xa3",4)) {
        size_t at=4;
        const auto vint=[&](bool id,uint64_t& value){
            if(at>=b.n||!b.p[at])return false;
            unsigned first=b.p[at++],bit=128,n=1;while(!(first&bit)){bit>>=1;++n;}
            if(n>(id?4u:8u)||n-1>b.n-at)return false;
            value=id?first:first&(bit-1);for(unsigned i=1;i<n;++i)value=(value<<8)|b.p[at++];
            return id||value!=((uint64_t(1)<<(7*n))-1);
        };
        uint64_t n{};
        if(vint(false,n)&&n<=4096&&n<=b.n-at){
            const size_t end=at+size_t(n);bool webm=false,valid=true;
            while(at<end){uint64_t id{},len{};if(!vint(true,id)||!vint(false,len)||at>end||len>end-at){valid=false;break;}
                if(id==0x4282){if(webm||len!=4||!b.is(at,"webm")){valid=false;break;}webm=true;}at+=size_t(len);}
            if(valid&&webm)set(Webm,"webm","",L".webm",L".weba",L"WebM");
        }
    } else {
        unsigned id{},layer{},id2{},layer2{};
        const auto adts=AdtsFrame(b,0);
        const auto next_adts=adts&&adts<=b.n?AdtsFrame(b,adts):0;
        const auto mpeg=MpegFrame(b,0,id,layer);
        const auto next_mpeg=mpeg&&mpeg<=b.n?MpegFrame(b,mpeg,id2,layer2):0;
        if(adts&&next_adts&&next_adts<=b.n-adts&&(b.p[2]&0xfc)==(b.p[adts+2]&0xfc))set(Aac,"adts","aac",L".aac",L".adts",L"AAC (ADTS)");
        else if(mpeg&&next_mpeg&&next_mpeg<=b.n-mpeg&&id==id2)set(Mpeg,"mpeg",layer==1?"mp1":layer==2?"mp2":"mp3",layer==1?L".mp1":layer==2?L".mp2":L".mp3",layer==3?L".mpa;.mpga;.mpeg;.mpg;.mp3pro":L".mpa;.mpga;.mpeg;.mpg",L"MPEG Audio");
        else if(b.is(0,"\x0b\x77",2)&&(b.p[4]>>6)<3&&(b.p[4]&63)<38&&(b.p[5]>>3)<=10)set(Ac3,"ac3","ac3",L".ac3",L".a52",L"Dolby AC3",Tentative);
        else if(b.is(0,"\x7f\xfe\x80\x01",4)||b.is(0,"\xfe\x7f\x01\x80",4)||b.is(0,"\x1f\xff\xe8\x00",4)||b.is(0,"\xff\x1f\x00\xe8",4))set(Dts,"dts","dts",L".dts",L".dtswav",L"DTS",Tentative);
        else if(b.is(0,"Extended Module: ")&&b.n>60&&b.p[37]==0x1a)set(Module,"xm","module",L".xm",L"",L"FastTracker XM");
        else if(b.is(0,"IMPM")&&b.n>=192)set(Module,"it","module",L".it",L"",L"Impulse Tracker");
        else if(b.is(44,"SCRM")&&b.n>=96)set(Module,"s3m","module",L".s3m",L"",L"Scream Tracker 3");
        else if(b.is(0,"MTM")&&b.n>=66)set(Module,"mtm","module",L".mtm",L"",L"MultiTracker");
        else if(b.is(0,"FAR\xfe",4)&&b.n>=98)set(Module,"far","module",L".far",L"",L"Farandole");
        else if((b.is(20,"!Scream!")||b.is(20,"BMOD2STM"))&&b.n>=48)set(Module,"stm","module",L".stm",L"",L"Scream Tracker 2");
        else if(b.is(0,"\xc1\x83\x2a\x9e",4))set(Module,"umx","module",L".umx",L"",L"Unreal Music",Tentative);
        else if(b.n>=1084&&(b.is(1080,"M.K.")||b.is(1080,"M!K!")||b.is(1080,"FLT4")||b.is(1080,"FLT8")||b.is(1080,"4CHN")||b.is(1080,"6CHN")||b.is(1080,"8CHN")||b.is(1080,"CD81")||b.is(1080,"OKTA")))set(Module,"mod","module",L".mod",L"",L"ProTracker MOD");
        else if(mask&Uc) {
            std::array<BYTE,prefix_limit> plain{};for(size_t i=0;i<b.n;++i)plain[i]=b.p[i]^0xa3;
            Request unwrapped=q;unwrapped.data=plain.data();
            auto inner=Identify(unwrapped,Mpeg|Flac);
            if(inner.confidence>=Strong)set(Uc,"uc",inner.codec,L".uc",L".uc!",L"UC audio cache");
        }
    }
    if(r.family&&!r.codec[0])r.flags|=ContainerOnly;
    return r;
}

struct Snapshot { std::array<BYTE,prefix_limit> bytes{};Request request; };
inline HRESULT ReadSnapshot(IStream* stream,Snapshot& output) noexcept {
    output.request=Request{};output.request.data=output.bytes.data();
    if(!stream)return E_POINTER;
    LARGE_INTEGER zero{};ULARGE_INTEGER saved{};
    auto hr=stream->Seek(zero,STREAM_SEEK_CUR,&saved);
    if(hr==E_NOTIMPL||hr==STG_E_INVALIDFUNCTION)return S_FALSE;
    if(FAILED(hr))return hr;
    STATSTG stat{};
    // Unknown-length/live streams keep the host's existing replay-prefix path.
    // Do not wait for a large prefix or consume a stream we cannot restore.
    if(stream->Stat(&stat,STATFLAG_NONAME)!=S_OK||!stat.cbSize.QuadPart)return S_FALSE;
    output.request.file_size=stat.cbSize.QuadPart;
    constexpr ULONG initial_bytes=8192;
    const auto read=[&](){
        output.request.bytes=0;
        ULONG wanted=initial_bytes;
        while(output.request.bytes<wanted){ULONG got{};auto status=stream->Read(output.bytes.data()+output.request.bytes,wanted-output.request.bytes,&got);
            if(got>wanted-output.request.bytes)return STG_E_READFAULT;
            output.request.bytes+=got;if(FAILED(status))return status;if(!got||status==S_FALSE)break;
            if(output.request.bytes==wanted){
                const auto detected=Identify(output.request,Ogg);
                if(detected.confidence==NeedMore&&detected.required_bytes>wanted&&detected.required_bytes<=prefix_limit)
                    wanted=detected.required_bytes;
            }
        }
        return S_OK;
    };
    hr=stream->Seek(zero,STREAM_SEEK_SET,nullptr);
    if(SUCCEEDED(hr))hr=read();
    if(SUCCEEDED(hr)&&output.request.bytes>=10&&!memcmp(output.bytes.data(),"ID3",3)){
        const auto& p=output.bytes;
        if(p[3]>=2&&p[3]<=4&&p[4]!=255&&!((p[6]|p[7]|p[8]|p[9])&128)){
            const uint64_t offset=10ull+(uint64_t(p[6])<<21)+(uint64_t(p[7])<<14)+(uint64_t(p[8])<<7)+p[9]+(p[3]==4&&(p[5]&16)?10:0);
            // A bounded probe must not download an arbitrarily large leading tag.
            if(offset<=1024*1024&&offset<output.request.file_size){LARGE_INTEGER next{};next.QuadPart=LONGLONG(offset);
                hr=stream->Seek(next,STREAM_SEEK_SET,nullptr);if(SUCCEEDED(hr)){output.request.data_offset=offset;hr=read();}}
        }
    }
    LARGE_INTEGER restore{};restore.QuadPart=LONGLONG(saved.QuadPart);
    const auto restored=stream->Seek(restore,STREAM_SEEK_SET,nullptr);
    return FAILED(restored)?restored:hr;
}
class ProbeSupport : public Probe {
protected:
    virtual DWORD ProbeFamilies() const noexcept=0;
public:
    HRESULT STDMETHODCALLTYPE ProbeFormat(const Request* q,Result* out) override {
        if(!q||!out)return E_POINTER;
        if(out->size!=sizeof(Result)||out->version!=abi_version||q->size!=sizeof(Request)||q->version!=abi_version||q->bytes>prefix_limit||(!q->data&&q->bytes))return E_INVALIDARG;
        *out=Identify(*q,ProbeFamilies());return out->confidence==NoMatch?S_FALSE:S_OK;
    }
};
class InfoSupport : public SourceInfo {
    Result format_;bool opened_{};
protected:
    HRESULT PrepareFormatInfo(IStream* stream,DWORD families) noexcept {
        if(opened_)return S_OK;format_=Result{};
        Snapshot snapshot;const auto hr=ReadSnapshot(stream,snapshot);
        if(hr==S_OK)format_=Identify(snapshot.request,families);
        return hr;
    }
    HRESULT CompleteFormatInfo(HRESULT hr) noexcept {
        if(hr==S_OK){opened_=true;if(format_.confidence>=Tentative)format_.confidence=Opened;}return hr;
    }
    void SetSourceCodec(const char* codec) noexcept {
        if(format_.confidence>=Tentative){Copy(format_.codec,codec);format_.flags&=~ContainerOnly;}
    }
public:
    HRESULT STDMETHODCALLTYPE GetFormatInfo(Result* out) override {
        if(!out)return E_POINTER;if(out->size!=sizeof(Result)||out->version!=abi_version)return E_INVALIDARG;
        *out=Result{};if(!opened_)return E_UNEXPECTED;if(format_.confidence!=Opened)return S_FALSE;*out=format_;return S_OK;
    }
};
} // namespace ttp::format
