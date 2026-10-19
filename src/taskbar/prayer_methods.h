#pragma once
#include "taskbar/prayer_model.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <concepts>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace minima {
// Ways to read a masjid's times, tried in this order by Automatic.
enum class PrayerMethod {Automatic,GalaxyStream,AthanPlus,Mawaqit,Table,Text,Count};
inline const wchar_t* prayer_method_name(PrayerMethod m) {
    switch(m) {
    case PrayerMethod::GalaxyStream:return L"GalaxyStream widget";
    case PrayerMethod::AthanPlus:return L"Masjidal / Athan+ widget";
    case PrayerMethod::Mawaqit:return L"Mawaqit";
    case PrayerMethod::Table:return L"Prayer table on the page";
    case PrayerMethod::Text:return L"Times in the page text";
    default:return L"Automatic";
    }
}

// A complete, plausible day: five prayers in order at believable times, each
// Iqama at or after its Adhan (within two hours), sunrise between Fajr and Zuhr.
inline bool sensible(const PrayerDay& d) {
    if(!d.valid())return false;
    const int f=d[Salah::Fajr].adhan,z=d[Salah::Zuhr].adhan,a=d[Salah::Asr].adhan,m=d[Salah::Maghrib].adhan,i=d[Salah::Isha].adhan;
    if(f<150||f>570||z<660||z>900||a<780||a>1230||m<900||m>1380||i<1000||i>1439)return false;
    if(!(f<z&&z<a&&a<m&&m<i))return false;
    if(const int s=d[Salah::Sunrise].begin;s>=0&&!(f<s&&s<z))return false;
    for(const auto s:{Salah::Fajr,Salah::Zuhr,Salah::Asr,Salah::Maghrib,Salah::Isha,Salah::Jumah1}) {
        const auto& t=d[s];if(t.iqama>=0&&(t.adhan<0||t.iqama<t.adhan||t.iqama>t.adhan+120))return false;
    }
    return true;
}

