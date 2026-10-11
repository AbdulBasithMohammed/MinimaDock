#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <array>
namespace minima::theme {
inline constexpr float button = 44, icon = 24, launcher_icon = 24, gap = 4, margin = 10, surface_height = 48;
inline D2D1_COLOR_F rgb(unsigned hex, float alpha = 1) { return D2D1::ColorF(hex, alpha); }
inline auto background() { return rgb(0x101319); }
inline auto glass() { return rgb(0x101010, .78f); }
inline auto primary() { return rgb(0xf2f4fa); }
inline auto secondary() { return rgb(0x939eaf); }
inline auto accent() { return rgb(0x9ebaf0); }
inline const wchar_t* text_family(IDWriteFontCollection* collection) {
    for(const auto family:{L"Bahnschrift",L"Segoe UI Variable Text"}) {
        UINT index=0;BOOL available=FALSE;
        if(SUCCEEDED(collection->FindFamilyName(family,&index,&available))&&available)return family;
    }
    return L"Segoe UI";
}
inline auto material_stops() { return std::array<D2D1_GRADIENT_STOP,4>{{{0,rgb(0x888888,.30f)},{.35f,glass()},{.8f,rgb(0x080808,.84f)},{1,rgb(0x343434,.60f)}}}; }
inline auto sheen_stops() { return std::array<D2D1_GRADIENT_STOP,3>{{{0,rgb(0xffffff,.16f)},{.5f,rgb(0xffffff,.02f)},{1,rgb(0xffffff,0)}}}; }
}
