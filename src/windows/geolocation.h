#pragma once
#include "taskbar/geo_json.h"
#include <atomic>
#include <string_view>
#include <vector>

// Where this PC is, and what a typed city is called. Every function blocks and
// is only called from worker threads.
//
// Privacy: Windows' location service is asked only by "Locate me" (and then once
// a day while following the device). Nominatim (OpenStreetMap) is contacted only
// when you search a city, or after a located move of more than 5 km; reverse
// lookups send coordinates rounded to about 1 km. Nothing here is logged.
namespace minima {
enum class LocateStatus {Ok,Denied,Disabled,Unavailable,Cancelled};
struct GeoFix {
    LocateStatus status=LocateStatus::Unavailable;
    double latitude=0,longitude=0;
    double accuracy_m=0; // Radius in metres (Wi-Fi or IP positioning can be kilometres).
};
// One Windows.Devices.Geolocation fix, at most about 10 s. A fix up to 10 minutes
// old is accepted. Denied: this app or desktop apps may not use location.
// Disabled: location is off for the whole PC. Both are fixed in
// ms-settings:privacy-location.
GeoFix locate_device(const std::atomic<bool>* cancel=nullptr);

enum class PlaceError {None,Offline,Busy,Failed};
struct PlaceLookup {
    PlaceError error=PlaceError::None;
    std::vector<Place> places; // Empty without an error: nothing matched.
};
// Nominatim search (up to 5 places) and reverse lookup (at most one place).
// Requests are spaced at least one second apart across the process, as the
// usage policy asks, and answers are kept for identical requests.
PlaceLookup search_places(std::wstring_view query);
PlaceLookup reverse_place(double latitude,double longitude);
}
