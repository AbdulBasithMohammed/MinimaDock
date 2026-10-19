#include "taskbar/prayer_model.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace minima;
namespace {
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
// Arrays in the format a GalaxyStream widget publishes.
constexpr std::string_view sample=R"(var beginTime = new Array();beginTime[0] = "6:13<span class='ampm'>AM</span>";beginTime[1] = "7:32<span class='ampm'>AM</span>";beginTime[2] = "12:58<span class='ampm'>PM</span> - 1:03<span class='ampm'>PM</span>";beginTime[3] = "1:03<span class='ampm'>PM</span>";beginTime[4] = "4:50<span class='ampm'>PM</span>";beginTime[5] = "6:37<span class='ampm'>PM</span>";beginTime[6] = "7:47<span class='ampm'>PM</span>";beginTime[7] = "1:03<span class='ampm'>PM</span>";beginTime[8] = "1:03<span class='ampm'>PM</span>";beginTime[9] = "1:03<span class='ampm'>PM</span>";beginTime[10] = "";
var adhanTime = new Array();adhanTime[0] = "6:25<span class='ampm'>AM</span>";adhanTime[1] = " ";adhanTime[2] = " ";adhanTime[3] = "1:30<span class='ampm'>PM</span>";adhanTime[4] = "5:00<span class='ampm'>PM</span>";adhanTime[5] = "6:37<span class='ampm'>PM</span>";adhanTime[6] = "8:30<span class='ampm'>PM</span>";adhanTime[7] = "1:30<span class='ampm'>PM</span>";adhanTime[8] = "2:45<span class='ampm'>PM</span>";adhanTime[9] = "3:45<span class='ampm'>PM</span>";adhanTime[10] = "";
var iqamaTime = new Array();iqamaTime[0] = "6:40<span class='ampm'>AM</span>";iqamaTime[1] = " ";iqamaTime[2] = " ";iqamaTime[3] = "1:45<span class='ampm'>PM</span>";iqamaTime[4] = "5:10<span class='ampm'>PM</span>";iqamaTime[5] = "6:40<span class='ampm'>PM</span>";iqamaTime[6] = "8:45<span class='ampm'>PM</span>";iqamaTime[7] = "1:45<span class='ampm'>PM</span>";iqamaTime[8] = "2:45<span class='ampm'>PM</span>";iqamaTime[9] = "3:45<span class='ampm'>PM</span>";iqamaTime[10] = "";)";
constexpr int at(int h,int m){return h*60+m;}
}
int main() {
    try {
        expect(parse_clock("6:13<span class='ampm'>AM</span>")==at(6,13),"AM time");
        expect(parse_clock("12:58<span class='ampm'>PM</span> - 1:03<span class='ampm'>PM</span>")==at(12,58),"Range start keeps its own marker");
        expect(parse_clock("12:05 AM")==at(0,5)&&parse_clock("12:05 PM")==at(12,5),"Midnight/noon");
        expect(parse_clock(" ")<0&&parse_clock("")<0&&parse_clock("7:5 PM")<0,"Blank/malformed rejected");
        const auto day=parse_galaxystream(sample);expect(day.has_value(),"Sample parses");
        const auto& d=*day;
        expect(d[Salah::Fajr].adhan==at(6,25)&&d[Salah::Fajr].iqama==at(6,40),"Fajr");
        expect(d[Salah::Sunrise].begin==at(7,32)&&d[Salah::Sunrise].adhan<0,"Sunrise is a single time");
        expect(d[Salah::Zuhr].adhan==at(13,30)&&d[Salah::Zuhr].iqama==at(13,45),"Zuhr (not Zawal)");
        expect(d[Salah::Asr].adhan==at(17,0)&&d[Salah::Maghrib].iqama==at(18,40)&&d[Salah::Isha].iqama==at(20,45),"Asr/Maghrib/Isha");
        expect(d[Salah::Jumah2].adhan==at(14,45),"Jumu'ah slots");
        expect(!parse_galaxystream("var nothing;").has_value(),"Unrelated script rejected");
        auto next=next_prayer(d,at(16,0),false);expect(next.salah==Salah::Asr&&!next.waiting&&!next.tomorrow,"Before Asr");
        next=next_prayer(d,at(17,5),false);expect(next.salah==Salah::Asr&&next.waiting,"Between Asr Adhan and Iqama");
        next=next_prayer(d,at(17,10),false);expect(next.salah==Salah::Maghrib,"Iqama passed");
        next=next_prayer(d,at(21,0),false);expect(next.salah==Salah::Fajr&&next.tomorrow,"After Isha: tomorrow's Fajr");
        next=next_prayer(d,at(12,0),true);expect(next.salah==Salah::Jumah1,"Friday: Jumu'ah replaces Zuhr");
        const auto events=prayer_events(d,false,10);
        const auto has=[&](PrayerEvent::Kind k,Salah s,int minute){return std::any_of(events.begin(),events.end(),[&](const PrayerEvent& e){return e.kind==k&&e.salah==s&&e.minute==minute;});};
        expect(has(PrayerEvent::Kind::Adhan,Salah::Asr,at(17,0))&&has(PrayerEvent::Kind::Iqama,Salah::Isha,at(20,35)),"Adhan and Iqama reminders");
        expect(!has(PrayerEvent::Kind::Iqama,Salah::Maghrib,at(18,30)),"No Iqama reminder before a short Maghrib gap");
        expect(has(PrayerEvent::Kind::Sunrise,Salah::Sunrise,at(7,17)),"Sunrise warning");
        std::vector<int> keys;for(const auto& e:events)keys.push_back(e.key());std::sort(keys.begin(),keys.end());
        expect(std::adjacent_find(keys.begin(),keys.end())==keys.end(),"Event keys are unique");
        std::cout<<"Prayer times passed: GalaxyStream parsing (AM/PM, ranges, blanks), next prayer incl. Iqama wait, after Isha and Jumu'ah, reminder events.\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
