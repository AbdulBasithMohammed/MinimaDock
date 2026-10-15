#pragma once
#include <cstdint>

namespace minima {
// Finite effects never hold the dock open or delay an action.
class ClickPulse {
public:
    using Time=std::int64_t;
    void start(Time now,bool reduced=false){start_=now;progress_=reduced?1.0f:0.0f;}
    void tick(Time now);
    void stop(){progress_=1;}
    [[nodiscard]] bool active() const{return progress_<1;}
    [[nodiscard]] float radius() const{return 10+8*(1-(1-progress_)*(1-progress_));}
    [[nodiscard]] float opacity() const{return .16f*(1-progress_)*(1-progress_);}
    [[nodiscard]] float scale() const;
private:
    Time start_=0;
    float progress_=1;
};
class PanelReveal {
public:
    using Time=std::int64_t;
    void start(Time now,bool reduced=false){value_=target_=0;moving_=false;transition(true,now,reduced);}
    void open(Time now,bool reduced=false){transition(true,now,reduced);}
    void close(Time now,bool reduced=false){transition(false,now,reduced);}
    void tick(Time now);
    void settle(){value_=target_;moving_=false;}
    [[nodiscard]] bool active() const{return moving_;}
    [[nodiscard]] bool hidden() const{return value_==0&&!moving_;}
    [[nodiscard]] float opacity() const{return value_;}
    [[nodiscard]] float offset() const{return 8*(1-opacity());}
    [[nodiscard]] float scale() const{return .98f+.02f*opacity();}
private:
    void transition(bool opening,Time now,bool reduced);
    Time start_=0,duration_=180;
    float value_=1,from_=1,target_=1;
    bool moving_=false;
};
// Independent of horizontal expansion: bounded critically damped visibility
// motion keeps position and velocity continuous when the user reverses direction.
class DockVisibility {
public:
    using Time=std::int64_t;
    void update(bool maximized,bool fullscreen,bool engaged,Time now);
    bool tick(Time now);
    void reduced_motion(bool value);
    [[nodiscard]] float progress() const{return position_;}
    [[nodiscard]] float offset() const{return 58*(1-position_);}
    [[nodiscard]] float opacity() const{return position_*position_*(3-2*position_);}
    [[nodiscard]] bool wanted() const{return target_==1;}
    [[nodiscard]] bool needs_tick() const{return moving_;}
private:
    float position_=1,velocity_=0,target_=1;
    Time previous_=0;
    bool moving_=false,reduced_=false;
};
enum class DockState { Collapsed, Expanding, Expanded, Collapsing, SuppressedFullscreen, Recovering };
enum class Hold : unsigned { Pointer = 1, Keyboard = 2, Flyout = 4, ContextMenu = 8, Drag = 16 };
class DockMachine {
public:
    using Time = std::int64_t; // Monotonic milliseconds, supplied by caller.
    void hold(Hold reason, bool enabled, Time now);
    void suppress(bool enabled, Time now);
    void recover(Time now);
    bool tick(Time now);
    [[nodiscard]] bool needs_tick() const;
    [[nodiscard]] float progress() const { return progress_; }
    [[nodiscard]] DockState state() const { return state_; }
    [[nodiscard]] bool visible() const;
    [[nodiscard]] unsigned holds() const { return holds_; }
    void reduced_motion(bool value) {
        reduced_ = value;
        if (value && (state_ == DockState::Expanding || state_ == DockState::Collapsing)) {
            progress_ = target_;
            state_ = target_ == 1 ? DockState::Expanded : DockState::Collapsed;
        }
    }
private:
    void target(float value, Time now);
    DockState state_ = DockState::Collapsed;
    unsigned holds_ = 0;
    float progress_ = 0, start_ = 0, target_ = 0;
    Time animation_start_ = 0;
    bool reduced_ = false;
};
}
