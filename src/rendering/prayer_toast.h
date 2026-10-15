#pragma once
#include "rendering/theme.h"
#include <windows.h>
#include <memory>
#include <string>

namespace minima {
// What the prayer card says. `end_tick` (GetTickCount64) is the moment the
// countdown reaches (Iqama, sunrise); 0 shows no countdown and closes after a minute.
struct PrayerToastInfo {
    std::wstring title;      // "Asr · Adhan"
    std::wstring subtitle;   // "Iqama at 5:10 PM"
    std::wstring pill;       // "Asr" (the shrunken pill's label)
    std::wstring done;       // "Iqama now" (shown when the countdown ends)
    ULONGLONG start_tick=0,end_tick=0;
    bool chime=true;         // A short chime as the card appears.
    int sound=0;             // 0 Calendar, 1 soft bell (Customize > Prayer times > Chime).
};
// A small always-on-top card at the top centre of the screen in use (also over
// games in windowed or borderless full screen). It never takes focus. After
// about 20 seconds it shrinks to a countdown pill; hovering expands it again,
// clicking dismisses it, and it fades out shortly after the countdown ends.
class PrayerToast {
public:
    PrayerToast();
    ~PrayerToast();
    PrayerToast(const PrayerToast&)=delete;
    PrayerToast& operator=(const PrayerToast&)=delete;
    void show(const PrayerToastInfo& info,const theme::Palette& palette,bool reduced_motion);
    void close();
    bool visible() const;
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};
int save_prayer_toast_snapshot(const std::wstring& path,bool pill); // --snapshot-prayer-toast[-pill]
int save_prayer_toast_compare(const std::wstring& path);           // --snapshot-prayer-toast-compare
int run_prayer_toast_demo();                                          // --prayer-toast-demo
}