namespace prayer_text {
inline char lower(char c){return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));}
// Visible text of a page (scripts and styles dropped, tags become spaces,
// common entities decoded), optionally only what is inside <table> elements.
inline std::string visible_text(std::string_view html,bool tables_only) {
    std::string out;out.reserve(html.size()/2);
    int table=0;std::size_t i=0;
    const auto starts=[&](std::size_t at,std::string_view tag){if(at+tag.size()>html.size())return false;for(std::size_t k=0;k<tag.size();++k)if(lower(html[at+k])!=tag[k])return false;return true;};
    while(i<html.size()) {
        const char c=html[i];
        if(c=='<') {
            if(starts(i,"<script")||starts(i,"<style")) {
                const std::string_view close=starts(i,"<script")?"</script":"</style";
                std::size_t k=i;while(k<html.size()&&!starts(k,close))++k;
                i=k;continue;
            }
            if(starts(i,"<table"))++table;else if(starts(i,"</table")&&table>0)--table;
            while(i<html.size()&&html[i]!='>')++i;
            ++i;if(!tables_only||table>0)out+=' ';continue;
        }
        if(tables_only&&table==0){++i;continue;}
        if(c=='&') {
            const auto semi=html.find(';',i);
            if(semi!=std::string_view::npos&&semi-i<=8) {
                const auto entity=html.substr(i,semi-i+1);
                if(entity=="&nbsp;"||entity=="&#160;")out+=' ';else if(entity=="&amp;")out+='&';else if(entity=="&#58;")out+=':';else if(entity=="&#39;"||entity=="&rsquo;")out+='\'';else out+=' ';
                i=semi+1;continue;
            }
        }
        out+=(c=='\r'||c=='\n'||c=='\t')?' ':c;++i;
    }
    return out;
}
struct Token {std::size_t at;int minutes;bool marked;}; // marked: had AM/PM.
// "6:25", "6:25 AM", "06:25pm", "1:30 p.m."
inline std::vector<Token> times(std::string_view text,std::size_t from=0,std::size_t to=std::string_view::npos) {
    std::vector<Token> list;to=std::min(to,text.size());
    for(std::size_t i=from;i<to;++i) {
        if(!std::isdigit(static_cast<unsigned char>(text[i]))||(i>0&&std::isdigit(static_cast<unsigned char>(text[i-1]))))continue;
        std::size_t k=i;int hour=0;while(k<to&&k-i<2&&std::isdigit(static_cast<unsigned char>(text[k]))){hour=hour*10+(text[k]-'0');++k;}
        if(k>=to||text[k]!=':')continue;
        if(k+2>=text.size()||!std::isdigit(static_cast<unsigned char>(text[k+1]))||!std::isdigit(static_cast<unsigned char>(text[k+2])))continue;
        if(k+3<text.size()&&std::isdigit(static_cast<unsigned char>(text[k+3])))continue;
        const int minute=(text[k+1]-'0')*10+(text[k+2]-'0');
        if(hour>23||minute>59)continue;
        std::size_t m=k+3;while(m<text.size()&&text[m]==' ')++m;
        bool marked=false;
        if(m+1<text.size()&&(lower(text[m])=='a'||lower(text[m])=='p')&&(lower(text[m+1])=='m'||(text[m+1]=='.'&&m+2<text.size()&&lower(text[m+2])=='m'))) {
            marked=true;const bool pm=lower(text[m])=='p';
            if(pm&&hour<12)hour+=12;else if(!pm&&hour==12)hour=0;
        }
        list.push_back({i,hour*60+minute,marked});
        i=k+2;
    }
    return list;
}
// A 12-hour time without AM/PM, read the way that prayer is said.
inline int resolve(Salah s,const Token& t) {
    if(t.marked||t.minutes>=13*60)return t.minutes;
    const int hour=t.minutes/60;
    switch(s) {
    case Salah::Fajr:case Salah::Sunrise:return hour==12?t.minutes-720:t.minutes;
    case Salah::Zuhr:case Salah::Jumah1:case Salah::Jumah2:case Salah::Jumah3:return hour<=10?t.minutes+720:t.minutes;
    default:return hour<12?t.minutes+720:t.minutes;
    }
}
struct Name {Salah salah;std::size_t at,end;};
// Every whole-word mention of a prayer name, in page order.
inline std::vector<Name> names(std::string_view text) {
    static const std::pair<Salah,std::vector<std::string_view>> table[]={
        {Salah::Fajr,{"fajr","fajar","fajer","subh","sobh"}},{Salah::Sunrise,{"sunrise","shuruq","shurooq","shorook"}},
        {Salah::Zuhr,{"zuhr","dhuhr","duhr","dhuhur","zohr","thuhr","zuhur","dhur","zuhar"}},{Salah::Asr,{"asr"}},
        {Salah::Maghrib,{"maghrib","magrib","maghreb"}},{Salah::Isha,{"isha","esha","ishaa","ishÄ"}},{Salah::Jumah1,{"jumu'ah","jummah","jumuah","jumah","juma"}}};
    std::string low(text);for(auto& c:low)c=lower(c);
    std::vector<Name> found;
    for(const auto& [salah,aliases]:table)for(const auto alias:aliases) {
        for(auto at=low.find(alias);at!=std::string::npos;at=low.find(alias,at+1)) {
            const bool before=at==0||!std::isalpha(static_cast<unsigned char>(low[at-1]));
            const auto end=at+alias.size();
            const bool after=end>=low.size()||!std::isalpha(static_cast<unsigned char>(low[end]));
            if(before&&after)found.push_back({salah,at,end});
        }
    }
    std::sort(found.begin(),found.end(),[](const Name& a,const Name& b){return a.at<b.at;});
    // Drop overlaps (e.g. "jumu'ah" and "juma").
    std::vector<Name> unique;for(const auto& n:found)if(unique.empty()||n.at>=unique.back().end)unique.push_back(n);
    return unique;
}
// Rows: each prayer name followed by its time(s) before the next prayer name.
inline std::optional<PrayerDay> rows(std::string_view text) {
    const auto list=names(text);PrayerDay day;bool any=false;
    for(std::size_t n=0;n<list.size();++n) {
        auto& t=day[list[n].salah];if(t.adhan>=0||(list[n].salah==Salah::Sunrise&&t.begin>=0))continue;
        const std::size_t to=n+1<list.size()?list[n+1].at:std::min(text.size(),list[n].end+160);
        const auto found=times(text,list[n].end,std::min(to,list[n].end+160));
        if(found.empty())continue;
        const int first=resolve(list[n].salah,found[0]);
        if(list[n].salah==Salah::Sunrise){t.begin=first;continue;}
        any=true;
        // Three times read Begins, Adhan, Iqama; two read Adhan, Iqama.
        if(found.size()>=3){t.begin=first;t.adhan=resolve(list[n].salah,found[1]);t.iqama=resolve(list[n].salah,found[2]);}
        else {t.begin=t.adhan=first;if(found.size()>1)t.iqama=resolve(list[n].salah,found[1]);}
    }
    if(!any)return std::nullopt;
    return day;
}
// Columns: a header row of prayer names, then a row of Adhan times and a row
// of Iqama times (in either order; the later of each pair is the Iqama).
inline std::optional<PrayerDay> columns(std::string_view text) {
    const auto list=names(text);
    for(std::size_t start=0;start<list.size();++start) {
        if(list[start].salah!=Salah::Fajr)continue;
        std::vector<Salah> header;std::size_t k=start;
        while(k<list.size()&&header.size()<7) {
            if(k>start&&!times(text,list[k-1].end,list[k].at).empty())break; // A time between names: not a header.
            header.push_back(list[k].salah);++k;
        }
        if(header.size()<5)continue;
        const auto found=times(text,list[k-1].end,std::min(text.size(),list[k-1].end+1200));
        // A month-long timetable has a row per day; without knowing today's row,
        // decline rather than show another day's times.
        if(found.size()>header.size()*3)continue;
        PrayerDay day;std::size_t used=0;
        for(const auto s:header){if(used>=found.size())break;auto& t=day[s];const int v=resolve(s,found[used++]);if(s==Salah::Sunrise)t.begin=v;else t.begin=t.adhan=v;}
        for(const auto s:header) {
            if(s==Salah::Sunrise||used>=found.size())continue;
            auto& t=day[s];const int v=resolve(s,found[used++]);
            if(v<t.adhan){t.iqama=t.adhan;t.begin=t.adhan=v;}else t.iqama=v;
        }
        if(sensible(day))return day;
    }
    return std::nullopt;
}
}
// A prayer table (or any listing) in the page: rows first, then columns.
inline std::optional<PrayerDay> parse_listing(std::string_view html,bool tables_only) {
    const auto text=prayer_text::visible_text(html,tables_only);
    if(auto d=prayer_text::rows(text);d&&sensible(*d))return d;
    if(auto d=prayer_text::columns(text))return d;
    return std::nullopt;
}

