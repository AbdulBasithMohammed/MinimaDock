#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include "taskbar/prayer_model.h"

namespace minima {
// Posted to the notify window after every download or calculation.
inline constexpr UINT prayer_times_message=WM_APP+25;
// Where the times come from: the masjid's website, or calculated for a place.
enum class PrayerSource {Website,Location};
// Location mode: a city chosen once (no network afterwards), or this PC's
// position, read again once a day.
enum class LocateMode {City,Device};
// HKCU\Software\MinimaDock\Prayer. The location is stored only there and in
// the local cache.
struct PrayerSettings {
    PrayerSource source=PrayerSource::Location;
    LocateMode locate=LocateMode::City;
    std::wstring website;  // Masjid website (Website source; the location is its calculated fallback).
    bool clock=true;      // Next prayer on the dock clock's second line.
    bool reminders=true;  // Chime and card at Adhan, before Iqama and sunrise.
    int iqama_lead=10;    // Minutes before Iqama (0 = off).
    int chime=0;          // 0 Calendar, 1 Soft bell, 2 none.
    bool sunrise=true;    // Reminder 15 minutes before Fajr time ends.
    int method=0;         // 0 automatic; otherwise one PrayerMethod (see prayer_model.h).
    bool calculate=true;  // Calculated Adhan times when the website cannot be read.
    int calc_method=0;    // 0 automatic by country; otherwise a CalcMethod (prayer_methods.h).
    int asr=0;            // 0 automatic by country, 1 Standard, 2 Hanafi.
    // The location, unset until the user picks a city or uses Locate me.
    bool located=false;
    double latitude=0,longitude=0;
    std::wstring place;   // Display name ("Town, Region, Country"); may be empty.
    std::string country;  // ISO 3166 alpha-2 code for the automatic method; may be empty.
    int located_day=0;    // Device mode: yyyymmdd of the last fix (one a day).
    bool same_location(const PrayerSettings& o) const {
        return located==o.located&&latitude==o.latitude&&longitude==o.longitude&&place==o.place&&country==o.country&&located_day==o.located_day;
    }
};
PrayerSettings load_prayer_settings();
void save_prayer_settings(const PrayerSettings& settings);
struct PrayerSnapshot {
    bool have=false;        // `day` holds published times.
    PrayerDay day;          // For day.year/month/day (may be an earlier day when offline).
    std::wstring updated;   // Local time of the last successful download.
    std::wstring error;     // Why the last attempt failed.
    std::wstring method;    // How today's times were read ("GalaxyStream widget", "Calculated · ISNA · Standard"...).
    std::wstring note;      // Location mode: why the location could not be updated, or a caveat.
    // Device mode: the location after the latest fix. The UI saves it when it
    // differs from its settings (the worker never writes the registry).
    PrayerSettings located;
};
// Works out today's times on a worker thread and caches them under
// %LOCALAPPDATA%\MinimaDock.
// Website: reads the masjid's daily times (GalaxyStream or Athan+ widgets,
// Mawaqit, a prayer table or the page text; see prayer_methods.h), refreshing
// after midnight, every three hours and 15 minutes after a failure; without
// published times it can calculate Adhan times for the saved location. Only the
// configured website and the widget hosts it names are contacted.
// Location: calculates the times locally. A chosen city needs no network at
// all; following the device asks Windows for the position once a day and names
// it through Nominatim only after a move of more than 5 km (geolocation.h).
// start() and stop() never wait for the worker: a slow request finishes in the
// background and its result is dropped.
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
