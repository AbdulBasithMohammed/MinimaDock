#include "taskbar/quick_tiles.h"
#include <iostream>
#include <stdexcept>
using namespace minima;
namespace {
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int main() {
    try {
        // Round trip of the default layout.
        const auto standard=QuickLayout::defaults();
        expect(standard.serialize()==L"wifi,bluetooth,mic,capture,timer,updates","default serializes to its keys");
        expect(QuickLayout::parse(standard.serialize())==standard,"default round-trips");
        expect(standard.pages()==1&&standard.hidden().size()==tile_specs.size()-6,"default is one full page");
        // Empty means empty (only a missing value means default).
        const auto none=QuickLayout::parse(L"");
        expect(none.tiles.empty()&&none.pages()==1&&none.hidden().size()==tile_specs.size(),"empty layout keeps one page");
        expect(none.at(0,0)==Tile::Count,"empty slot");
        // Unknown keys, repeats, spaces, wrong case and stray commas.
        const auto messy=QuickLayout::parse(L" timer ,future-tile,,timer,wifi,WIFI,");
        expect((messy.tiles==std::vector<Tile>{Tile::Timer,Tile::Wifi}),"unknown and repeated keys are dropped");
        // A long value never grows past the cap.
        std::wstring many;for(int i=0;i<40;++i)many+=L"x"+std::to_wstring(i)+L",mic,";
        expect(QuickLayout::parse(many).tiles.size()==1,"repeats never grow the layout");
        std::wstring all;for(int round=0;round<5;++round)for(const auto& s:tile_specs){all+=s.key;all+=L',';}
        expect(QuickLayout::parse(all).tiles.size()==std::min(tile_specs.size(),max_tiles),"parse stops at the cap");
        // Add, remove, hidden.
        auto layout=QuickLayout::parse(L"wifi,mic");
        expect(!layout.add(Tile::Wifi),"no duplicates");
        expect(layout.add(Tile::Updates)&&layout.tiles.back()==Tile::Updates,"add appends");
        expect(!layout.add(Tile::Count),"Count is not a tile");
        const auto hidden=layout.hidden();
        expect(hidden.size()==tile_specs.size()-3&&hidden[0]==Tile::Bluetooth&&hidden[1]==Tile::Capture&&hidden[2]==Tile::Timer&&hidden[3]==Tile::Project,"hidden in catalog order");
        layout.remove(0);layout.remove(9);
        expect((layout.tiles==std::vector<Tile>{Tile::Mic,Tile::Updates}),"remove by index, out of range ignored");
        // Move forwards, backwards and past the end.
        auto order=QuickLayout::defaults();
        order.move(0,3);
        expect(order.serialize()==L"bluetooth,mic,capture,wifi,timer,updates","move forwards");
        order.move(5,0);
        expect(order.serialize()==L"updates,bluetooth,mic,capture,wifi,timer","move backwards");
        order.move(1,99);
        expect(order.serialize()==L"updates,mic,capture,wifi,timer,bluetooth","move past the end clamps");
        order.move(99,0);
        expect(order.serialize()==L"updates,mic,capture,wifi,timer,bluetooth","bad source ignored");
        // Paging: seven tiles make two pages; removing the seventh leaves one.
        auto paged=QuickLayout::defaults();paged.tiles.push_back(Tile::Mic);
        expect(paged.pages()==2&&paged.at(1,0)==Tile::Mic&&paged.at(1,1)==Tile::Count,"second page");
        paged.remove(6);expect(paged.pages()==1,"page disappears when emptied");
        // Every key is unique and non-empty, so parse can never confuse two tiles.
        for(std::size_t i=0;i<tile_specs.size();++i) {
            expect(!tile_specs[i].key.empty(),"key set");
            for(std::size_t j=i+1;j<tile_specs.size();++j)expect(tile_specs[i].key!=tile_specs[j].key,"keys unique");
        }
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"quick tiles ok\n";
    return 0;
}
