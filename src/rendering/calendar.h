#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace minima {
inline constexpr UINT calendar_closed_message=WM_APP+13;
// Today's prayer times shown under the month (preformatted by the dock).
struct PrayerRow {std::wstring name,adhan,iqama;int state=2;}; // 0 passed, 1 next, 2 later
struct PrayerView {bool visible=false;std::wstring status,website;std::vector<PrayerRow> rows;};
class CalendarPopup {
public:
    CalendarPopup();
    ~CalendarPopup();
    CalendarPopup(const CalendarPopup&)=delete;
    CalendarPopup& operator=(const CalendarPopup&)=delete;
    void open(HWND owner,RECT anchor,bool reduced);
    void close(bool animated=true);
    void preferences(bool reduced);
    // Applies at the next open when it would change the card's height.
    void prayers(const PrayerView& view);
    HWND window() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int save_calendar_snapshot(const std::wstring& path,bool moving=false,const PrayerView* prayers=nullptr);
}
