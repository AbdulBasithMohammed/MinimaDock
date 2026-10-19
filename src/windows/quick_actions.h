#pragma once
#include <windows.h>
#include <string>

namespace minima {
// Reads and writes behind the extra quick settings tiles. Reads are cheap
// (registry, process list, small Win32 queries) and run on the GUI thread when
// the panel opens. Anything that can stall (changing the display topology,
// broadcasting a theme change) runs on a detached thread.
// Tiles without a documented Windows API (Night light, Do not disturb, Nearby
// sharing, ...) only open their Settings page; see docs/decisions.md.

// Win+P modes, in Windows' order.
enum class DisplayMode {Unknown,Internal,Clone,Extend,External};
struct DisplayState {
    int displays=0;                       // Connected displays, active or not.
    DisplayMode mode=DisplayMode::Unknown;
};
DisplayState display_state();
// Applies a mode on a detached thread (the dock moves through WM_DISPLAYCHANGE
// as usual). If Windows refuses, Display settings opens instead.
void set_display_mode(DisplayMode mode);

// Apps theme is dark (HKCU Themes\Personalize). Writing it is set_dark_mode in
// launcher_index.h; the tile re-reads this afterwards.
bool dark_mode();

bool process_running(const wchar_t* exe); // Case-insensitive image name.

// Installed keyboard layouts, and the short language name ("ENG") of the one
// `window`'s thread uses (layouts can be per app).
int keyboard_layouts();
std::wstring keyboard_label(HWND window);
// Asks `window` to switch to the next layout; returns the new layout's label.
std::wstring next_keyboard_layout(HWND window);

bool energy_saver_on();  // Battery/Energy saver (GetSystemPowerStatus).
bool vpn_connected();    // Windows (RAS) VPNs only; other clients are invisible.
bool has_rotation_sensor();

// ShellExecute of an ms-settings: page; falls back to the Settings home page
// when this Windows build lacks the page.
void open_settings(const wchar_t* uri);
}
