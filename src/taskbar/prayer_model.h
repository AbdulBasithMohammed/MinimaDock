#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace minima {
// A masjid's published times for one day, in minutes after local midnight
// (-1 when not published).
struct PrayerTime {int begin=-1,adhan=-1,iqama=-1;};
enum class Salah {Fajr,Sunrise,Zuhr,Asr,Maghrib,Isha,Jumah1,Jumah2,Jumah3,Count};
struct PrayerDay {
    int year=0,month=0,day=0;
    std::array<PrayerTime,static_cast<std::size_t>(Salah::Count)> times{};
    const PrayerTime& operator[](Salah s) const{return times[static_cast<std::size_t>(s)];}
    PrayerTime& operator[](Salah s){return times[static_cast<std::size_t>(s)];}
    bool valid() const {
        for(const auto s:{Salah::Fajr,Salah::Zuhr,Salah::Asr,Salah::Maghrib,Salah::Isha})if((*this)[s].adhan<0)return false;
        return true;
    }
};
inline const wchar_t* salah_name(Salah s) {
    switch(s){case Salah::Fajr:return L"Fajr";case Salah::Sunrise:return L"Sunrise";case Salah::Zuhr:return L"Zuhr";case Salah::Asr:return L"Asr";
    case Salah::Maghrib:return L"Maghrib";case Salah::Isha:return L"Isha";default:return L"Jumu'ah";}
}
// "6:13<span class='ampm'>AM</span>" -> minutes after midnight.
inline int parse_clock(std::string_view text) {
    std::size_t i=0;while(i<text.size()&&(text[i]<'0'||text[i]>'9'))++i;
    int hour=0,minute=0,digits=0;
    while(i<text.size()&&text[i]>='0'&&text[i]<='9'&&digits<2){hour=hour*10+(text[i]-'0');++i;++digits;}
    if(!digits||i>=text.size()||text[i]!=':')return -1;++i;digits=0;
    while(i<text.size()&&text[i]>='0'&&text[i]<='9'&&digits<2){minute=minute*10+(text[i]-'0');++i;++digits;}
    if(digits!=2||hour>23||minute>59)return -1;
    // The marker follows before the next number (a range "12:58 - 1:03" stops at '-').
    std::size_t end=i;while(end<text.size()&&!(text[end]>='0'&&text[end]<='9')&&text[end]!='-')++end;
    // Ignore markup: the widget wraps the marker in <span class='ampm'>.
    std::string tail;bool tag=false;
    for(auto k=i;k<end;++k){const char c=text[k];if(c=='<')tag=true;else if(c=='>')tag=false;else if(!tag&&c!=' ')tail+=static_cast<char>(c>='a'&&c<='z'?c-32:c);}
    const bool pm=tail.find("PM")!=std::string::npos,am=tail.find("AM")!=std::string::npos;
    if(pm&&hour<12)hour+=12;else if(am&&hour==12)hour=0;
    return hour*60+minute;
}
// GalaxyStream's PrayerTimesJS widget (used by many masjid sites):
// beginTime[i] / adhanTime[i] / iqamaTime[i] arrays for today, where i is
// Fajr, Sunrise, Zawal, Zuhr, Asr, Maghrib, Isha, Jumah 1-3.
inline std::optional<PrayerDay> parse_galaxystream(std::string_view script) {
    constexpr int order[]={0,1,-1,2,3,4,5,6,7,8};
    PrayerDay day;
    const auto read=[&](std::string_view array,std::size_t index)->int {
        const std::string key=std::string(array)+"["+std::to_string(index)+"] = \"";
        const auto at=script.find(key);if(at==std::string_view::npos)return -1;
        const auto start=at+key.size();const auto end=script.find('"',start);if(end==std::string_view::npos)return -1;
        return parse_clock(script.substr(start,end-start));
    };
    for(std::size_t i=0;i<std::size(order);++i) {
        if(order[i]<0)continue;
        auto& t=day.times[static_cast<std::size_t>(order[i])];
        t.begin=read("beginTime",i);t.adhan=read("adhanTime",i);t.iqama=read("iqamaTime",i);
    }
    // Sunrise is a single time.
    auto& sunrise=day[Salah::Sunrise];if(sunrise.begin>=0)sunrise.adhan=sunrise.iqama=-1;
    if(!day.valid())return std::nullopt;
    return day;
}
// The congregational prayers of a day; Jumu'ah replaces Zuhr on Fridays.
inline std::array<Salah,5> daily_salah(const PrayerDay& day,bool friday) {
    return {Salah::Fajr,friday&&day[Salah::Jumah1].adhan>=0?Salah::Jumah1:Salah::Zuhr,Salah::Asr,Salah::Maghrib,Salah::Isha};
}
struct NextPrayer {
    Salah salah=Salah::Fajr;
    PrayerTime time;
    bool tomorrow=false; // After Isha: tomorrow's Fajr (today's times until refreshed).
    bool waiting=false;  // Adhan has been called; iqama is still ahead.
};
inline NextPrayer next_prayer(const PrayerDay& day,int now,bool friday) {
    for(const auto s:daily_salah(day,friday)) {
        const auto& t=day[s];
        if(now<t.adhan)return {s,t,false,false};
        if(t.iqama>=0&&now<t.iqama)return {s,t,false,true};
    }
    return {Salah::Fajr,day[Salah::Fajr],true,false};
}
// Moments that raise a reminder: each Adhan, a reminder before each Iqama, and
// a warning before sunrise ends Fajr time.
struct PrayerEvent {
    enum class Kind {Adhan,Iqama,Sunrise};
    Kind kind;Salah salah;int minute;
    int key() const{return static_cast<int>(salah)*10+static_cast<int>(kind);}
};
inline std::vector<PrayerEvent> prayer_events(const PrayerDay& day,bool friday,int iqama_lead,int sunrise_lead=15) {
    std::vector<PrayerEvent> events;
    if(const auto sunrise=day[Salah::Sunrise].begin;sunrise>=sunrise_lead)events.push_back({PrayerEvent::Kind::Sunrise,Salah::Sunrise,sunrise-sunrise_lead});
    for(const auto s:daily_salah(day,friday)) {
        const auto& t=day[s];
        events.push_back({PrayerEvent::Kind::Adhan,s,t.adhan});
        // Only when the reminder falls after the Adhan (not for a short Maghrib gap).
        if(iqama_lead>0&&t.iqama>=0&&t.iqama-iqama_lead>t.adhan)events.push_back({PrayerEvent::Kind::Iqama,s,t.iqama-iqama_lead});
    }
    return events;
}
}
