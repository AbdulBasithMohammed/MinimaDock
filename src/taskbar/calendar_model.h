#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace minima {
struct CalendarDate {
    int year=2026;
    unsigned month=1,day=1;
    bool operator==(const CalendarDate&) const=default;
};
inline std::chrono::sys_days calendar_days(CalendarDate d) {
    return std::chrono::sys_days(std::chrono::year(d.year)/std::chrono::month(d.month)/std::chrono::day(d.day));
}
inline CalendarDate calendar_date(std::chrono::sys_days days) {
    const std::chrono::year_month_day d(days);
    return {static_cast<int>(d.year()),static_cast<unsigned>(d.month()),static_cast<unsigned>(d.day())};
}
inline bool valid_calendar_date(CalendarDate d) {
    return d.year>=1601&&d.year<=9999&&(std::chrono::year(d.year)/std::chrono::month(d.month)/std::chrono::day(d.day)).ok();
}
inline std::array<CalendarDate,42> calendar_grid(int year,unsigned month) {
    const auto first=calendar_days({year,month,1});
    const auto start=first-std::chrono::days(std::chrono::weekday(first).c_encoding());
    std::array<CalendarDate,42> result{};
    for(unsigned i=0;i<result.size();++i)result[i]=calendar_date(start+std::chrono::days(i));
    return result;
}
class CalendarModel {
public:
    explicit CalendarModel(CalendarDate today={}){reset(today);}
    void reset(CalendarDate today){if(!valid_calendar_date(today))return;today_=selected_=today;year_=today.year;month_=today.month;}
    void update_today(CalendarDate today){if(valid_calendar_date(today))today_=today;}
    bool navigate(int delta) {
        const int index=year_*12+static_cast<int>(month_)-1;
        const int target=std::clamp(index+std::clamp(delta,-12,12),1601*12,9999*12+11);
        if(target==index)return false;
        year_=target/12;month_=static_cast<unsigned>(target%12+1);return true;
    }
    bool select(CalendarDate d){if(!valid_calendar_date(d))return false;selected_=d;year_=d.year;month_=d.month;return true;}
    void move_selection(int days){select(calendar_date(calendar_days(selected_)+std::chrono::days(days)));}
    void today(){select(today_);}
    int year() const{return year_;}
    unsigned month() const{return month_;}
    CalendarDate selected() const{return selected_;}
    CalendarDate current_date() const{return today_;}
    auto grid() const{return calendar_grid(year_,month_);}
private:
    CalendarDate today_{},selected_{};
    int year_=2026;
    unsigned month_=1;
};
class CalendarSlide {
public:
    using Time=std::int64_t;
    void start(int direction,Time now,bool reduced=false){direction_=direction<0?-1:1;start_=now;progress_=reduced?1.0f:0.0f;}
    void tick(Time now){if(active())progress_=std::clamp(static_cast<float>(now-start_)/200,0.0f,1.0f);}
    void settle(){progress_=1;}
    bool active() const{return progress_<1;}
    float blend() const{return 1-(1-progress_)*(1-progress_)*(1-progress_);}
    float old_offset() const{return -direction_*320*blend();}
    float new_offset() const{return direction_*320*(1-blend());}
private:
    Time start_=0;
    float progress_=1;
    int direction_=1;
};
}
