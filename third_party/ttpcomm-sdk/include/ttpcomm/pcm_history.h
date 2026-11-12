#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
namespace ttp::pcm {
// Exact recent PCM for codecs whose predictor state cannot be reconstructed
// from a byte offset alone. Bounded per reader; allocation failure is optional.
class History {
    struct Block {uint64_t start;std::vector<BYTE> bytes;};
    std::deque<Block> blocks_;size_t bytes_{};uint64_t end_{};
    static constexpr size_t limit=4*1024*1024;
public:
    void clear() noexcept {blocks_.clear();bytes_=0;end_=0;}
    bool contains(uint64_t sample)const noexcept{return !blocks_.empty()&&sample>=blocks_.front().start&&sample<=end_;}
    void remember(uint64_t sample,const BYTE* data,size_t size,unsigned align) noexcept {
        if(!align||!size||size>limit)return;
        try {
            if(!blocks_.empty()&&sample!=end_)clear();
            blocks_.push_back({sample,std::vector<BYTE>(data,data+size)});bytes_+=size;end_=sample+size/align;
            while(bytes_>limit){bytes_-=blocks_.front().bytes.size();blocks_.pop_front();}
        }catch(...){clear();}
    }
    DWORD read(uint64_t sample,BYTE* output,DWORD capacity,unsigned align)const noexcept {
        DWORD done{};
        for(const auto& b:blocks_){
            if(sample<b.start)break;
            const auto offset=sample-b.start;if(offset>=b.bytes.size()/align)continue;
            const auto bytes=size_t(offset)*align;
            const auto n=std::min<size_t>(capacity-done,b.bytes.size()-bytes);
            memcpy(output+done,b.bytes.data()+bytes,n);done+=DWORD(n);sample+=n/align;
            if(done==capacity)break;
        }
        return done;
    }
};
}