// Mawaqit pages embed today's times ("times": Fajr, Dhuhr, Asr, Maghrib, Isha),
// sunrise ("shuruq"), Jumu'ah ("jumua") and a yearly Iqama calendar of offsets
// ("+10") or fixed times per day.
inline std::optional<PrayerDay> parse_mawaqit(std::string_view page,int month,int day_of_month) {
    const auto quoted=[&](std::size_t from,std::size_t limit,std::vector<std::string_view>& out) {
        std::size_t i=from;
        while(i<limit) {const auto a=page.find('"',i);if(a==std::string_view::npos||a>=limit)break;const auto b=page.find('"',a+1);if(b==std::string_view::npos||b>=limit)break;out.push_back(page.substr(a+1,b-a-1));i=b+1;}
    };
    const auto clock=[&](std::string_view s)->int{const auto t=prayer_text::times(s);return t.empty()?-1:t[0].minutes;};
    const auto key=page.find("\"times\":[");if(key==std::string_view::npos)return std::nullopt;
    const auto close=page.find(']',key);if(close==std::string_view::npos)return std::nullopt;
    std::vector<std::string_view> values;quoted(key+9,close,values);if(values.size()<5)return std::nullopt;
    PrayerDay d;const Salah order[]={Salah::Fajr,Salah::Zuhr,Salah::Asr,Salah::Maghrib,Salah::Isha};
    for(int i=0;i<5;++i){auto& t=d[order[i]];t.begin=t.adhan=clock(values[static_cast<std::size_t>(i)]);}
    if(const auto s=page.find("\"shuruq\":\"");s!=std::string_view::npos)d[Salah::Sunrise].begin=clock(page.substr(s+10,5));
    if(const auto j=page.find("\"jumua\":\"");j!=std::string_view::npos){const int v=clock(page.substr(j+9,5));if(v>=0)d[Salah::Jumah1].begin=d[Salah::Jumah1].adhan=v;}
    // Iqama: the month's object in iqamaCalendar, then this day's five entries.
    if(const auto cal=page.find("\"iqamaCalendar\":[");cal!=std::string_view::npos) {
        std::size_t i=cal+17;int depth=0,index=-1;std::size_t object_start=std::string_view::npos,object_end=std::string_view::npos;
        for(;i<page.size();++i) {
            const char c=page[i];
            if(c=='{'){if(depth==0&&++index==month-1)object_start=i;++depth;}
            else if(c=='}'){--depth;if(depth==0&&index==month-1){object_end=i;break;}}
            else if(c==']'&&depth==0)break;
        }
        if(object_start!=std::string_view::npos&&object_end!=std::string_view::npos) {
            const std::string dayKey="\""+std::to_string(day_of_month)+"\":[";
            const auto at=page.substr(object_start,object_end-object_start).find(dayKey);
            if(at!=std::string_view::npos) {
                const auto from=object_start+at+dayKey.size();const auto end=page.find(']',from);
                std::vector<std::string_view> iq;if(end!=std::string_view::npos)quoted(from,end,iq);
                for(std::size_t k=0;k<iq.size()&&k<5;++k) {
                    auto& t=d[order[k]];const auto v=iq[k];
                    if(!v.empty()&&(v[0]=='+'||v[0]=='-')){int minutes=0;for(std::size_t q=1;q<v.size()&&std::isdigit(static_cast<unsigned char>(v[q]));++q)minutes=minutes*10+(v[q]-'0');t.iqama=t.adhan+(v[0]=='-'?-minutes:minutes);}
                    else if(const int fixed=clock(v);fixed>=0)t.iqama=fixed;
                }
            }
        }
    }
    if(!sensible(d))return std::nullopt;
    return d;
}

