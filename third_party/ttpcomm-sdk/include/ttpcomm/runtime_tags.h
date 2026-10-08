#pragma once
#include <ttpcomm/tags.h>
#include <ttpcomm/runtime_client.h>
#include <stdexcept>

namespace ttpcomm::host::tags {
using ttpcomm::tags::View;
using ttpcomm::tags::Picture;
using ttpcomm::tags::Frame;
using ttpcomm::tags::FrameStatus;
using ttpcomm::tags::TerminatorSize;
inline size_t FindTerminator(View data,unsigned encoding) noexcept {
    const auto api=Runtime();
    return api && data.size()<=UINT32_MAX ? api->tag_terminator(data.data(),static_cast<uint32_t>(data.size()),encoding) : data.size();
}
inline std::vector<unsigned char> RemoveUnsynchronization(View data) {
    const auto api=Runtime();
    if(!api || data.size()>UINT32_MAX) throw std::runtime_error("The rebuilt ttpcomm.dll tag runtime is unavailable");
    std::vector<unsigned char> output(data.size());uint32_t produced{};
    if(api->tag_unsynchronize(data.data(),static_cast<uint32_t>(data.size()),output.data(),static_cast<uint32_t>(output.size()),&produced)!=TTPCOMM_OK)
        throw std::runtime_error("Invalid unsynchronized tag");
    output.resize(produced);return output;
}
inline std::optional<size_t> FrameStart(View tag,unsigned major,unsigned flags) noexcept {
    const auto api=Runtime();uint32_t offset{};
    if(!api || tag.size()>UINT32_MAX || api->tag_frame_start(tag.data(),static_cast<uint32_t>(tag.size()),major,flags,&offset)!=TTPCOMM_OK) return {};
    return offset;
}
inline FrameStatus NextFrame(View tag,unsigned major,size_t& offset,Frame& frame) noexcept {
    const auto api=Runtime();TtpCommTagFrame result{};
    if(!api || tag.size()>UINT32_MAX || offset>tag.size()) return FrameStatus::invalid;
    auto next=static_cast<uint32_t>(offset);
    const int status=api->tag_next_frame(tag.data(),static_cast<uint32_t>(tag.size()),major,&next,&result);
    if(status==TTPCOMM_END) return FrameStatus::end;
    if(status!=TTPCOMM_OK) return FrameStatus::invalid;
    frame={{reinterpret_cast<const char*>(tag.data()+result.offset),major==2?3u:4u},
        View(tag.data()+result.offset,result.raw_size),View(tag.data()+result.payload_offset,result.payload_size),result.flags};
    offset=next;return FrameStatus::frame;
}
inline std::optional<Picture> ReadPicture(View data,unsigned major) noexcept {
    const auto api=Runtime();TtpCommPicture result{};
    if(!api || data.size()>UINT32_MAX || api->tag_picture(data.data(),static_cast<uint32_t>(data.size()),major,&result)!=TTPCOMM_OK) return {};
    const char* names[]{"","image/jpeg","image/png","image/bmp","image/gif"};
    const std::string_view mime=result.legacy_mime && result.legacy_mime<5 ? std::string_view(names[result.legacy_mime]) :
        std::string_view(reinterpret_cast<const char*>(data.data()+result.mime_offset),result.mime_size);
    return Picture{result.type,result.width,result.height,result.depth,mime,View(data.data()+result.data_offset,result.data_size)};
}
inline std::optional<Picture> Id3Picture(View data,unsigned major) noexcept {
    if(major<2 || major>4) return {};
    return ReadPicture(data,major);
}
inline std::optional<Picture> FlacPicture(View data) noexcept {return ReadPicture(data,0);}
inline std::optional<View> DecodePayload(View data,unsigned char major,unsigned char flags,bool unsynchronized,
    std::vector<unsigned char>& storage,std::uint64_t& budget) {
    const auto api=Runtime();TtpCommDecoded result{};
    if(!api || data.size()>UINT32_MAX || api->tag_decode(data.data(),static_cast<uint32_t>(data.size()),major,flags,
        unsynchronized?1:0,&budget,&result)!=TTPCOMM_OK) return {};
    struct Guard {const TtpCommRuntimeApi* api;void* owner;~Guard(){api->tag_decoded_destroy(owner);}} guard{api,result.owner};
    const auto* bytes=static_cast<const unsigned char*>(result.data);
    if(!result.owner) return View(bytes,result.size);
    if(result.size) storage.assign(bytes,bytes+result.size);else storage.clear();
    return View(storage.data(),storage.size());
}
}
