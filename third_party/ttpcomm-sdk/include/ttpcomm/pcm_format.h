#pragma once
#include <windows.h>
#include <mmreg.h>
#include <objbase.h>
// Header-only ABI helper. Plugins vendor this SDK header for independent builds.
namespace ttp::pcm {
// FLAC and reordered Vorbis/family-1 Opus use these defined speaker layouts.
inline DWORD standard_mask(unsigned channels) noexcept {
    constexpr DWORD masks[]{0,4,3,7,0x33,0x37,0x3f,0x70f,0x63f};
    return channels<9?masks[channels]:0;
}
inline HRESULT copy_format(const WAVEFORMATEX& source,DWORD mask,WORD valid_bits,WAVEFORMATEX** out) noexcept {
    if(!out)return E_POINTER;*out=nullptr;
    if(source.wFormatTag!=WAVE_FORMAT_PCM&&source.wFormatTag!=WAVE_FORMAT_IEEE_FLOAT)return E_INVALIDARG;
    if(!valid_bits)valid_bits=source.wBitsPerSample;
    if(!valid_bits||valid_bits>source.wBitsPerSample)return E_INVALIDARG;
    if(!source.nChannels)return E_INVALIDARG;
    if(source.wFormatTag==WAVE_FORMAT_IEEE_FLOAT&&
       ((source.wBitsPerSample!=32&&source.wBitsPerSample!=64)||valid_bits!=source.wBitsPerSample))return E_INVALIDARG;
    unsigned count=0;for(DWORD bits=mask;bits;bits&=bits-1)++count;
    if(mask&&count!=source.nChannels)return E_INVALIDARG;
    const bool extended=source.nChannels>2||valid_bits!=source.wBitsPerSample||
        (mask&&mask!=standard_mask(source.nChannels));
    const auto size=extended?sizeof(WAVEFORMATEXTENSIBLE):sizeof(WAVEFORMATEX);
    auto* p=static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(size));if(!p)return E_OUTOFMEMORY;
    ZeroMemory(p,size);*p=source;p->cbSize=0;
    if(extended){auto* e=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(p);e->Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;e->Format.cbSize=22;e->Samples.wValidBitsPerSample=valid_bits;e->dwChannelMask=mask;e->SubFormat={source.wFormatTag,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};}
    *out=p;return S_OK;
}
}
