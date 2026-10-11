// Exercise the actual inventory merge and Scene layout without launching apps,
// changing pins, or touching the desktop. Handles are opaque fixture identities.
#include "../../src/rendering/preview.cpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        minima::Scene scene;
        scene.width=1000;scene.height=88;
        scene.machine.reduced_motion(true);
        scene.machine.hold(minima::Hold::Keyboard,true,0);
        minima::Inventory fixture;
        for(size_t i=0;i<7;++i) {
            const auto name=L"Pin"+std::to_wstring(i),path=L"C:\\fixture\\"+name+L".exe";
            fixture.pinned.push_back({name,path+L".lnk",false,path,{},L"fixture."+name});
        }
        // Keep some pins and an unpinned app running throughout the sequence.
        for(size_t i:{size_t{0},size_t{2},size_t{6}}) {
            const auto& pin=fixture.pinned[i];
            fixture.windows.push_back({reinterpret_cast<HWND>(i+1),pin.title,pin.target,{},false,{},pin.app_id,{}});
        }
        fixture.windows.push_back({reinterpret_cast<HWND>(100),L"Other",L"C:\\fixture\\other.exe",{},false,{},L"fixture.other",{}});
        if(!scene.accept(fixture,true))throw std::runtime_error("Initial inventory was not accepted");
        const auto count=scene.groups.size();
        const auto width=scene.layout().expanded_width();
        std::vector<float> positions;
        for(size_t i=0;i<count;++i)positions.push_back(scene.app_x(i));
        scene.groups[1].pulse.start(0);scene.groups[1].pressed=true;
        if(scene.accept(fixture,false)||!scene.groups[1].pulse.active()||!scene.groups[1].pressed)
            throw std::runtime_error("Unchanged inventory discarded click feedback");
        int transitions=0;
        for(int repeat=0;repeat<8;++repeat) {
            // Simulate a new pinned app opening, gaining another window,
            // minimizing, and closing. Foreground Z order is reversed each scan.
            for(int state=1;state<=4;++state) {
                auto update=fixture;
                const auto& pin=fixture.pinned[1];
                if(state<4)update.windows.push_back({reinterpret_cast<HWND>(20),pin.title,pin.target,{},state==3,{},pin.app_id,{}});
                if(state==2)update.windows.push_back({reinterpret_cast<HWND>(21),pin.title,pin.target,{},false,{},pin.app_id,{}});
                std::reverse(update.windows.begin(),update.windows.end());
                if(!scene.accept(std::move(update),false))throw std::runtime_error("Window transition was not accepted");
                if(!scene.groups[1].pulse.active()||!scene.groups[1].pressed)throw std::runtime_error("Pin window update discarded its click feedback");
                if(scene.groups.size()!=count||scene.layout().expanded_width()!=width)throw std::runtime_error("Opening a pin added a slot or changed expanded width");
                for(size_t i=0;i<count;++i)
                    if(scene.app_x(i)!=positions[i])throw std::runtime_error("Pin state update moved an expanded icon");
                if(!scene.groups[1].pinned||scene.tile_running(1)!=(state<4))throw std::runtime_error("Pin running indicator state did not track its windows");
                if(scene.groups[1].windows.size()!=(state==2?2u:state==4?0u:1u))throw std::runtime_error("Pinned windows did not merge into the existing icon");
                ++transitions;
            }
        }
        auto added=fixture;
        added.windows.push_back({reinterpret_cast<HWND>(200),L"New",L"C:\\fixture\\new.exe",{},false,{},L"fixture.new",{}});
        if(!scene.accept(std::move(added),false)||scene.groups.size()!=count+1)
            throw std::runtime_error("New unpinned app was not added");
        for(size_t i=0;i<count;++i)if(scene.app_x(i)!=positions[i])
            throw std::runtime_error("New app shuffled existing icons despite a free balanced side");
        if(!scene.accept(fixture,false)||scene.groups.size()!=count)
            throw std::runtime_error("Closed unpinned app was not removed");
        for(size_t i=0;i<count;++i)if(scene.app_x(i)!=positions[i])
            throw std::runtime_error("Removing the new app changed established slots");
        std::cout<<"Pin inventory regression passed: "<<transitions<<" open/group/minimize/close transitions, no duplicate slots, fixed expanded positions and correct running indicator state.\n";
        std::cout<<"Identity mapping passed: add/remove an unpinned app without shuffling established icons.\n";
        scene.stop_feedback();
        if(scene.feedback_moving()||scene.groups[1].pressed)throw std::runtime_error("Feedback did not settle on reduced motion/cancellation");
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
