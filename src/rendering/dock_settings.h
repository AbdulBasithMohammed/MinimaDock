#pragma once
#include "rendering/theme.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace minima {
// Everything the Customize window changes about the dock itself, stored in
// HKCU\Software\MinimaDock\Settings (one DWORD per field; missing = default).
struct DockSettings {
    enum class Theme {System,Light,Dark};
    enum class Tint {Smoke,Midnight,Forest,Plum,Sand,Frost};
    enum class Style {Floating,Bar};
    enum class Hide {OverMaximized,Always,Never};
    enum class Motion {Windows,Full,Reduced};
    Theme theme=Theme::System;
    bool custom_accent=false;unsigned accent=0x9ebaf0; // 0xRRGGBB when custom.
    Tint tint=Tint::Smoke;
    int glass=55;          // 0 clear .. 100 solid-looking (live glass tint strength).
    bool solid=false;      // No blur or transparency.
    Style style=Style::Floating;
    Hide hide=Hide::OverMaximized;
    bool bar_autohide=false,bar_left=false;
    int icon=24;           // 20, 24 or 28 DIP.
    int magnify=122;       // Hover scale x100: 100 (off), 112, 122, 140.
    int hide_delay=0;      // ms a revealed (hiding) dock stays up after the pointer leaves: 0, 500, 1000, 2000.
    Motion motion=Motion::Windows;
    bool media=true,date=true,clock24=false;
    bool operator==(const DockSettings&) const=default;
};

inline constexpr wchar_t dock_settings_key[]=L"Software\\MinimaDock\\Settings";
namespace detail {
inline DWORD read_dword(const wchar_t* name,DWORD fallback) {
    DWORD value=fallback,size=sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,dock_settings_key,name,RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS?value:fallback;
}
inline void write_dword(const wchar_t* name,DWORD value){RegSetKeyValueW(HKEY_CURRENT_USER,dock_settings_key,name,REG_DWORD,&value,sizeof(value));}
template<class E> E pick(DWORD value,E last,E fallback){return value<=static_cast<DWORD>(last)?static_cast<E>(value):fallback;}
}
inline DockSettings load_dock_settings() {
    using namespace detail;DockSettings s;
    s.theme=pick(read_dword(L"Theme",0),DockSettings::Theme::Dark,DockSettings::Theme::System);
    s.custom_accent=read_dword(L"CustomAccent",0)!=0;s.accent=read_dword(L"Accent",0x9ebaf0)&0xffffff;
    s.tint=pick(read_dword(L"Tint",0),DockSettings::Tint::Frost,DockSettings::Tint::Smoke);
    s.glass=static_cast<int>(std::min<DWORD>(100,read_dword(L"Glass",55)));
    s.solid=read_dword(L"Solid",0)!=0;
    s.style=pick(read_dword(L"Style",0),DockSettings::Style::Bar,DockSettings::Style::Floating);
    s.hide=pick(read_dword(L"Hide",0),DockSettings::Hide::Never,DockSettings::Hide::OverMaximized);
    s.bar_autohide=read_dword(L"BarAutoHide",0)!=0;s.bar_left=read_dword(L"BarLeft",0)!=0;
    const DWORD icon=read_dword(L"IconSize",24);s.icon=icon==20||icon==28?static_cast<int>(icon):24;
    const DWORD magnify=read_dword(L"Magnify",122);s.magnify=magnify==100||magnify==112||magnify==140?static_cast<int>(magnify):122;
    const DWORD delay=read_dword(L"HideDelay",0);s.hide_delay=delay==500||delay==1000||delay==2000?static_cast<int>(delay):0;
    s.motion=pick(read_dword(L"Motion",0),DockSettings::Motion::Reduced,DockSettings::Motion::Windows);
    s.media=read_dword(L"Media",1)!=0;s.date=read_dword(L"Date",1)!=0;s.clock24=read_dword(L"Clock24",0)!=0;
    return s;
}
inline void save_dock_settings(const DockSettings& s) {
    using namespace detail;
    write_dword(L"Theme",static_cast<DWORD>(s.theme));write_dword(L"CustomAccent",s.custom_accent);write_dword(L"Accent",s.accent);
    write_dword(L"Tint",static_cast<DWORD>(s.tint));write_dword(L"Glass",static_cast<DWORD>(s.glass));write_dword(L"Solid",s.solid);
    write_dword(L"Style",static_cast<DWORD>(s.style));write_dword(L"Hide",static_cast<DWORD>(s.hide));
    write_dword(L"BarAutoHide",s.bar_autohide);write_dword(L"BarLeft",s.bar_left);
    write_dword(L"IconSize",static_cast<DWORD>(s.icon));write_dword(L"Magnify",static_cast<DWORD>(s.magnify));write_dword(L"HideDelay",static_cast<DWORD>(s.hide_delay));
    write_dword(L"Motion",static_cast<DWORD>(s.motion));write_dword(L"Media",s.media);write_dword(L"Date",s.date);write_dword(L"Clock24",s.clock24);
}
// Resets the appearance fields only (Customize > About > Reset appearance).
inline DockSettings default_appearance(DockSettings s) {
    const DockSettings d;s.theme=d.theme;s.custom_accent=d.custom_accent;s.accent=d.accent;s.tint=d.tint;s.glass=d.glass;s.solid=d.solid;return s;
}

// Light or dark for these settings (Frost glass is always light).
inline bool dock_light(const DockSettings& s) {
    if(s.tint==DockSettings::Tint::Frost)return true;
    return s.theme==DockSettings::Theme::System?theme::system_light():s.theme==DockSettings::Theme::Light;
}
inline bool dock_transparency(const DockSettings& s){return !s.solid&&theme::transparency_enabled();}
// The palette every dock surface draws with: theme, accent, glass color and strength.
inline theme::Palette dock_palette(const DockSettings& s) {
    auto p=theme::palette(dock_light(s));
    if(s.custom_accent) {
        p.accent=theme::rgb(s.accent);
        const float luminance=.2126f*p.accent.r+.7152f*p.accent.g+.0722f*p.accent.b;
        p.on_accent=luminance>.45f?theme::rgb(0x0e1118):theme::rgb(0xffffff);
    } else theme::apply_system_accent(p);
    // Glass color: Smoke keeps the theme's own tint; the others recolor it.
    struct Tint {unsigned top,bottom;};
    constexpr Tint tints[]={{0,0},{0x16213a,0x080d1a},{0x17261f,0x08110d},{0x2a1a2c,0x120a14},{0x3a3125,0x1a150e},{0xfbfbfd,0xececf1}};
    const auto& tint=tints[static_cast<int>(s.tint)];
    if(s.tint!=DockSettings::Tint::Smoke) {
        p.tint_top=theme::rgb(tint.top,p.tint_top.a);p.tint_bottom=theme::rgb(tint.bottom,p.tint_bottom.a);
        p.solid_top=theme::rgb(tint.top,p.solid_top.a);p.solid_bottom=theme::rgb(tint.bottom,p.solid_bottom.a);
        p.tooltip_top=theme::rgb(tint.top,p.tooltip_top.a);p.tooltip_bottom=theme::rgb(tint.bottom,p.tooltip_bottom.a);
    }
    // Strength: 55 is the designed default; 0 nearly clear, 100 nearly opaque.
    const float k=static_cast<float>(s.glass-55)/45.0f;
    const auto scale=[&](float a){return std::clamp(k>=0?a+(.95f-a)*k:a*(1+k*.75f),.05f,.97f);};
    p.tint_top.a=scale(p.tint_top.a);p.tint_bottom.a=scale(p.tint_bottom.a);
    return p;
}
}
