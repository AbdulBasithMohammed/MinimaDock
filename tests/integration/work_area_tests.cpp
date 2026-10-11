#include "recovery/work_area.h"
#include <iostream>
#include <stdexcept>
int main() {
    unsigned checks=0;
    const auto expect=[&](bool value){++checks;if(!value)throw std::runtime_error("Work area geometry failed");};
    try {
        const RECT monitor{0,0,2560,1440},work{0,0,2560,1392},bar{0,1392,2560,1440};
        expect(minima::same_rect(minima::without_bottom_taskbar(monitor,work,bar),monitor));
        const RECT with_other_toolbar{80,40,2560,1392},preserved{80,40,2560,1440};
        expect(minima::same_rect(minima::without_bottom_taskbar(monitor,with_other_toolbar,bar),preserved));
        const RECT auto_hide{0,0,2560,1438},auto_bar{0,1438,2560,1486};
        expect(minima::same_rect(minima::without_bottom_taskbar(monitor,auto_hide,auto_bar),monitor));
        const RECT other_monitor{-1920,-400,0,680},other_work{-1920,-400,0,632},other_bar{-1920,632,0,680};
        expect(minima::same_rect(minima::without_bottom_taskbar(other_monitor,other_work,other_bar),other_monitor));
        expect(minima::same_rect(minima::without_bottom_taskbar(monitor,work,other_bar),work));
        expect(minima::same_rect(minima::without_bottom_taskbar(monitor,monitor,bar),monitor));
        const RECT bottom_toolbar{0,0,2560,1344};
        expect(minima::same_rect(minima::without_bottom_taskbar(monitor,bottom_toolbar,bar),bottom_toolbar));
        expect(minima::contained_rect(monitor,work));
        expect(!minima::contained_rect(monitor,other_work));
        expect(!minima::contained_rect(monitor,{0,0,0,1440}));
        RECT result{};
        expect(minima::reflow_frame(work,monitor,{0,0,1280,1392},false,true,result)&&minima::same_rect(result,{0,0,1280,1440}));
        expect(minima::reflow_frame(work,monitor,{1280,0,2560,1392},false,true,result)&&minima::same_rect(result,{1280,0,2560,1440}));
        expect(minima::reflow_frame(monitor,work,{1280,0,2560,1440},false,true,result)&&minima::same_rect(result,{1280,0,2560,1392}));
        expect(!minima::reflow_frame(work,monitor,{0,0,1280,696},false,true,result));
        expect(!minima::reflow_frame(work,monitor,{1280,696,2560,1392},false,true,result));
        expect(!minima::reflow_frame(work,monitor,{0,0,1280,1392},false,false,result));
        expect(!minima::reflow_frame(work,monitor,{0,0,2560,1440},false,true,result));
        expect(minima::reflow_frame(work,monitor,work,true,false,result)&&minima::same_rect(result,monitor));
        expect(!minima::reflow_frame(work,monitor,{0,0,2400,1392},true,false,result));
        expect(!minima::reflow_frame(work,work,{0,0,1280,1392},false,true,result));
        std::cout<<checks<<" work-area geometry checks passed. No live work area changed.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
