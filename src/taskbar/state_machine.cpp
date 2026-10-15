#include "taskbar/state_machine.h"
#include <algorithm>
#include <cmath>

namespace minima {
void ClickPulse::tick(Time now){if(active())progress_=std::clamp(static_cast<float>(now-start_)/240,0.0f,1.0f);}
float ClickPulse::scale() const{return active()?1-.06f*std::cos(progress_*6.2831853f)*std::exp(-6*progress_):1;}
void PanelReveal::transition(bool opening,Time now,bool reduced){
    tick(now);const float desired=opening?1.0f:0.0f;
    if(reduced){value_=target_=desired;moving_=false;return;}
    if(target_==desired&&moving_)return;
    from_=value_;target_=desired;start_=now;duration_=opening?180:150;moving_=from_!=target_;
}
void PanelReveal::tick(Time now){
    if(!moving_)return;
    const auto t=std::clamp(static_cast<float>(now-start_)/duration_,0.0f,1.0f);
    const auto ease=1-(1-t)*(1-t)*(1-t);value_=from_+(target_-from_)*ease;
    if(t==1){value_=target_;moving_=false;}
}
void DockVisibility::update(bool maximized,bool fullscreen,bool engaged,Time now) {
    tick(now);
    const float desired=!fullscreen&&(!maximized||engaged)?1.0f:0.0f;
    if(fullscreen||reduced_){target_=position_=desired;velocity_=0;moving_=false;previous_=now;return;}
    if(desired==target_)return;
    target_=desired;previous_=now;moving_=true;
}
bool DockVisibility::tick(Time now) {
    if(!moving_)return false;
    const float before=position_,dt=std::max(0.0f,static_cast<float>(now-previous_)/1000);
    previous_=now;
    const float omega=target_==1?36.0f:43.0f,displacement=position_-target_,coefficient=velocity_+omega*displacement;
    const float decay=std::exp(-omega*dt);
    position_=target_+(displacement+coefficient*dt)*decay;
    velocity_=(velocity_-omega*coefficient*dt)*decay;
    if(position_<0||position_>1){position_=std::clamp(position_,0.0f,1.0f);velocity_=0;}
    if(std::abs(position_-target_)<.002f&&std::abs(velocity_)<.08f){position_=target_;velocity_=0;moving_=false;}
    return before!=position_;
}
void DockVisibility::reduced_motion(bool value) {
    reduced_=value;
    if(value){position_=target_;velocity_=0;moving_=false;}
}
void DockMachine::hold(Hold reason, bool enabled, Time now) {
    if (state_ == DockState::Recovering || state_ == DockState::SuppressedFullscreen) return;
    const auto old = holds_;
    if (enabled) holds_ |= static_cast<unsigned>(reason); else holds_ &= ~static_cast<unsigned>(reason);
    if (old == holds_) return;
    tick(now);
    target(holds_ ? 1.0f : 0.0f, now);
}
void DockMachine::target(float value, Time now) {
    if((state_==DockState::Expanding||state_==DockState::Collapsing)&&target_==value)return;
    if (progress_ == value) { state_ = value == 1 ? DockState::Expanded : DockState::Collapsed; return; }
    start_ = progress_; target_ = value; animation_start_ = now;
    state_ = value == 1 ? DockState::Expanding : DockState::Collapsing;
    if (reduced_) { progress_ = value; state_ = value == 1 ? DockState::Expanded : DockState::Collapsed; }
}
bool DockMachine::tick(Time now) {
    const float before = progress_;
    if (state_ == DockState::Recovering || state_ == DockState::SuppressedFullscreen) return false;
    if (state_ == DockState::Expanding || state_ == DockState::Collapsing) {
        const float duration = 90.0f * std::abs(target_ - start_);
        const float t = duration > 0 ? std::clamp(static_cast<float>(now - animation_start_) / duration, 0.0f, 1.0f) : 1.0f;
        const float ease = t * t * (3 - 2 * t);
        progress_ = start_ + (target_ - start_) * ease;
        if (t == 1) state_ = target_ == 1 ? DockState::Expanded : DockState::Collapsed;
    }
    return progress_ != before;
}
void DockMachine::suppress(bool enabled, Time now) {
    if (state_ == DockState::Recovering) return;
    if (enabled) { state_ = DockState::SuppressedFullscreen; holds_ = 0; progress_ = 0; }
    else if (state_ == DockState::SuppressedFullscreen) { state_ = DockState::Collapsed; animation_start_ = now; }
}
void DockMachine::recover(Time) { state_ = DockState::Recovering; holds_ = 0; progress_ = 0; }
bool DockMachine::visible() const { return state_ != DockState::SuppressedFullscreen && state_ != DockState::Recovering; }
bool DockMachine::needs_tick() const { return state_ == DockState::Expanding || state_ == DockState::Collapsing; }
}
