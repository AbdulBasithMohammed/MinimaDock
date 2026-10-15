#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace minima {
// Under-damped spring for hover motion and drag shifts: a short settle with an
// overshoot set by tune(). Time is monotonic milliseconds from the caller.
class Spring {
public:
    using Time=std::int64_t;
    void target(float value,Time now,bool reduced=false) {
        if(reduced){value_=target_=value;velocity_=0;moving_=false;previous_=now;return;}
        if(value==target_&&!moving_)return;
        if(!moving_)previous_=now;
        target_=value;moving_=value_!=target_||velocity_!=0;
    }
    void snap(float value){value_=target_=value;velocity_=0;moving_=false;}
    void tune(float stiffness,float damping){stiffness_=stiffness;damping_=damping;}
    bool tick(Time now) {
        if(!moving_)return false;
        // Fixed 4 ms substeps keep the integration stable at any timer cadence.
        float dt=std::clamp(static_cast<float>(now-previous_)/1000,0.0f,.05f);previous_=now;
        while(dt>0) {
            const float h=std::min(dt,.004f);dt-=h;
            velocity_+=(stiffness_*(target_-value_)-damping_*velocity_)*h;value_+=velocity_*h;
        }
        if(std::abs(target_-value_)<.0015f&&std::abs(velocity_)<.02f){value_=target_;velocity_=0;moving_=false;}
        return true;
    }
    float value() const{return value_;}
    float goal() const{return target_;}
    bool moving() const{return moving_;}
private:
    float value_=0,target_=0,velocity_=0;
    float stiffness_=520,damping_=30; // Drag shifts; hover motion is tuned softer.
    Time previous_=0;
    bool moving_=false;
};
// Eased glide between two positions (the active-app indicator).
class Glide {
public:
    using Time=std::int64_t;
    static constexpr Time duration=200;
    void to(float value,Time now,bool reduced=false) {
        if(!placed_||reduced){from_=to_=value;placed_=true;moving_=false;return;}
        if(value==to_)return;
        from_=current(now);to_=value;start_=now;moving_=true;
    }
    void clear(){placed_=false;moving_=false;}
    bool placed() const{return placed_;}
    bool tick(Time now){if(moving_&&now-start_>=duration)moving_=false;return moving_;}
    bool moving() const{return moving_;}
    float current(Time now) const {
        if(!moving_)return to_;
        const float t=std::clamp(static_cast<float>(now-start_)/duration,0.0f,1.0f);
        const float ease=1-(1-t)*(1-t)*(1-t);
        return from_+(to_-from_)*ease;
    }
    float target() const{return to_;}
private:
    float from_=0,to_=0;
    Time start_=0;
    bool placed_=false,moving_=false;
};
}
