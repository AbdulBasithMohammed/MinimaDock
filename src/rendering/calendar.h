#pragma once
#include <windows.h>
#include <memory>
#include <string>

namespace minima {
inline constexpr UINT calendar_closed_message=WM_APP+13;
class CalendarPopup {
public:
    CalendarPopup();
    ~CalendarPopup();
    CalendarPopup(const CalendarPopup&)=delete;
    CalendarPopup& operator=(const CalendarPopup&)=delete;
    void open(HWND owner,RECT anchor,bool reduced);
    void close(bool animated=true);
    void preferences(bool reduced);
    HWND window() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int save_calendar_snapshot(const std::wstring& path,bool moving=false);
}
