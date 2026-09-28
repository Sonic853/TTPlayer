#pragma once

// Inspect only this loaded PE32 image. Disk ILT data is needed when the Windows
// loader has overwritten a legal OFT==0 table; packed images also lose names.
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ttplayer::audio::detail::dsp_imports {
struct Import {
    ULONG_PTR* slot{};
    ULONG_PTR original{};
    std::string library, name;
    bool recovered{};
};
struct Image {
    std::vector<BYTE> bytes;
    IMAGE_NT_HEADERS32 nt{};
    std::vector<IMAGE_SECTION_HEADER> sections;
    template<class T> bool Get(size_t offset,T& value) const {
        if(offset>bytes.size()||sizeof(T)>bytes.size()-offset)return false;
        std::memcpy(&value,bytes.data()+offset,sizeof(T));return true;
    }
    std::string Text(DWORD offset) const {
        if(offset>=bytes.size())return {};
        size_t end=offset;
        while(end<bytes.size()&&end-offset<1024&&bytes[end])++end;
        if(end==bytes.size()||end-offset==1024)return {};
        return {reinterpret_cast<const char*>(bytes.data()+offset),end-offset};
    }
    void Headers() {
        IMAGE_DOS_HEADER dos{};
        if(!Get(0,dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||
           !Get(static_cast<size_t>(dos.e_lfanew),nt)||nt.Signature!=IMAGE_NT_SIGNATURE||
           nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||
           nt.FileHeader.NumberOfSections>96||nt.OptionalHeader.SizeOfImage>256*1024*1024||
           nt.OptionalHeader.SizeOfImage<4096)
            throw std::runtime_error("Invalid x86 DSP image headers");
        const size_t table=static_cast<size_t>(dos.e_lfanew)+24+nt.FileHeader.SizeOfOptionalHeader;
        for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i){
            IMAGE_SECTION_HEADER section{};
            if(!Get(table+i*sizeof(section),section))throw std::runtime_error("Truncated DSP section table");
            sections.push_back(section);
        }
    }
};
inline Image Memory(HMODULE module) {
    Image image;image.bytes.resize(4096);SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),module,image.bytes.data(),image.bytes.size(),&copied))
        throw std::runtime_error("Cannot read DSP image");
    image.Headers();image.bytes.resize(image.nt.OptionalHeader.SizeOfImage);
    for(size_t offset=0;offset<image.bytes.size();){
        MEMORY_BASIC_INFORMATION region{};
        auto* address=reinterpret_cast<BYTE*>(module)+offset;
        if(!VirtualQuery(address,&region,sizeof(region)))break;
        const size_t available=static_cast<BYTE*>(region.BaseAddress)+region.RegionSize-address;
        const size_t amount=(std::min)(available,image.bytes.size()-offset);
        if(!amount)break;
        if(region.AllocationBase==module&&region.State==MEM_COMMIT&&!(region.Protect&(PAGE_NOACCESS|PAGE_GUARD)))
            ReadProcessMemory(GetCurrentProcess(),address,image.bytes.data()+offset,amount,&copied);
        offset+=amount;
    }
    return image;
}
inline Image Disk(const std::filesystem::path& path) {
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(error||size>256*1024*1024)throw std::runtime_error("Cannot read DSP import metadata");
    Image raw;raw.bytes.resize(static_cast<size_t>(size));
    std::ifstream file(path,std::ios::binary);
    if(!file.read(reinterpret_cast<char*>(raw.bytes.data()),raw.bytes.size()))throw std::runtime_error("Cannot read DSP import metadata");
    raw.Headers();Image mapped;mapped.nt=raw.nt;mapped.sections=raw.sections;mapped.bytes.resize(raw.nt.OptionalHeader.SizeOfImage);
    const size_t headers=(std::min)({static_cast<size_t>(raw.nt.OptionalHeader.SizeOfHeaders),raw.bytes.size(),mapped.bytes.size()});
    std::copy_n(raw.bytes.begin(),headers,mapped.bytes.begin());
    for(const auto& section:raw.sections){
        if(section.PointerToRawData>raw.bytes.size()||section.VirtualAddress>mapped.bytes.size())continue;
        const size_t amount=(std::min)({static_cast<size_t>(section.SizeOfRawData),raw.bytes.size()-static_cast<size_t>(section.PointerToRawData),mapped.bytes.size()-static_cast<size_t>(section.VirtualAddress)});
        std::copy_n(raw.bytes.begin()+section.PointerToRawData,amount,mapped.bytes.begin()+section.VirtualAddress);
    }
    return mapped;
}
struct Result {
    size_t image_size{};
    std::vector<Import> imports;
    std::set<std::string> libraries;
};
inline Result Inspect(HMODULE module,const std::filesystem::path& path,
                      const std::map<ULONG_PTR,std::string>& addresses) {
    const auto memory=Memory(module);const auto disk=Disk(path);
    if(memory.nt.OptionalHeader.SizeOfImage!=disk.nt.OptionalHeader.SizeOfImage)
        throw std::runtime_error("DSP file changed after loading");
    Result result;result.image_size=memory.bytes.size();std::set<DWORD> slots;
    const auto add=[&](DWORD rva,std::string library,std::string name,bool recovered){
        DWORD value{};if(!memory.Get(rva,value)||!value||!slots.insert(rva).second)return;
        result.imports.push_back({reinterpret_cast<ULONG_PTR*>(reinterpret_cast<BYTE*>(module)+rva),value,std::move(library),std::move(name),recovered});
    };
    // Prefer intact runtime metadata, then the matching on-disk ILT/FT.
    for(const auto* source:{&memory,&disk}){
        const auto directory=source->nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if(!directory.VirtualAddress)continue;
        for(size_t i=0;i<4096;++i){
            IMAGE_IMPORT_DESCRIPTOR descriptor{};
            if(!source->Get(size_t(directory.VirtualAddress)+i*sizeof(descriptor),descriptor))break;
            if(!descriptor.Name)break;
            const auto library=source->Text(descriptor.Name);if(library.empty()||!descriptor.FirstThunk)continue;
            result.libraries.insert(library);
            const HMODULE imported=GetModuleHandleA(library.c_str());
            const DWORD lookup=descriptor.OriginalFirstThunk?descriptor.OriginalFirstThunk:descriptor.FirstThunk;
            for(size_t index=0;index<65536;++index){
                DWORD token{},actual{};const size_t target=size_t(descriptor.FirstThunk)+index*4;
                if(!source->Get(size_t(lookup)+index*4,token)||!memory.Get(target,actual)||!token||!actual)break;
                std::string name;
                if(!(token&IMAGE_ORDINAL_FLAG32)&&token<source->bytes.size()-2)name=source->Text(token+2);
                const auto known=addresses.find(actual);
                if(name.empty()&&known!=addresses.end())name=known->second;
                if(name.empty())continue;
                // Never trust a disk name whose live slot points elsewhere.
                const auto expected=imported?GetProcAddress(imported,name.c_str()):nullptr;
                if(reinterpret_cast<ULONG_PTR>(expected)!=actual&&known==addresses.end())continue;
                add(static_cast<DWORD>(target),library,name,source==&disk||descriptor.OriginalFirstThunk==0);
            }
        }
    }
    // PECompact/UPX can discard the business descriptor/name table. Recover
    // only exact system export pointers referenced by x86 indirect call/jump
    // or absolute load instructions in executable image sections. This does
    // not replace arbitrary pointer-looking words in sample buffers/BSS.
    const auto base=reinterpret_cast<ULONG_PTR>(module);
    for(const auto& section:memory.sections){
        if(!(section.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        const size_t begin=section.VirtualAddress;
        if(begin>=memory.bytes.size())continue;
        const size_t end=begin+(std::min)(static_cast<size_t>((std::max)(section.Misc.VirtualSize,section.SizeOfRawData)),memory.bytes.size()-begin);
        for(size_t at=begin;at+6<=end;++at){
            const BYTE opcode=memory.bytes[at],operand=memory.bytes[at+1];size_t immediate{};
            if((opcode==0xff&&(operand==0x15||operand==0x25))||
               (opcode==0x8b&&(operand&0xc7)==0x05))immediate=at+2;
            else if(opcode==0xa1)immediate=at+1;
            else continue;
            DWORD absolute{};memory.Get(immediate,absolute);
            if(absolute<base||absolute-base>=memory.bytes.size()||absolute%4)continue;
            const auto rva=static_cast<DWORD>(absolute-base);DWORD value{};
            if(!memory.Get(rva,value))continue;
            const auto known=addresses.find(value);if(known==addresses.end())continue;
            add(rva,"<resolved>",known->second,true);
        }
    }
    return result;
}
} // namespace ttplayer::audio::detail::dsp_imports
