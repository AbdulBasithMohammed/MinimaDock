#pragma once
#include <algorithm>
#include <cstddef>
#include <cmath>

namespace minima {
// Logical pixels (DIPs). The ordered app group stays centered while hover
// reveals media on the left and system controls on the right; both wings are
// `utility_width` wide.
// The launcher and each app's ordered slot keep their coordinates.
struct DockLayout {
    float center, center_y, available_width;
    std::size_t apps;
    // Animated app count while apps open or close (negative: use `apps`); the
    // pill's width and centred position follow it smoothly.
    float slots=-1;
    bool media=true; // The left (media) wing; without it the dock only extends right.
    bool bar=false;float bar_width=0; // Full-width style: the wings reach the screen edges.
    // `edge`: extra inner padding at both ends of the pill.
    static constexpr float button=44, expanded_height=button, pitch=40, utility_width=210, desktop_width=14, edge=5;
    static float compact_width(std::size_t count) { return button+static_cast<float>(count)*pitch+2*edge; }
    static float compact_width_at(float count) { return button+count*pitch+2*edge; }
    float count() const { return slots>=0?slots:static_cast<float>(apps); }
    // Reserve the desktop sliver only in the expanded utility wings.
    static float natural_expanded_width(std::size_t count) { return compact_width(count)+2*(utility_width+12)+desktop_width+12; }
    static std::size_t capacity(float available) {
        return static_cast<std::size_t>(std::max(0.0f,std::floor((available-natural_expanded_width(0))/pitch)));
    }
    float left() const { return center-collapsed_width()/2; }
    float launcher_x() const { return left()+edge+button/2; }
    float app_x(std::size_t index) const { return left()+edge+button+18+static_cast<float>(index)*pitch; }
    float collapsed_width() const { return compact_width_at(count()); }
    // Extra width each wing adds when expanded (squeezed to fit narrow monitors).
    static float side_extra() { return utility_width+12+(desktop_width+12)/2; }
    float left_extra() const { return media?side_extra():12; }
    float right_extra() const { return side_extra(); }
    float extra_scale() const {
        const float want=left_extra()+right_extra(),room=std::max(0.0f,available_width-collapsed_width());
        return want>room&&want>0?room/want:1.0f;
    }
    // Never wider than the monitor allows (as before the wings became independent).
    float expanded_left() const { return bar?0:std::max(center-available_width/2,center-collapsed_width()/2-left_extra()*extra_scale()); }
    float expanded_right() const { return bar?bar_width:std::min(center+available_width/2,center+collapsed_width()/2+right_extra()*extra_scale()); }
    float expanded_width() const { return expanded_right()-expanded_left(); }
    float width(float progress) const { return collapsed_width()+(expanded_width()-collapsed_width())*progress; }
    float height(float) const { return button; }
};
}
