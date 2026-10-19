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
        // Calculation, on synthetic coordinates. Reference times come from the
        // AlAdhan API (an independent implementation) for the same inputs.
        const auto near=[](const PrayerDay& d,std::initializer_list<int> want,const char* message) {
            const Salah order[]={Salah::Fajr,Salah::Sunrise,Salah::Zuhr,Salah::Asr,Salah::Maghrib,Salah::Isha};
            size_t i=0;
            for(const int w:want) {
                const auto& t=d[order[i]];const int got=order[i]==Salah::Sunrise?t.begin:t.adhan;++i;
                int diff=std::abs(got-w);diff=std::min(diff,1440-diff);
                if(diff>2){show("calculated",d);throw std::runtime_error(message);}
            }
        };
        const auto k=calculate_day(2026,10,15,30,70,calc_params(CalcMethod::Karachi,2),5.0);
        near(k,{at(5,2),at(6,22),at(12,6),at(16,11),at(17,50),at(19,9)},"Karachi angles with Hanafi Asr");
        expect(sensible(k)&&k[Salah::Fajr].iqama<0,"Calculated day is sensible and has no Iqama");
        near(calculate_day(2026,10,15,40,-75,calc_params(CalcMethod::ISNA),-4.0),{at(5,56),at(7,11),at(12,46),at(15,52),at(18,20),at(19,35)},"ISNA with Standard Asr");
        const auto q=calculate_day(2026,10,15,25,45,calc_params(CalcMethod::Makkah),3.0);
        near(q,{at(4,40),at(5,58),at(11,46),at(15,6),at(17,33),at(19,3)},"Umm al-Qura: Isha 90 minutes after Maghrib");
        auto ramadan=calc_params(CalcMethod::Makkah);ramadan.isha_minutes=120;
        expect(calculate_day(2026,10,15,25,45,ramadan,3.0)[Salah::Isha].adhan==q[Salah::Maghrib].adhan+120,"Umm al-Qura: 120 minutes in Ramadan");
        near(calculate_day(2026,10,15,35,50,calc_params(CalcMethod::Tehran),3.0),{at(4,23),at(5,46),at(11,26),at(14,39),at(17,23),at(18,10)},"Tehran: Maghrib by angle");
        // High latitude in June: twilight all night, so Fajr and Isha follow the angle-based rule.
        bool approximate=true;
        near(calculate_day(2026,6,21,60,10,calc_params(CalcMethod::MWL),2.0,&approximate),{at(2,23),at(3,56),at(13,22),at(18,4),at(22,48),at(0,15)},"Angle-based rule at high latitude");
        expect(!approximate,"Sun rises and sets at 60 degrees");
        // Polar day and night: no sunrise or sunset, calculated at 65 degrees, never NaN.
        for(const int month:{6,12}) for(const double latitude:{70.0,-70.0,89.9}) {
            const auto p=calculate_day(2026,month,21,latitude,10,calc_params(CalcMethod::MWL),1.0,&approximate);
            expect(approximate,"Polar days are approximate");
            for(const auto& t:p.times)expect(t.begin>=-1&&t.begin<1440&&t.adhan>=-1&&t.adhan<1440,"Polar times stay on the clock");
            expect(p.valid(),"Polar day still has all five prayers");
        }
        // Daylight saving: each instant gets the offset in force at that moment.
        const auto shifted=calculate_day(2026,10,15,30,70,calc_params(CalcMethod::Karachi,2),[](double utc){
            const double offset=utc<6?5:4; // The clocks go back at 06:00 UTC.
            return static_cast<int>(std::lround((utc+offset)*60));
        });
        expect(shifted[Salah::Fajr].adhan==k[Salah::Fajr].adhan&&shifted[Salah::Maghrib].adhan==k[Salah::Maghrib].adhan-60,"Offset applied per instant");
        // Automatic method and Asr by country.
        expect(auto_method("PK")==CalcMethod::Karachi&&auto_method("us")==CalcMethod::ISNA&&auto_method("sa")==CalcMethod::Makkah&&auto_method("ir")==CalcMethod::Tehran,"Method by country");
        expect(auto_method("")==CalcMethod::MWL&&auto_method("zz")==CalcMethod::MWL&&auto_method("x")==CalcMethod::MWL,"Unknown country: Muslim World League");
        expect(auto_asr("in")==2&&auto_asr("TR")==2&&auto_asr("eg")==1&&auto_asr("")==1,"Asr by country");
        std::cout<<"Prayer methods passed: rows (begins/adhan/iqama), bare 12-hour times, columns, Mawaqit offsets, sanity rules, calculation methods, high latitude, polar days, daylight saving, method by country.\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
