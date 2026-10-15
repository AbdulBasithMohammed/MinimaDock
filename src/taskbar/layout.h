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
    // `edge`: extra inner padding at both ends of the pill.
    static constexpr float button=44, expanded_height=button, pitch=40, utility_width=210, desktop_width=14, edge=5;
    static float compact_width(std::size_t count) { return button+static_cast<float>(count)*pitch+2*edge; }
    // Reserve the desktop sliver only in the expanded utility wings.
    static float natural_expanded_width(std::size_t count) { return compact_width(count)+2*(utility_width+12)+desktop_width+12; }
    static std::size_t capacity(float available) {
        return static_cast<std::size_t>(std::max(0.0f,std::floor((available-natural_expanded_width(0))/pitch)));
    }
    float left() const { return center-collapsed_width()/2; }
    float launcher_x() const { return left()+edge+button/2; }
    float app_x(std::size_t index) const { return left()+edge+button+18+static_cast<float>(index)*pitch; }
    float collapsed_width() const { return compact_width(apps); }
    float expanded_width() const { return std::min(available_width,natural_expanded_width(apps)); }
    float width(float progress) const { return collapsed_width()+(expanded_width()-collapsed_width())*progress; }
    float height(float) const { return button; }
};
}
