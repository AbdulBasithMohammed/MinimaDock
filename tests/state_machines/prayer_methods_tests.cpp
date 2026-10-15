#include "taskbar/prayer_methods.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace minima;
namespace {
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int at(int h,int m){return h*60+m;}
std::string clock(int minutes){if(minutes<0)return "-";char b[16];std::snprintf(b,sizeof(b),"%02d:%02d",minutes/60,minutes%60);return b;}
void show(const char* label,const std::optional<PrayerDay>& d) {
    std::cout<<"  "<<label<<": ";
    if(!d){std::cout<<"no\n";return;}
    for(const auto s:{Salah::Fajr,Salah::Sunrise,Salah::Zuhr,Salah::Asr,Salah::Maghrib,Salah::Isha}) {
        const auto& t=(*d)[s];
        std::cout<<clock(s==Salah::Sunrise?t.begin:t.adhan);if(t.iqama>=0)std::cout<<"/"<<clock(t.iqama);std::cout<<' ';
    }
    std::cout<<'\n';
}
}
int main(int argc,char** argv) {
    // Report mode: what each method reads from saved pages.
    if(argc>1) {
        for(int i=1;i<argc;++i) {
            std::ifstream file(argv[i],std::ios::binary);std::stringstream buffer;buffer<<file.rdbuf();const auto page=buffer.str();
            std::cout<<argv[i]<<'\n';
            show("galaxystream",parse_galaxystream(page));show("mawaqit",parse_mawaqit(page,10,15));
            show("athan+ page",parse_listing(page,false));show("table",parse_listing(page,true));show("text",parse_listing(page,false));
        }
        return 0;
    }
    try {
        // Rows with Begins / Adhan / Iqama (GalaxyStream's own table layout).
        const std::string rows=R"(<table><tr><td>Salah</td><td>Begins</td><td>Adhan</td><td>Iqama</td></tr>
            <tr><td>Fajr</td><td>6:03 AM</td><td>6:25 AM</td><td>6:40 AM</td></tr><tr><td>Sunrise</td><td>7:32 AM</td></tr>
            <tr><td>Zuhr</td><td>1:05 PM</td><td>1:30 PM</td><td>1:45 PM</td></tr><tr><td>Asr</td><td>4:15 PM</td><td>5:00 PM</td><td>5:10 PM</td></tr>
            <tr><td>Maghrib</td><td>6:37 PM</td><td>6:37 PM</td><td>6:40 PM</td></tr><tr><td>Isha</td><td>7:55 PM</td><td>8:30 PM</td><td>8:45 PM</td></tr></table>)";
        auto d=parse_listing(rows,true);
        expect(d&&(*d)[Salah::Fajr].begin==at(6,3)&&(*d)[Salah::Fajr].adhan==at(6,25)&&(*d)[Salah::Fajr].iqama==at(6,40),"Rows: begins, adhan, iqama");
        expect((*d)[Salah::Sunrise].begin==at(7,32)&&(*d)[Salah::Isha].iqama==at(20,45),"Rows: sunrise and Isha");
        // 12-hour times without AM/PM, Dhuhr spelling, two columns (Adhan, Iqamah).
        const std::string bare=R"(<div>Prayer Times</div><ul><li>Fajr 5:45 6:15</li><li>Dhuhr 1:20 1:45</li><li>Asr 4:30 5:00</li><li>Maghrib 6:41 6:46</li><li>Isha 8:00 8:15</li></ul>)";
        d=parse_listing(bare,false);
        expect(d&&(*d)[Salah::Zuhr].adhan==at(13,20)&&(*d)[Salah::Zuhr].iqama==at(13,45)&&(*d)[Salah::Fajr].adhan==at(5,45)&&(*d)[Salah::Isha].iqama==at(20,15),"Text: times without AM/PM by prayer");
        expect(!parse_listing(bare,true),"Table method ignores text outside tables");
        // Columns: a header row of names, then Adhan and Iqama rows.
        const std::string cols=R"(<table><tr><th></th><th>Fajr</th><th>Sunrise</th><th>Dhuhr</th><th>Asr</th><th>Maghrib</th><th>Isha</th></tr>
            <tr><td>Begins</td><td>05:31</td><td>07:02</td><td>12:47</td><td>15:51</td><td>18:28</td><td>19:57</td></tr>
            <tr><td>Jamaat</td><td>06:00</td><td>13:15</td><td>16:15</td><td>18:33</td><td>20:30</td></tr></table>)";
        d=parse_listing(cols,true);
        expect(d&&(*d)[Salah::Sunrise].begin==at(7,2)&&(*d)[Salah::Asr].adhan==at(15,51)&&(*d)[Salah::Asr].iqama==at(16,15)&&(*d)[Salah::Isha].iqama==at(20,30),"Columns: names then rows of times");
        // Mawaqit: times array, shuruq, monthly Iqama offsets.
        const std::string mawaqit=R"(..."shuruq":"08:11","times":["06:36","13:41","16:27","19:04","20:31"],"jumua":"13:50","iqamaCalendar":[{"1":["+8","+8","+8","+0","+8"]},{"1":["+1","+1","+1","+1","+1"]},{},{},{},{},{},{},{},{"15":["+10","14:00","+5","+3","+8"]},{},{}]...)";
        d=parse_mawaqit(mawaqit,10,15);
        expect(d&&(*d)[Salah::Fajr].adhan==at(6,36)&&(*d)[Salah::Fajr].iqama==at(6,46)&&(*d)[Salah::Zuhr].iqama==at(14,0)&&(*d)[Salah::Sunrise].begin==at(8,11)&&(*d)[Salah::Jumah1].adhan==at(13,50),"Mawaqit: offsets and fixed Iqama");
        // Sanity rules.
        PrayerDay bad=*d;bad[Salah::Asr].adhan=at(12,0);expect(!sensible(bad),"Out-of-order prayers are rejected");
        bad=*d;bad[Salah::Isha].iqama=at(19,0);expect(!sensible(bad),"Iqama before Adhan is rejected");
        // Calculation (Toronto, 2026-10-15, UTC-4): sunrise, noon and sunset are method-independent.
        const auto c=calculate_day(2026,10,15,43.6532,-79.3832,-4);
        // The Islamic Society of Toronto publishes sunrise 7:33 and Maghrib 6:35 PM that day.
        expect(std::abs(c[Salah::Sunrise].begin-at(7,33))<=2&&std::abs(c[Salah::Maghrib].adhan-at(18,35))<=2&&std::abs(c[Salah::Zuhr].adhan-at(13,3))<=2,"Calculated sunrise, noon and sunset");
        expect(sensible(c)&&c[Salah::Fajr].iqama<0,"Calculated day is sensible and has no Iqama");
        std::cout<<"Prayer methods passed: rows (begins/adhan/iqama), bare 12-hour times, columns, Mawaqit offsets, sanity rules, calculated times.\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
