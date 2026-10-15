#pragma once
#include <windows.h>
#include <memory>

namespace minima {
inline constexpr UINT updates_changed_message=WM_APP+23;
struct UpdateStatus {
    bool known=false;   // A status has been read at least once.
    bool restart=false; // Installed updates wait for a restart.
    long pending=0;     // Updates found by Windows' last scan, not yet installed.
};
// Windows Update state for quick settings, read on a background thread from
// the Update Agent's cached results (no network search of its own).
class UpdateMonitor {
public:
    UpdateMonitor();
    ~UpdateMonitor();
    UpdateMonitor(const UpdateMonitor&)=delete;
    UpdateMonitor& operator=(const UpdateMonitor&)=delete;
    void start(HWND notify);
    void stop();
    void refresh(bool force=false); // Throttled to every few minutes unless forced.
    UpdateStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
