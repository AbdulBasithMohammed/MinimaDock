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
    std::wstring website;  // Empty until the user adds their masjid (no prayer times shown).
    bool clock=true;      // Next prayer on the dock clock's second line.
    bool reminders=true;  // Chime and card at Adhan, before Iqama and sunrise.
    int iqama_lead=10;    // Minutes before Iqama (0 = off).
    int chime=0;          // 0 Calendar, 1 Soft bell, 2 none.
    bool sunrise=true;    // Reminder 15 minutes before Fajr time ends.
    int method=0;         // 0 automatic; otherwise one PrayerMethod (see prayer_model.h).
    bool calculate=true;  // Calculated Adhan times when the website cannot be read.
    double latitude=43.6532,longitude=-79.3832; // For calculated times (default Toronto).
};
PrayerSettings load_prayer_settings();
void save_prayer_settings(const PrayerSettings& settings);
struct PrayerSnapshot {
    bool have=false;        // `day` holds published times.
    PrayerDay day;          // For day.year/month/day (may be an earlier day when offline).
    std::wstring updated;   // Local time of the last successful download.
    std::wstring error;     // Why the last attempt failed.
    std::wstring method;    // How today's times were read ("GalaxyStream widget", "Calculated"...).
};
// Reads the masjid's daily times on a worker thread (GalaxyStream or Athan+
// widgets, Mawaqit, a prayer table or the page text; see prayer_methods.h),
// caches them under %LOCALAPPDATA%\MinimaDock and refreshes after midnight,
// every three hours and 15 minutes after a failure. Without published times it
// can calculate Adhan times for the configured location. Contacts only the
// configured website and the widget hosts it names.
class PrayerService {
public:
    PrayerService();
    ~PrayerService();
    PrayerService(const PrayerService&)=delete;
    PrayerService& operator=(const PrayerService&)=delete;
    void start(HWND notify,const PrayerSettings& settings);
    void stop();
    void refresh(bool force=false); // Cheap when nothing is due.
    PrayerSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
