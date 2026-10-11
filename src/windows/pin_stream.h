#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
namespace minima {
struct PinItem {std::size_t offset,length;};
// Read-only compatibility adapter for Explorer's observed version-3 Favorites stream.
// Validate the entire stream and every SHITEMID before passing a PIDL to Shell.
inline std::vector<PinItem> parse_pin_stream(std::span<const std::uint8_t> bytes) {
    std::vector<PinItem> items;
    if(bytes.empty()||bytes.size()>1024*1024)return {};
    std::size_t position=0;
    while(position<bytes.size()) {
        if(position+1==bytes.size())return bytes[position]==255?items:std::vector<PinItem>{};
        if(bytes[position]!=0||bytes.size()-position<5||items.size()>=128)return {};
        const auto length=static_cast<std::uint32_t>(bytes[position+1])|
            (static_cast<std::uint32_t>(bytes[position+2])<<8)|
            (static_cast<std::uint32_t>(bytes[position+3])<<16)|
            (static_cast<std::uint32_t>(bytes[position+4])<<24);
        position+=5;
        if(length<4||length>bytes.size()-position)return {};
        std::size_t part=0;
        while(part+2<=length) {
            const auto size=static_cast<unsigned>(bytes[position+part])|(static_cast<unsigned>(bytes[position+part+1])<<8);
            if(size==0)break;
            if(size<2||size>length-part)return {};
            part+=size;
        }
        if(part+2!=length||bytes[position+part]!=0||bytes[position+part+1]!=0)return {};
        items.push_back({position,length});position+=length;
    }
    return {};
}
}