// Calculation conventions (praytimes.org / AlAdhan values). Fajr and Isha are
// the sun's depression in degrees; Umm al-Qura sets Isha a fixed time after
// Maghrib instead, and Tehran sets Maghrib by angle rather than at sunset.
enum class CalcMethod {Automatic,MWL,ISNA,Egypt,Makkah,Karachi,Dubai,Singapore,Turkey,Tehran,Count};
struct CalcParams {
    double fajr=18,isha=17;
    int isha_minutes=0;   // >0: Isha this many minutes after Maghrib.
    double maghrib=0;     // >0: Maghrib at this depression, else at sunset.
    int asr_factor=1;     // Shadow length: 1 Standard (Shafi, Maliki, Hanbali), 2 Hanafi.
};
inline CalcParams calc_params(CalcMethod m,int asr_factor=1) {
    CalcParams p;p.asr_factor=asr_factor==2?2:1;
    switch(m) {
    case CalcMethod::ISNA:p.fajr=15;p.isha=15;break;
    case CalcMethod::Egypt:p.fajr=19.5;p.isha=17.5;break;
    case CalcMethod::Makkah:p.fajr=18.5;p.isha_minutes=90;break; // 120 in Ramadan: the caller knows the Hijri month.
    case CalcMethod::Karachi:p.fajr=18;p.isha=18;break;
    case CalcMethod::Dubai:p.fajr=18.2;p.isha=18.2;break;
    case CalcMethod::Singapore:p.fajr=20;p.isha=18;break;
    case CalcMethod::Turkey:p.fajr=18;p.isha=17;break;
    case CalcMethod::Tehran:p.fajr=17.7;p.isha=14;p.maghrib=4.5;break;
    default:break; // Muslim World League: 18 / 17.
    }
    return p;
}
inline const wchar_t* calc_method_name(CalcMethod m) {
    switch(m) {
    case CalcMethod::MWL:return L"Muslim World League";
    case CalcMethod::ISNA:return L"ISNA";
    case CalcMethod::Egypt:return L"Egyptian Authority";
    case CalcMethod::Makkah:return L"Umm al-Qura";
    case CalcMethod::Karachi:return L"Karachi";
    case CalcMethod::Dubai:return L"Dubai";
    case CalcMethod::Singapore:return L"Singapore";
    case CalcMethod::Turkey:return L"Diyanet";
    case CalcMethod::Tehran:return L"Tehran";
    default:return L"Automatic";
    }
}
namespace detail {
inline std::string country_code(std::string_view country) {
    std::string c;for(const char ch:country.substr(0,2))c+=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return c;
}
inline bool one_of(const std::string& c,std::initializer_list<const char*> list){for(const auto* x:list)if(c==x)return true;return false;}
}
// The convention most used in a country (ISO 3166 alpha-2, any case), as the
// established libraries choose it; elsewhere the Muslim World League.
inline CalcMethod auto_method(std::string_view country) {
    const auto c=detail::country_code(country);
    if(detail::one_of(c,{"pk","in","bd","af"}))return CalcMethod::Karachi;
    if(detail::one_of(c,{"us","ca"}))return CalcMethod::ISNA;
    if(detail::one_of(c,{"eg","sd","ly","sy","iq","lb","jo","ps"}))return CalcMethod::Egypt;
    if(detail::one_of(c,{"sa","ye"}))return CalcMethod::Makkah;
    if(c=="ae")return CalcMethod::Dubai;
    if(detail::one_of(c,{"sg","my","bn","id"}))return CalcMethod::Singapore;
    if(c=="tr")return CalcMethod::Turkey;
    if(c=="ir")return CalcMethod::Tehran;
    return CalcMethod::MWL;
}
// Asr shadow factor most used in a country: Hanafi across South Asia and Turkey.
inline int auto_asr(std::string_view country) {
    return detail::one_of(detail::country_code(country),{"pk","in","bd","af","tr"})?2:1;
}

