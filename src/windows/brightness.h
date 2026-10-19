#pragma once
#include <windows.h>
#include <memory>

namespace minima {
// Posted to the notify window when availability or the level changes.
inline constexpr UINT brightness_changed_message=WM_APP+33;
// Brightness of the built-in panel through WMI (root\wmi WmiMonitorBrightness,
// WmiSetBrightness). External monitors don't expose it, so on most desktops
// available() stays false and quick settings shows no brightness row.
// WMI calls can take tens of milliseconds, so they all run on one worker
// thread; the GUI thread only reads the cached values.
class Brightness {
public:
    Brightness();
    ~Brightness();
    Brightness(const Brightness&)=delete;
    Brightness& operator=(const Brightness&)=delete;
    void start(HWND notify);
    void stop();
    bool available() const;
    int level() const;     // 0-100.
    void refresh();        // Re-read (the brightness keys change it behind our back).
    // Latest value wins: while dragging, at most one write per 100 ms reaches WMI.
    void set(int percent);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
