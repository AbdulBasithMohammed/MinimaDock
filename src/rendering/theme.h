#pragma once
#include <windows.h>
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

// Smoked-crystal glass. Dark is the approved default; light follows the
// Windows system (taskbar) colour mode. Every surface reads these roles
// instead of literal colours so both modes stay legible.
struct Palette {
    bool light=false;
    D2D1_COLOR_F ink,ink_secondary,ink_muted;   // text and glyphs
    D2D1_COLOR_F hover,hover_strong;            // pointer wash behind a target
    D2D1_COLOR_F separator,field,tile,tile_edge,track;
    D2D1_COLOR_F accent,on_accent,running;
    // Glass recipe. `tint_*` sit over the live blurred backdrop; `solid_*`
    // replace it when transparency is off or composition is unavailable.
    D2D1_COLOR_F tint_top,tint_bottom,solid_top,solid_bottom;
    float boost=0;                               // extra tint over busy app content
    float sheen,underglow,rim_top,rim_mid,rim_bottom,glint,edge,grain,shadow;
    D2D1_COLOR_F edge_color,shadow_color,tooltip_top,tooltip_bottom;
};
inline Palette palette(bool light) {
    Palette p;p.light=light;
    if(!light) {
        p.ink=rgb(0xf4f5f8);p.ink_secondary=rgb(0xa9afba);p.ink_muted=rgb(0xf4f5f8,.38f);
        p.hover=rgb(0xffffff,.075f);p.hover_strong=rgb(0xffffff,.12f);
        p.separator=rgb(0xffffff,.12f);p.field=rgb(0x000000,.30f);p.tile=rgb(0xffffff,.065f);p.tile_edge=rgb(0xffffff,.10f);p.track=rgb(0xffffff,.16f);
        p.accent=rgb(0x9ebaf0);p.on_accent=rgb(0x0e1118);p.running=rgb(0xf4f5f8,.78f);
        p.tint_top=rgb(0x1c1c22,.46f);p.tint_bottom=rgb(0x0a0a0d,.60f);
        p.solid_top=rgb(0x222228);p.solid_bottom=rgb(0x0c0c0f);p.boost=.16f; // Solid is opaque: no blur hides what is behind.
        p.sheen=.10f;p.underglow=.05f;p.rim_top=.46f;p.rim_mid=.07f;p.rim_bottom=.16f;p.glint=.62f;p.edge=.42f;p.grain=.045f;p.shadow=1;
        p.edge_color=rgb(0x000000);p.shadow_color=rgb(0x000000);
        p.tooltip_top=rgb(0x26262c,.72f);p.tooltip_bottom=rgb(0x16161a,.80f);
    } else {
        p.ink=rgb(0x15171c);p.ink_secondary=rgb(0x555b66);p.ink_muted=rgb(0x15171c,.40f);
        p.hover=rgb(0x000000,.06f);p.hover_strong=rgb(0x000000,.10f);
        p.separator=rgb(0x000000,.11f);p.field=rgb(0xffffff,.55f);p.tile=rgb(0x000000,.045f);p.tile_edge=rgb(0x000000,.07f);p.track=rgb(0x000000,.14f);
        p.accent=rgb(0x2f5fb8);p.on_accent=rgb(0xffffff);p.running=rgb(0x15171c,.70f);
        p.tint_top=rgb(0xfbfbfd,.56f);p.tint_bottom=rgb(0xececf1,.66f);
        p.solid_top=rgb(0xf8f8fa);p.solid_bottom=rgb(0xe8e8ed);p.boost=.18f;
        p.sheen=.30f;p.underglow=.20f;p.rim_top=.90f;p.rim_mid=.30f;p.rim_bottom=.55f;p.glint=.95f;p.edge=.16f;p.grain=.03f;p.shadow=.55f;
        p.edge_color=rgb(0x000000);p.shadow_color=rgb(0x000000);
        p.tooltip_top=rgb(0xffffff,.78f);p.tooltip_bottom=rgb(0xf2f2f5,.84f);
    }
    return p;
}
inline DWORD personalize(const wchar_t* name,DWORD fallback) {
    DWORD value=fallback,size=sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",name,RRF_RT_REG_DWORD,nullptr,&value,&size);
    return value;
}
// Personal accent from Windows (Settings > Personalization > Colors). The
// AccentPalette holds eight RGBA shades: light 3/2/1, base, dark 1/2/3. Dark
// glass uses Light 2 (as Windows does on dark surfaces), light glass Dark 1.
inline void apply_system_accent(Palette& p) {
    BYTE shades[32]{};DWORD size=sizeof(shades);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",L"AccentPalette",RRF_RT_REG_BINARY,nullptr,shades,&size)!=ERROR_SUCCESS||size<32)return;
    const BYTE* c=shades+(p.light?4*4:1*4);
    p.accent=D2D1::ColorF(c[0]/255.0f,c[1]/255.0f,c[2]/255.0f,1);
    const float luminance=.2126f*p.accent.r+.7152f*p.accent.g+.0722f*p.accent.b;
    p.on_accent=luminance>.45f?rgb(0x0e1118):rgb(0xffffff);
}
// The taskbar follows the *system* mode, not the app mode (as Windows does).
inline bool system_light(){return personalize(L"SystemUsesLightTheme",0)!=0;}
inline bool transparency_enabled(){return personalize(L"EnableTransparency",1)!=0;}
}
