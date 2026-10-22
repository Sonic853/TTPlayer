#pragma once
#include <windows.h>
#include <objidl.h>
#include <stdint.h>

// Optional in-process x86 ABI. Existing AddIn/Creator/Reader slots never change.
// No registry, CRT allocation, STL object or C++ exception crosses this ABI.
namespace ttp::format {
inline constexpr GUID iid_probe{0x7e892943,0x481a,0x43db,{0xa8,0x9b,0x59,0x93,0xf5,0x0b,0x60,0x49}};
inline constexpr GUID iid_info{0xb58ee1a2,0x812c,0x4920,{0x92,0x0e,0x5b,0xe9,0x37,0x9f,0x31,0x32}};
inline constexpr DWORD abi_version=1;
inline constexpr DWORD prefix_limit=65536;
enum Family : DWORD {
    None=0, Mpeg=1u<<0, Aac=1u<<1, Mp4=1u<<2, Flac=1u<<3,
    Tta=1u<<4, Ape=1u<<5, Ogg=1u<<6, Webm=1u<<7, Asf=1u<<8,
    Real=1u<<9, Vqf=1u<<10, Tak=1u<<11, Ncm=1u<<12, Uc=1u<<13,
    Ac3=1u<<14, Dts=1u<<15, Module=1u<<16, Wave=1u<<17,
    Aiff=1u<<18, Au=1u<<19, Midi=1u<<20,
    Mpc=1u<<21, Dsd=1u<<22, Builtin=Mpeg|Wave|Aiff|Au|Midi, All=0x7fffffu
};
enum Confidence : DWORD { NoMatch=0, NeedMore=1, Tentative=2, Strong=3, Opened=4 };
enum ResultFlags : DWORD { HasLeadingTag=1, ContainerOnly=2 };
#pragma pack(push, 8)
struct Request {
    DWORD size{sizeof(Request)}, version{abi_version};
    const BYTE* data{};
    DWORD bytes{};
    uint64_t file_size{UINT64_MAX}, data_offset{};
};
struct Result {
    DWORD size{sizeof(Result)}, version{abi_version};
    DWORD family{}, confidence{}, flags{}, required_bytes{};
    char container[24]{}, codec[24]{};
    wchar_t extension[16]{}, aliases[160]{}, display[80]{};
};
#pragma pack(pop)
struct Probe : IUnknown {
    // S_OK: inspect result.confidence. S_FALSE: no match. Never perform I/O.
    virtual HRESULT STDMETHODCALLTYPE ProbeFormat(const Request*, Result*) = 0;
};
struct SourceInfo : IUnknown {
    // E_UNEXPECTED until Open succeeds; S_FALSE if format remains ambiguous.
    virtual HRESULT STDMETHODCALLTYPE GetFormatInfo(Result*) = 0;
};
static_assert(sizeof(Result)==584, "format result ABI");
} // namespace ttp::format
