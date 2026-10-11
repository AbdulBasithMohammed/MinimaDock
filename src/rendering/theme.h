#pragma once
#include <d2d1.h>
namespace minima::theme {
inline constexpr float button = 44, icon = 24, launcher_icon = 24, gap = 4, margin = 10, surface_height = 48;
inline D2D1_COLOR_F rgb(unsigned hex, float alpha = 1) { return D2D1::ColorF(hex, alpha); }
inline auto background() { return rgb(0x101319); }
inline auto glass() { return rgb(0x101010, .78f); }
inline auto primary() { return rgb(0xf2f4fa); }
inline auto secondary() { return rgb(0x939eaf); }
inline auto accent() { return rgb(0x9ebaf0); }
}
