#pragma once
#include <windows.h>
#include <memory>

namespace minima {
// Posted to the dock when the Windows key is pressed and released on its own.
inline constexpr UINT windows_key_message=WM_APP+29;
// The Windows key opens the MinimaDock launcher instead of Start.
// A low-level keyboard hook on its own thread watches the key. Shortcuts that use
// it (Win+E, Win+L, Win+Shift+S...) pass through untouched. When the key is tapped
// alone, an unassigned key (vkE8) is sent before its release so Start stays
// closed, and the dock is told to open the launcher. Full-screen apps and games
// keep Windows' behaviour. Ctrl+Esc still opens Start.
class WindowsKey {
public:
    WindowsKey();
    ~WindowsKey();
    WindowsKey(const WindowsKey&)=delete;
    WindowsKey& operator=(const WindowsKey&)=delete;
    void start(HWND notify);
    void stop();
    void enable(bool on);
    bool enabled() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// HKCU\Software\MinimaDock\Launcher\WindowsKey (DWORD, default on).
bool load_windows_key_setting();
void save_windows_key_setting(bool on);
bool open_start_menu(); // Ctrl+Esc.
}
