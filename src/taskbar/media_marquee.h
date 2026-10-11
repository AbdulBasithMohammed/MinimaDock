#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace minima {
// A paused, reversible title scroll. Only visible overflow needs a timer.
class MediaMarquee {
public:
    using Time=std::int64_t;
    void configure(float overflow){distance_=std::max(0.0f,overflow);stop();}
    void visible(bool shown,bool reduced,Time now){
        const bool wanted=shown&&!reduced&&distance_>0;
        if(wanted!=active_){active_=wanted;start_=now;offset_=0;}
    }
    void tick(Time now){
        if(!active_)return;
        const double travel=std::max(1000.0,static_cast<double>(distance_)*1000/24);
        const double elapsed=std::fmod(static_cast<double>(std::max<Time>(0,now-start_)),2*(travel+1200));
        double progress=0;
        if(elapsed<1200)progress=0;
        else if(elapsed<1200+travel)progress=(elapsed-1200)/travel;
        else if(elapsed<2400+travel)progress=1;
        else progress=1-(elapsed-2400-travel)/travel;
        offset_=distance_*static_cast<float>(progress*progress*(3-2*progress));
    }
    void stop(){active_=false;offset_=0;}
    bool active() const{return active_;}
    float offset() const{return offset_;}
    float distance() const{return distance_;}
private:
    float distance_=0,offset_=0;
    Time start_=0;
    bool active_=false;
};
}
