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
        for(size_t i=0;i<fixture.pinned.size();++i)
            if(scene.groups[i].app_id!=fixture.pinned[i].app_id||!scene.groups[i].pinned)
                throw std::runtime_error("Pinned app order differs from saved taskbar order");
        if(scene.launcher_rect().right>scene.app_rect(0).left)
            throw std::runtime_error("Launcher does not precede all apps");
        const auto initial_launcher=scene.layout().launcher_x();
        const auto count=scene.groups.size();
        const auto width=scene.layout().expanded_width();
        std::vector<float> positions;
        for(size_t i=0;i<count;++i)positions.push_back(scene.app_x(i));
        const auto expanded=scene.dock();
        const auto desktop=scene.desktop_button_rect();
        if(scene.media_rect().right>=scene.launcher_rect().left||scene.system_rect().left<=scene.app_rect(count-1).right)
            throw std::runtime_error("Media/system wings do not flank the centered app group");
        if(std::abs((expanded.left+expanded.right)/2-scene.width/2)>.001f)
            throw std::runtime_error("Expanded pill is not centered");
        scene.machine.reduced_motion(false);
        scene.machine.hold(minima::Hold::Keyboard,false,0);
        for(int time:{0,25,50,90}){scene.machine.tick(time);for(size_t i=0;i<count;++i)if(scene.app_x(i)!=positions[i])throw std::runtime_error("Hiding utility wings moved an app");if(scene.desktop_button_rect().left!=desktop.left)throw std::runtime_error("Hiding utilities swept the desktop target across an app");}
        for(size_t i=0;i<count;++i){const auto button=scene.app_rect(i);if(button.left<scene.dock().left||button.right>scene.dock().right)throw std::runtime_error("Collapsed pill does not fit every pin and running app");scene.press_at(scene.app_x(i),scene.layout().center_y);if(!scene.groups[i].pressed)throw std::runtime_error("Collapsed pinned/running app press was ignored");scene.clear_press();}
        const auto compact=scene.dock();
        if(scene.layout().launcher_x()-compact.left!=compact.right-scene.app_x(count-1))throw std::runtime_error("Compact first/last icon padding is uneven");
        scene.press_at(compact.right-2,scene.layout().center_y);
        if(scene.control_pressed!=-1)throw std::runtime_error("Compact trailing padding still activates a utility control");
        scene.clear_press();
        scene.machine.hold(minima::Hold::Keyboard,true,91);
        for(int time:{91,110,140,181}){scene.machine.tick(time);for(size_t i=0;i<count;++i)if(scene.app_x(i)!=positions[i])throw std::runtime_error("Showing utility wings moved an app");}
        scene.machine.reduced_motion(true);
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
        for(size_t i=0;i<count;++i)if(scene.app_x(i)-scene.layout().launcher_x()!=positions[i]-initial_launcher)
            throw std::runtime_error("New app shuffled existing icons despite a free appended slot");
        if(!scene.accept(fixture,false)||scene.groups.size()!=count)
            throw std::runtime_error("Closed unpinned app was not removed");
        for(size_t i=0;i<count;++i)if(scene.app_x(i)!=positions[i])
            throw std::runtime_error("Removing the new app changed established slots");
        std::cout<<"Pin inventory regression passed: "<<transitions<<" open/group/minimize/close transitions, no duplicate slots, fixed expanded positions and correct running indicator state.\n";
        std::cout<<"Identity mapping passed: appends retain slot order; the compact group stays centered.\n";
        std::cout<<"Fixed app strip passed: every pin/running app fits and accepts press while collapsed; all coordinates stay fixed through utility reveal/hide frames.\n";
        auto reordered=fixture;
        std::reverse(reordered.pinned.begin(),reordered.pinned.end());
        if(!scene.accept(reordered,true))throw std::runtime_error("Pin reorder was not accepted");
        for(size_t i=0;i<reordered.pinned.size();++i)
            if(scene.groups[i].app_id!=reordered.pinned[i].app_id)
                throw std::runtime_error("Saved pin reorder was ignored");
        auto focused=fixture;
        focused.windows.insert(focused.windows.begin(),{reinterpret_cast<HWND>(200),L"New",L"C:\\fixture\\new.exe",{},false,{},L"fixture.new",{}});
        scene.accept(focused,true);
        if(scene.groups[count-1].app_id!=L"fixture.other"||scene.groups[count].app_id!=L"fixture.new")
            throw std::runtime_error("New foreground app did not append after established running apps");
        std::reverse(focused.windows.begin(),focused.windows.end());scene.accept(focused,false);
        if(scene.groups[count-1].app_id!=L"fixture.other"||scene.groups[count].app_id!=L"fixture.new")
            throw std::runtime_error("Foreground changes reordered unpinned apps");
        scene.accept(fixture,true);
        scene.stop_feedback();
        if(scene.feedback_moving()||scene.groups[1].pressed)throw std::runtime_error("Feedback did not settle on reduced motion/cancellation");
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
