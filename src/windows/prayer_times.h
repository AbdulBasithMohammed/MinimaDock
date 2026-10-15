#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include "taskbar/prayer_model.h"

namespace minima {
// Posted to the notify window after every download attempt.
inline constexpr UINT prayer_times_message=WM_APP+25;
// HKCU\Software\MinimaDock\Prayer.
struct PrayerSettings {
    std::wstring website=L"https://www.islamicsocietyoftoronto.com/";
    bool clock=true;      // Next prayer on the dock clock's second line.
    bool reminders=true;  // Chime and card at Adhan, before Iqama and sunrise.
    int iqama_lead=10;    // Minutes before Iqama.
};
PrayerSettings load_prayer_settings();
void save_prayer_settings(const PrayerSettings& settings);
struct PrayerSnapshot {
    bool have=false;        // `day` holds published times.
    PrayerDay day;          // For day.year/month/day (may be an earlier day when offline).
    std::wstring updated;   // Local time of the last successful download.
    std::wstring error;     // Why the last attempt failed.
};
// Downloads the masjid's daily times (its website's GalaxyStream widget) on a
// worker thread, caches them under %LOCALAPPDATA%\MinimaDock, and refreshes at
// most every three hours, after midnight, and 15 minutes after a failure.
// Contacts only the configured website and the widget host it names.
class PrayerService {
public:
    PrayerService();
    ~PrayerService();
    PrayerService(const PrayerService&)=delete;
    PrayerService& operator=(const PrayerService&)=delete;
    void start(HWND notify,std::wstring website);
    void stop();
    void refresh(bool force=false); // Cheap when nothing is due.
    PrayerSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