// Calculated times (praytimes.org algorithm) for one date at a place.
// `to_local` turns an instant, in hours after that date's UTC midnight (it may
// be negative or past 24), into minutes after local midnight, so each time gets
// the offset in force at that instant (daylight saving changes mid-day).
// Above about 48° the sun may not get deep enough for Fajr or Isha: those then
// follow the angle-based rule (at most angle/60 of the night from sunrise or
// sunset). Where the sun doesn't rise or set at all, the day is calculated at
// 65° and `approximate` is set. Iqama is left unset: only the masjid publishes it.
template<std::invocable<double> ToLocal>
PrayerDay calculate_day(int year,int month,int day,double latitude,double longitude,const CalcParams& params,ToLocal to_local,bool* approximate=nullptr) {
    constexpr double pi=3.14159265358979323846;
    const double nan=std::nan("");
    const auto dsin=[&](double d){return std::sin(d*pi/180);};const auto dcos=[&](double d){return std::cos(d*pi/180);};
    const auto dtan=[&](double d){return std::tan(d*pi/180);};const auto darcsin=[&](double x){return std::asin(x)*180/pi;};
    const auto darctan2=[&](double y,double x){return std::atan2(y,x)*180/pi;};
    const auto darccot=[&](double x){return std::atan(1/x)*180/pi;};
    const auto fix=[](double a,double b){a-=b*std::floor(a/b);return a<0?a+b:a;};
    int y=year,m=month;if(m<=2){y-=1;m+=12;}
    const double A=std::floor(y/100.0),B=2-A+std::floor(A/4);
    const double jdate=std::floor(365.25*(y+4716))+std::floor(30.6001*(m+1))+day+B-1524.5-longitude/(15*24);
    struct Sun {double declination,equation;};
    const auto sun=[&](double jd)->Sun{
        const double D=jd-2451545.0,g=fix(357.529+0.98560028*D,360),q=fix(280.459+0.98564736*D,360);
        const double L=fix(q+1.915*dsin(g)+0.020*dsin(2*g),360),e=23.439-0.00000036*D;
        const double RA=fix(darctan2(dcos(e)*dsin(L),dcos(L))/15,24);
        return {darcsin(dsin(e)*dsin(L)),q/15-RA};
    };
    const auto mid_day=[&](double t){return fix(12-sun(jdate+t).equation,24);};
    double lat=latitude;
    // NaN when the sun never reaches that angle today.
    const auto angle_time=[&](double angle,double t,bool before_noon){
        const double decl=sun(jdate+t).declination,noon=mid_day(t);
        const double c=(-dsin(angle)-dsin(decl)*dsin(lat))/(dcos(decl)*dcos(lat));
        if(c<-1||c>1)return nan;
        const double T=std::acos(c)*180/pi/15;
        return noon+(before_noon?-T:T);
    };
    const auto asr_time=[&](double factor,double t){const double decl=sun(jdate+t).declination;return angle_time(-darccot(factor+dtan(std::abs(lat-decl))),t,false);};
    double sunrise=angle_time(0.833,6.0/24,true),sunset=angle_time(0.833,18.0/24,false);
    const bool polar=std::isnan(sunrise)||std::isnan(sunset);
    if(polar){lat=std::copysign(65.0,latitude);sunrise=angle_time(0.833,6.0/24,true);sunset=angle_time(0.833,18.0/24,false);}
    if(approximate)*approximate=polar;
    const double dhuhr=mid_day(12.0/24);
    double fajr=angle_time(params.fajr,5.0/24,true),asr=asr_time(params.asr_factor,13.0/24);
    double maghrib=params.maghrib>0?angle_time(params.maghrib,18.0/24,false):sunset;
    // Angle-based high-latitude rule: never further from sunrise / sunset than
    // angle/60 of the night (also covers twilight that lasts all night).
    const double night=24-(sunset-sunrise);
    if(const double portion=params.fajr/60*night;std::isnan(fajr)||sunrise-fajr>portion)fajr=sunrise-portion;
    if(params.maghrib>0)if(const double portion=params.maghrib/60*night;std::isnan(maghrib)||maghrib-sunset>portion)maghrib=sunset+portion;
    double isha=params.isha_minutes>0?maghrib+params.isha_minutes/60.0:angle_time(params.isha,18.0/24,false);
    if(params.isha_minutes<=0)if(const double portion=params.isha/60*night;std::isnan(isha)||isha-sunset>portion)isha=sunset+portion;
    if(std::isnan(asr))asr=dhuhr+(sunset-dhuhr)/2; // Sun too low all day for the shadow rule.
    const auto minutes=[&](double h){return to_local(h-longitude/15);};
    PrayerDay d;d.year=year;d.month=month;d.day=day;
    const auto set=[&](Salah s,double h){auto& t=d[s];t.begin=t.adhan=minutes(h);};
    set(Salah::Fajr,fajr);d[Salah::Sunrise].begin=minutes(sunrise);set(Salah::Zuhr,dhuhr);set(Salah::Asr,asr);set(Salah::Maghrib,maghrib);set(Salah::Isha,isha);
    return d;
}
// The same with one fixed UTC offset (hours) for the whole day.
inline PrayerDay calculate_day(int year,int month,int day,double latitude,double longitude,const CalcParams& params,double utc_offset,bool* approximate=nullptr) {
    return calculate_day(year,month,day,latitude,longitude,params,[utc_offset](double utc){
        const long total=std::lround((utc+utc_offset)*60);return static_cast<int>(((total%1440)+1440)%1440);
    },approximate);
}
}
