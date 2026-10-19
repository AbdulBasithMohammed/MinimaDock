#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace minima {
// Quick settings tiles: which ones show, in what order, over up to three pages
// of six. The order is stored as a comma-separated list of keys (REG_SZ
// "QuickTiles" under HKCU\Software\MinimaDock\Settings, see dock_settings.h).
// New tiles go before Count; the order is the Add list's order.
enum class Tile {Wifi,Bluetooth,Mic,Capture,Timer,Updates,
    Project,Airplane,Hotspot,DarkMode,Captions,Keyboard,Cast,EnergySaver,Vpn,
    NightLight,Nearby,Accessibility,ColorFilters,Rotation,DoNotDisturb,Count};
struct TileSpec {
    std::wstring_view key;   // Stored form; never change a key once released.
    std::wstring_view name;  // Shown in the Add list.
    bool split;              // Has a chevron that opens its own page.
};
inline constexpr std::array<TileSpec,static_cast<std::size_t>(Tile::Count)> tile_specs{{
    {L"wifi",L"Wi-Fi",true},
    {L"bluetooth",L"Bluetooth",true},
    {L"mic",L"Microphone",false},
    {L"capture",L"Instant capture",false},
    {L"timer",L"Timer",true},
    {L"updates",L"Windows Update",false},
    {L"project",L"Project",true},
    {L"airplane",L"Airplane mode",false},
    {L"hotspot",L"Mobile hotspot",false},
    {L"dark",L"Dark mode",false},
    {L"captions",L"Live captions",false},
    {L"keyboard",L"Keyboard layout",false},
    {L"cast",L"Cast",false},
    {L"energy",L"Energy saver",false},
    {L"vpn",L"VPN",false},
    {L"nightlight",L"Night light",false},
    {L"nearby",L"Nearby sharing",false},
    {L"accessibility",L"Accessibility",false},
    {L"colorfilters",L"Color filters",false},
    {L"rotation",L"Rotation lock",false},
    {L"dnd",L"Do not disturb",false},
}};
inline const TileSpec& spec(Tile t){return tile_specs[static_cast<std::size_t>(t)];}
inline constexpr std::size_t tiles_per_page=6,max_pages=3,max_tiles=tiles_per_page*max_pages;

struct QuickLayout {
    std::vector<Tile> tiles;
    // The six tiles MinimaDock has always shown, in the same order.
    static QuickLayout defaults(){return {{Tile::Wifi,Tile::Bluetooth,Tile::Mic,Tile::Capture,Tile::Timer,Tile::Updates}};}
    // Unknown keys (from a newer version) and repeats are skipped, and the list
    // stops at max_tiles. An empty string is an empty layout: only a missing
    // value means "default" (the caller decides that).
    static QuickLayout parse(std::wstring_view text) {
        QuickLayout layout;
        while(!text.empty()&&layout.tiles.size()<max_tiles) {
            const auto comma=text.find(L',');
            auto key=text.substr(0,comma);
            while(!key.empty()&&key.front()==L' ')key.remove_prefix(1);
            while(!key.empty()&&key.back()==L' ')key.remove_suffix(1);
            for(std::size_t i=0;i<tile_specs.size();++i)
                if(tile_specs[i].key==key&&!layout.contains(static_cast<Tile>(i)))layout.tiles.push_back(static_cast<Tile>(i));
            if(comma==std::wstring_view::npos)break;
            text.remove_prefix(comma+1);
        }
        return layout;
    }
    std::wstring serialize() const {
        std::wstring out;
        for(const auto t:tiles){if(!out.empty())out+=L',';out+=spec(t).key;}
        return out;
    }
    bool contains(Tile t) const {return std::find(tiles.begin(),tiles.end(),t)!=tiles.end();}
    bool full() const {return tiles.size()>=max_tiles;}
    // Always at least one page, so an empty layout still has somewhere to show "+ Add".
    std::size_t pages() const {return std::max<std::size_t>(1,(tiles.size()+tiles_per_page-1)/tiles_per_page);}
    // The tile in a page's slot, or Tile::Count for an empty slot.
    Tile at(std::size_t page,std::size_t slot) const {const auto i=page*tiles_per_page+slot;return i<tiles.size()?tiles[i]:Tile::Count;}
    // Tiles not shown, in catalog order: the Add list.
    std::vector<Tile> hidden() const {
        std::vector<Tile> out;
        for(std::size_t i=0;i<tile_specs.size();++i)if(!contains(static_cast<Tile>(i)))out.push_back(static_cast<Tile>(i));
        return out;
    }
    bool add(Tile t) {if(t==Tile::Count||full()||contains(t))return false;tiles.push_back(t);return true;}
    void remove(std::size_t i) {if(i<tiles.size())tiles.erase(tiles.begin()+static_cast<std::ptrdiff_t>(i));}
    // Moves one tile; the others shift to fill the gap. `to` is clamped to the end.
    void move(std::size_t from,std::size_t to) {
        if(from>=tiles.size())return;
        to=std::min(to,tiles.size()-1);
        const auto t=tiles[from];tiles.erase(tiles.begin()+static_cast<std::ptrdiff_t>(from));
        tiles.insert(tiles.begin()+static_cast<std::ptrdiff_t>(to),t);
    }
    bool operator==(const QuickLayout&) const=default;
};
}
