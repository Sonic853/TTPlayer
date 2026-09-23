#include "ttplayer/audio/extension_correction.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace ttplayer::audio {
namespace {
struct File {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit File(const std::filesystem::path& path) : value(CreateFileW(path.c_str(),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::optional<MediaFileStamp> Stamp(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return {};
    return MediaFileStamp{
        (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
        (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        info.dwVolumeSerialNumber};
}
using Bytes = std::span<const unsigned char>;
bool Is(Bytes bytes, size_t offset, std::string_view text) {
    return offset <= bytes.size() && text.size() <= bytes.size() - offset &&
        std::memcmp(bytes.data() + offset, text.data(), text.size()) == 0;
}
unsigned Be32(Bytes b, size_t p) {
    return (unsigned(b[p]) << 24) | (unsigned(b[p+1]) << 16) |
        (unsigned(b[p+2]) << 8) | b[p+3];
}
std::optional<std::pair<std::wstring, std::wstring>> Detect(Bytes b,
    std::wstring_view extension, std::uint64_t size) {
    const auto choose = [&](const wchar_t* suffix, const wchar_t* name,
                            std::initializer_list<const wchar_t*> aliases)
        -> std::optional<std::pair<std::wstring, std::wstring>> {
        if (extension == suffix) return {};
        for (auto alias : aliases) if (extension == alias) return {};
        return std::pair<std::wstring, std::wstring>{suffix, name};
    };
    if (b.size() < 16) return {};
    // Prefer the outer container: AAC in MP4 is not a raw .aac file, and
    // MPEG compressed audio in RIFF is still a .wav file.
    if ((Is(b,0,"RIFF") || Is(b,0,"RF64") || Is(b,0,"BW64")) && Is(b,8,"WAVE"))
        return choose(L".wav",L"WAVE",{L".wave",L".bwf",L".dtswav"});
    if (Is(b,0,"RIFF") && Is(b,8,"RMID")) return choose(L".rmi",L"RIFF MIDI",{});
    if (Is(b,0,".snd") && Be32(b,4)>=24 && Be32(b,4)<=size)
        return choose(L".au",L"Sun / NeXT Audio",{L".snd"});
    if (Is(b,0,"FORM") && (Is(b,8,"AIFF") || Is(b,8,"AIFC")))
        return choose(L".aiff",L"AIFF",{L".aif",L".aifc"});
    if (Is(b,0,"fLaC") && (b[4]&0x7f)==0 && b[5]==0 && b[6]==0 && b[7]==34 && size>=42)
        return choose(L".flac",L"FLAC",{L".fla"});
    if (Is(b,0,"MAC ") && b.size()>=32)
        return choose(L".ape",L"Monkey's Audio",{L".mac"});
    if (Is(b,0,"TTA1")) return choose(L".tta",L"True Audio",{});
    if (Is(b,0,"wvpk") && b.size()>=32) return choose(L".wv",L"WavPack",{});
    if (Is(b,0,"MPCK") || Is(b,0,"MP+"))
        return choose(L".mpc",L"Musepack",{L".mp+",L".mpp"});
    if (Is(b,0,"MThd") && Be32(b,4)==6)
        return choose(L".mid",L"MIDI",{L".midi",L".kar"});
    if (Is(b,0,"#!AMR\n")) return choose(L".amr",L"AMR",{});
    if (Is(b,0,"#!AMR-WB\n")) return choose(L".awb",L"AMR-WB",{L".amr"});
    if (Is(b,4,"ftyp") && Be32(b,0)>=16 && Be32(b,0)<=size)
        return choose(L".mp4",L"MP4 / ISO Base Media",{L".m4a",L".m4b",L".m4p",L".m4v",L".3gp",L".3g2",L".mov"});
    static constexpr unsigned char asf[]{0x30,0x26,0xb2,0x75,0x8e,0x66,0xcf,0x11,0xa6,0xd9,0,0xaa,0,0x62,0xce,0x6c};
    if (std::memcmp(b.data(),asf,sizeof(asf))==0)
        return choose(L".asf",L"ASF / Windows Media",{L".wma",L".wmv"});
    if (Is(b,0,".RMF")) return choose(L".rm",L"RealMedia",{L".ra",L".ram",L".rmvb"});
    if (Is(b,0,"OggS") && b[4]==0 && b.size()>=28) {
        const size_t packet=27U+b[26];
        if (Is(b,packet,std::string_view("\x01vorbis",7)))
            return choose(L".ogg",L"Ogg Vorbis",{L".oga"});
        if (Is(b,packet,"OpusHead")) return choose(L".opus",L"Ogg Opus",{L".ogg",L".oga"});
        if (Is(b,packet,std::string_view("\x7f" "FLAC",5)))
            return choose(L".oga",L"Ogg FLAC",{L".ogg"});
        return {};
    }
    // Two complete, consistent ADTS frames. Do not mistake ADTS sync for MPEG.
    const auto adts = [&](size_t p) -> size_t {
        if (p+7>b.size() || b[p]!=0xff || (b[p+1]&0xf6)!=0xf0 ||
            ((b[p+2]>>2)&15)>=13) return 0;
        const size_t n=((b[p+3]&3U)<<11)|(unsigned(b[p+4])<<3)|(b[p+5]>>5);
        return n >= ((b[p+1]&1)?7U:9U) && n<=b.size()-p ? n : 0;
    };
    if (const auto n=adts(0); n && adts(n) && (b[2]&0xfc)==(b[n+2]&0xfc))
        return choose(L".aac",L"AAC (ADTS)",{L".adts"});
    const auto mpeg = [&](size_t p, unsigned& identity, unsigned& layer) -> size_t {
        if (p+4>b.size() || b[p]!=0xff || (b[p+1]&0xe0)!=0xe0) return 0;
        const unsigned version=(b[p+1]>>3)&3;
        layer=4-((b[p+1]>>1)&3);
        const unsigned rate=(b[p+2]>>2)&3, bit=b[p+2]>>4;
        if (version==1 || layer==4 || rate==3 || bit==0 || bit==15) return 0;
        static constexpr unsigned rates[]{44100,48000,32000};
        static constexpr unsigned high[3][14]{
            {32,64,96,128,160,192,224,256,288,320,352,384,416,448},
            {32,48,56,64,80,96,112,128,160,192,224,256,320,384},
            {32,40,48,56,64,80,96,112,128,160,192,224,256,320}};
        static constexpr unsigned low[2][14]{
            {32,48,56,64,80,96,112,128,144,160,176,192,224,256},
            {8,16,24,32,40,48,56,64,80,96,112,128,144,160}};
        const unsigned kbps=version==3?high[layer-1][bit-1]:low[layer==1?0:1][bit-1];
        const unsigned hz=rates[rate]/(version==3?1:version==2?2:4);
        const unsigned pad=(b[p+2]>>1)&1;
        const size_t n=layer==1?(12000*kbps/hz+pad)*4:
            ((layer==3 && version!=3)?72000:144000)*kbps/hz+pad;
        identity=(version<<8)|(layer<<4)|rate;
        return n>=4 && n<=b.size()-p?n:0;
    };
    unsigned id{},layer{},next_id{},next_layer{};
    if (const auto n=mpeg(0,id,layer); n && mpeg(n,next_id,next_layer) && id==next_id) {
        if (layer==3 && extension==L".mp3pro") return {};
        const auto suffix=layer==1?L".mp1":layer==2?L".mp2":L".mp3";
        return choose(suffix,layer==1?L"MPEG Audio Layer I":layer==2?L"MPEG Audio Layer II":L"MPEG Audio Layer III",
            {L".mpa",L".mpga",L".mpeg",L".mpg"});
    }
    return {};
}
}

std::optional<MediaFileStamp> ReadMediaFileStamp(const std::filesystem::path& path) {
    File file(path);
    return Stamp(file.value);
}

std::optional<ExtensionCorrection> FindExtensionCorrection(const std::filesystem::path& path, int subtrack) {
    const auto value=path.native();
    if (subtrack!=0 || value.find(L"://")!=std::wstring::npos ||
        value.find(L'|')!=std::wstring::npos || path.empty()) return {};
    // Do not rename links or CUE/CD tracks to their underlying stream type.
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    File file(path);
    const auto before=Stamp(file.value);
    if (!before) return {};
    std::array<unsigned char,65536> bytes{};
    DWORD count{};
    if (!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)) return {};
    if (count>=10 && Is(Bytes(bytes.data(),count),0,"ID3")) {
        if (bytes[3]<2 || bytes[3]>4 || ((bytes[6]|bytes[7]|bytes[8]|bytes[9])&0x80)) return {};
        const std::uint64_t offset=10ULL+(std::uint64_t(bytes[6])<<21)+
            (std::uint64_t(bytes[7])<<14)+(std::uint64_t(bytes[8])<<7)+bytes[9]+
            (bytes[3]==4 && (bytes[5]&0x10)?10:0);
        if (offset>=before->size) return {};
        LARGE_INTEGER seek{}; seek.QuadPart=static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file.value,seek,nullptr,FILE_BEGIN) ||
            !ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)) return {};
    }
    auto extension=path.extension().wstring();
    std::transform(extension.begin(),extension.end(),extension.begin(),towlower);
    const auto format=Detect(Bytes(bytes.data(),count),extension,before->size);
    if (!format || Stamp(file.value)!=before) return {};
    auto target=path; target.replace_extension(format->first);
    return ExtensionCorrection{path,target,format->second,*before};
}

ExtensionRenameResult CorrectFileExtension(const ExtensionCorrection& c,
    ExtensionCollision collision, const std::optional<MediaFileStamp>& destination) {
    if (ReadMediaFileStamp(c.source)!=c.stamp) return {{},ERROR_FILE_INVALID};
    if (_wcsicmp(c.source.c_str(),c.target.c_str())==0) return {{},ERROR_INVALID_NAME};
    const auto existing=ReadMediaFileStamp(c.target);
    if (existing && existing->file_id==c.stamp.file_id && existing->volume==c.stamp.volume)
        return {{},ERROR_ALREADY_EXISTS};
    if (collision==ExtensionCollision::overwrite) {
        if (!destination || existing!=destination) return {{},ERROR_FILE_INVALID};
        if (!MoveFileExW(c.source.c_str(),c.target.c_str(),MOVEFILE_REPLACE_EXISTING)) return {{},GetLastError()};
        return {c.target,0};
    }
    auto target=c.target;
    for (unsigned index=collision==ExtensionCollision::number?1U:0U; index<100000; ++index) {
        if (index) target=c.target.parent_path()/(c.target.stem().wstring()+L" ("+
            std::to_wstring(index)+L")"+c.target.extension().wstring());
        if (MoveFileW(c.source.c_str(),target.c_str())) return {target,0};
        const DWORD error=GetLastError();
        if (collision!=ExtensionCollision::number ||
            (error!=ERROR_ALREADY_EXISTS && error!=ERROR_FILE_EXISTS)) return {{},error};
    }
    return {{},ERROR_TOO_MANY_NAMES};
}

std::wstring MediaFileSizeText(std::uint64_t bytes) {
    const wchar_t* units[]{L"B",L"KiB",L"MiB",L"GiB",L"TiB"};
    double value=static_cast<double>(bytes); unsigned unit{};
    while (value>=1024 && unit<4) { value/=1024; ++unit; }
    wchar_t text[96]{};
    swprintf_s(text,L"%.2f %s (%llu bytes)",value,units[unit],static_cast<unsigned long long>(bytes));
    return text;
}
} // namespace ttplayer::audio
