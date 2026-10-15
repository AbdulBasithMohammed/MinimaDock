#pragma once
#include "windows/inventory.h"
#include <windows.h>
#include <memory>
#include <vector>

namespace minima {
inline constexpr UINT tray_ready_message=WM_APP+15;
struct TrayIcon {unsigned long long key=0;NativeIcon icon;std::wstring title;};
// Own notification-area receiver. Temporary trials mirror icon notifications
// to Explorer so reverting preserves the original tray.
class TrayPopup {
public:
    TrayPopup();
    ~TrayPopup();
    TrayPopup(const TrayPopup&)=delete;
    TrayPopup& operator=(const TrayPopup&)=delete;
    void prepare(HWND notify);
    void stop();
    bool ready() const;
    bool available() const;
    HRESULT error() const;
    bool open(RECT anchor,UINT dpi);
    void close();
    bool tick();
    bool owns(HWND window) const;
    HWND window() const;
    std::vector<TrayIcon> snapshot() const;
    bool activate(unsigned long long key,bool context,POINT point,bool double_click=false,bool keyboard=false);
    HWND receiver() const;
    void set_bounds(unsigned long long key,RECT bounds);
    bool holds_menu() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
