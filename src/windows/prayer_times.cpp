#include "windows/prayer_times.h"
#include "taskbar/prayer_methods.h"
#include "windows/geolocation.h"
#include "windows/http.h"
#include <shlobj.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace minima {
namespace {
constexpr wchar_t settings_key[]=L"Software\\MinimaDock\\Prayer";
constexpr ULONGLONG refresh_interval=3*60*60*1000,retry_interval=15*60*1000;
// The website embeds GalaxyStream's widget script with the masjid's id.
std::optional<std::wstring> find_widget(const std::string& page) {
    constexpr std::string_view marker="apps/prayer-times/PrayerTimesJS.asp?uid=";
    const auto at=page.find(marker);if(at==std::string::npos)return std::nullopt;
    std::wstring id;for(auto i=at+marker.size();i<page.size()&&page[i]>='0'&&page[i]<='9'&&id.size()<9;++i)id+=static_cast<wchar_t>(page[i]);
    if(id.empty())return std::nullopt;
    return L"https://www.galaxystream.com/apps/prayer-times/PrayerTimesJS.asp?uid="+id+L"&displayType=small";
}
std::filesystem::path cache_path() {
    PWSTR folder=nullptr;std::filesystem::path result;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&folder)))result=std::filesystem::path(folder)/L"MinimaDock"/L"prayer-times.txt";
    CoTaskMemFree(folder);return result;
}
std::wstring local_time_text() {
    wchar_t text[64]{};GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,nullptr,nullptr,text,64);return text;
}
std::string narrow(const std::wstring& value) {
    const int size=WideCharToMultiByte(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(static_cast<size_t>(size>0?size:0),'\0');
    if(size>0)WideCharToMultiByte(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr);
    return result;
}
std::wstring widen(const std::string& value) {
    const int size=MultiByteToWideChar(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(static_cast<size_t>(size>0?size:0),L'\0');
    if(size>0)MultiByteToWideChar(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),result.data(),size);
    return result;
}
int yyyymmdd(const SYSTEMTIME& t){return t.wYear*10000+t.wMonth*100+t.wDay;}
bool is_today(const PrayerDay& d,const SYSTEMTIME& t){return d.year==t.wYear&&d.month==t.wMonth&&d.day==t.wDay;}
// Website times are cached only for the website and reading method that produced them.
std::string cache_fingerprint(const PrayerSettings& s) {
    return std::to_string(std::hash<std::wstring>{}(s.website+L'|'+std::to_wstring(s.method)));
}
// This PC's region (ISO 3166 alpha-2), for the automatic method before any place has a country.
std::string pc_region() {
    wchar_t code[8]{};
    return GetGeoInfoW(GetUserGeoID(GEOCLASS_NATION),GEO_ISO2,code,8,0)>0?narrow(code):std::string();
}
// Minutes after local midnight for an instant given in hours after the date's
// UTC midnight, with the offset in force at that instant (daylight saving).
int local_minutes(int year,int month,int day,double hours) {
    SYSTEMTIME midnight{};midnight.wYear=static_cast<WORD>(year);midnight.wMonth=static_cast<WORD>(month);midnight.wDay=static_cast<WORD>(day);
    FILETIME file{};SystemTimeToFileTime(&midnight,&file);
    ULARGE_INTEGER at{};at.LowPart=file.dwLowDateTime;at.HighPart=file.dwHighDateTime;
    at.QuadPart+=static_cast<ULONGLONG>(std::llround((hours+48)*60))*600'000'000ULL-48ULL*60*600'000'000ULL; // Whole minutes; +48 h keeps it unsigned.
    file.dwLowDateTime=at.LowPart;file.dwHighDateTime=at.HighPart;
    SYSTEMTIME utc{},local{};DYNAMIC_TIME_ZONE_INFORMATION zone{};GetDynamicTimeZoneInformation(&zone);
    if(!FileTimeToSystemTime(&file,&utc)||!SystemTimeToTzSpecificLocalTimeEx(&zone,&utc,&local))return -1;
    return local.wHour*60+local.wMinute;
}
double utc_offset_hours() {
    TIME_ZONE_INFORMATION zone{};const auto mode=GetTimeZoneInformation(&zone);
    const long bias=zone.Bias+(mode==TIME_ZONE_ID_DAYLIGHT?zone.DaylightBias:mode==TIME_ZONE_ID_STANDARD?zone.StandardBias:0);
    return -static_cast<double>(bias)/60;
}
// Umm al-Qura month of a date, from Windows' own Umm al-Qura calendar (the
// default calendar of the ar-SA locale); 0 if unavailable.
int umm_al_qura_month(const SYSTEMTIME& t) {
    wchar_t month[8]{};
    return GetDateFormatEx(L"ar-SA",0,&t,L"M",month,8,nullptr)>0?_wtoi(month):0;
}
struct Calculated {PrayerDay day;std::wstring method,note;};
Calculated calculate(const PrayerSettings& s,const SYSTEMTIME& t) {
    const std::string country=s.country.empty()?pc_region():s.country;
    const auto method=s.calc_method>0&&s.calc_method<static_cast<int>(CalcMethod::Count)?static_cast<CalcMethod>(s.calc_method):auto_method(country);
    const int asr=s.asr==1||s.asr==2?s.asr:auto_asr(country);
    auto params=calc_params(method,asr);
    if(method==CalcMethod::Makkah&&umm_al_qura_month(t)==9)params.isha_minutes=120; // Ramadan.
    bool approximate=false;Calculated result;
    const int y=t.wYear,m=t.wMonth,d=t.wDay;
    result.day=calculate_day(y,m,d,s.latitude,s.longitude,params,[y,m,d](double hours){return local_minutes(y,m,d,hours);},&approximate);
    result.method=std::wstring(L"Calculated · ")+calc_method_name(method)+(asr==2?L" · Hanafi":L" · Standard");
    if(approximate)result.note=L"Approximate (high latitude)";
    else if(std::abs(s.longitude/15-utc_offset_hours())>2)result.note=L"Times shown in this PC's time zone";
    return result;
}
}

PrayerSettings load_prayer_settings() {
    PrayerSettings settings;
    const auto text=[](const wchar_t* name)->std::wstring{
        wchar_t value[1024]{};DWORD size=sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER,settings_key,name,RRF_RT_REG_SZ,nullptr,value,&size)==ERROR_SUCCESS?value:L"";
    };
    const auto flag=[](const wchar_t* name,DWORD fallback){DWORD value=fallback,size=sizeof(value);RegGetValueW(HKEY_CURRENT_USER,settings_key,name,RRF_RT_REG_DWORD,nullptr,&value,&size);return value;};
    settings.website=text(L"Website");
    // Older versions had only the website: keep using it until the user switches.
    settings.source=flag(L"Source",settings.website.empty()?1:0)==1?PrayerSource::Location:PrayerSource::Website;
    settings.locate=flag(L"Locate",0)==1?LocateMode::Device:LocateMode::City;
    settings.clock=flag(L"Clock",1)!=0;settings.reminders=flag(L"Reminders",1)!=0;
    settings.iqama_lead=static_cast<int>(std::min<DWORD>(flag(L"IqamaReminderMinutes",10),60));
    settings.chime=static_cast<int>(std::min<DWORD>(flag(L"Chime",0),2));settings.sunrise=flag(L"Sunrise",1)!=0;
    settings.method=static_cast<int>(std::min<DWORD>(flag(L"Method",0),16));settings.calculate=flag(L"Calculate",1)!=0;
    settings.calc_method=static_cast<int>(flag(L"CalcMethod",0));
    if(settings.calc_method>=static_cast<int>(CalcMethod::Count))settings.calc_method=0;
    settings.asr=static_cast<int>(std::min<DWORD>(flag(L"Asr",0),2));
    settings.located_day=static_cast<int>(flag(L"LocatedDay",0));
    // "lat, lon" written in the C locale (the app never calls setlocale).
    double latitude=0,longitude=0;
    if(swscanf(text(L"Location").c_str(),L"%lf , %lf",&latitude,&longitude)==2&&valid_coordinates(latitude,longitude)) {
        settings.located=true;settings.latitude=latitude;settings.longitude=longitude;
        settings.place=text(L"Place");settings.country=narrow(text(L"Country")).substr(0,2);
    }
    // 1.0.0-1.0.2 saved their built-in default (Toronto) with any prayer setting.
    // Without a website or a chosen place name, that was never the user's choice.
    if(settings.located&&settings.website.empty()&&settings.place.empty()&&
       std::abs(settings.latitude-43.6532)<1e-4&&std::abs(settings.longitude+79.3832)<1e-4)settings.located=false;
    return settings;
}
void save_prayer_settings(const PrayerSettings& settings) {
    const auto text=[](const wchar_t* name,const std::wstring& value){RegSetKeyValueW(HKEY_CURRENT_USER,settings_key,name,REG_SZ,value.c_str(),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));};
    const auto flag=[](const wchar_t* name,DWORD value){RegSetKeyValueW(HKEY_CURRENT_USER,settings_key,name,REG_DWORD,&value,sizeof(value));};
    text(L"Website",settings.website);
    flag(L"Source",settings.source==PrayerSource::Location);flag(L"Locate",settings.locate==LocateMode::Device);
    flag(L"Clock",settings.clock);flag(L"Reminders",settings.reminders);flag(L"IqamaReminderMinutes",static_cast<DWORD>(settings.iqama_lead));
    flag(L"Chime",static_cast<DWORD>(settings.chime));flag(L"Sunrise",settings.sunrise);flag(L"Method",static_cast<DWORD>(settings.method));flag(L"Calculate",settings.calculate);
    flag(L"CalcMethod",static_cast<DWORD>(settings.calc_method));flag(L"Asr",static_cast<DWORD>(settings.asr));flag(L"LocatedDay",static_cast<DWORD>(settings.located_day));
    if(settings.located) {
        wchar_t place[64]{};swprintf(place,64,L"%.4f, %.4f",settings.latitude,settings.longitude);
        text(L"Location",place);text(L"Place",settings.place);text(L"Country",widen(settings.country));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER,settings_key,L"Location");RegDeleteKeyValueW(HKEY_CURRENT_USER,settings_key,L"Place");RegDeleteKeyValueW(HKEY_CURRENT_USER,settings_key,L"Country");
    }
}

namespace {
// One worker and everything it touches. start() makes a new State each time and
// stop() lets go of it, so a request still running for old settings finishes
// into a State nobody reads (its notify window is cleared).
struct State {
    std::mutex mutex;
    std::condition_variable wake;
    PrayerSnapshot snapshot;
    PrayerSettings settings;
    bool requested=false,forced=false,stopping=false,busy=false,finished=false;
    std::atomic<HWND> notify=nullptr;
    std::atomic<bool> cancel=false;
    // Worker thread only.
    ULONGLONG last_success=0,last_attempt=0;bool failed=false;

    void post(){if(const auto window=notify.load())PostMessageW(window,prayer_times_message,0,0);}
    bool stopped(){std::lock_guard lock(mutex);return stopping;}
    // Cache: "v3|Y-M-D|updated|method|fingerprint" then "salah begin adhan
    // iqama" lines. Only website times are cached; calculated ones are instant.
    void load_cache() {
        if(settings.source!=PrayerSource::Website)return;
        const auto path=cache_path();std::ifstream file(path,std::ios::binary);if(!file)return;
        std::string header;std::getline(file,header);
        PrayerDay day;std::string updated,method;
        if(header.rfind("v3|",0)==0||header.rfind("v2|",0)==0) {
            std::istringstream fields(header.substr(3));std::string date,fingerprint;
            std::getline(fields,date,'|');std::getline(fields,updated,'|');std::getline(fields,method,'|');std::getline(fields,fingerprint);
            if(header[1]=='3'&&fingerprint!=cache_fingerprint(settings))return; // Another website or method.
            if(std::sscanf(date.c_str(),"%d-%d-%d",&day.year,&day.month,&day.day)!=3)return;
            int salah=0,begin=0,adhan=0,iqama=0;
            while(file>>salah>>begin>>adhan>>iqama)if(salah>=0&&salah<static_cast<int>(Salah::Count))day.times[static_cast<size_t>(salah)]={begin,adhan,iqama};
        } else { // Earlier versions cached the GalaxyStream script itself.
            const std::string body((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            char text[64]{};if(std::sscanf(header.c_str(),"%d-%d-%d|%63[^\r\n]",&day.year,&day.month,&day.day,text)<3)return;
            auto parsed=parse_galaxystream(body);if(!parsed)return;
            const int y=day.year,m=day.month,d=day.day;day=*parsed;day.year=y;day.month=m;day.day=d;updated=text;method="GalaxyStream widget";
        }
        if(!day.valid())return;
        std::lock_guard lock(mutex);snapshot.have=true;snapshot.day=day;snapshot.updated=widen(updated);snapshot.method=widen(method);
    }
    void save_cache(const PrayerSettings& s,const PrayerDay& day,const std::wstring& updated,const std::wstring& method) {
        const auto path=cache_path();if(path.empty()||stopped())return;
        std::error_code ignored;std::filesystem::create_directories(path.parent_path(),ignored);
        std::ofstream file(path,std::ios::binary|std::ios::trunc);if(!file)return;
        file<<"v3|"<<day.year<<'-'<<day.month<<'-'<<day.day<<'|'<<narrow(updated)<<'|'<<narrow(method)<<'|'<<cache_fingerprint(s)<<'\n';
        for(size_t s2=0;s2<day.times.size();++s2)file<<s2<<' '<<day.times[s2].begin<<' '<<day.times[s2].adhan<<' '<<day.times[s2].iqama<<'\n';
    }
    // Location: publishes the calculated day (the snapshot lock is taken here).
    void publish(const PrayerSettings& s,const SYSTEMTIME& t,const std::wstring& note) {
        auto c=calculate(s,t);
        std::lock_guard lock(mutex);
        snapshot.have=true;snapshot.day=c.day;snapshot.method=c.method;snapshot.updated=local_time_text();snapshot.error.clear();
        snapshot.note=note.empty()?c.note:c.note.empty()?note:note+L" · "+c.note;
        snapshot.located=s;
    }
    bool due(bool force) {
        if(force)return true;
        const auto now=GetTickCount64();SYSTEMTIME t{};GetLocalTime(&t);
        PrayerSnapshot s;PrayerSettings settings_now;{std::lock_guard lock(mutex);s=snapshot;settings_now=settings;}
        const bool today=s.have&&is_today(s.day,t);
        const bool retry_wait=failed&&now-last_attempt<retry_interval;
        if(settings_now.source==PrayerSource::Location) {
            if(!today)return true; // A new day: recalculating is instant.
            return settings_now.locate==LocateMode::Device&&settings_now.located_day!=yyyymmdd(t)&&!retry_wait;
        }
        if(retry_wait&&(today||!settings_now.calculate))return false;
        return !today||!last_success||now-last_success>=refresh_interval;
    }
    // A widget or page address found in the masjid's page: "host/path?..." up to the closing quote.
    static std::optional<std::wstring> embedded_url(const std::string& page,std::string_view marker) {
        const auto at=page.find(marker);if(at==std::string::npos)return std::nullopt;
        auto start=page.rfind('"',at);const auto start2=page.rfind('\'',at);
        if(start==std::string::npos||(start2!=std::string::npos&&start2>start))start=start2;
        if(start==std::string::npos)return std::nullopt;
        const char quote=page[start];const auto end=page.find(quote,start+1);if(end==std::string::npos||end-start>600)return std::nullopt;
        std::string url=page.substr(start+1,end-start-1);
        for(auto amp=url.find("&amp;");amp!=std::string::npos;amp=url.find("&amp;"))url.replace(amp,5,"&");
        if(url.rfind("//",0)==0)url="https:"+url;
        if(url.rfind("http",0)!=0)return std::nullopt;
        return widen(url);
    }
    // Location source. The saved place is calculated at once; following the
    // device then asks Windows for a fix (once a day) and only names the place
    // through Nominatim after a move of more than 5 km.
    void fetch_location(PrayerSettings s,const SYSTEMTIME& t) {
        if(s.located)publish(s,t,{});
        if(s.locate!=LocateMode::Device||s.located_day==yyyymmdd(t)) {
            if(!s.located){std::lock_guard lock(mutex);snapshot=PrayerSnapshot{};snapshot.error=L"Set your location";}
            failed=false;last_success=GetTickCount64();post();return;
        }
        post(); // Show the saved place's times while Windows looks.
        const auto fix=locate_device(&cancel);
        if(stopped())return;
        std::wstring note;
        if(fix.status==LocateStatus::Ok) {
            const bool moved=!s.located||distance_km(s.latitude,s.longitude,fix.latitude,fix.longitude)>5;
            s.located=true;s.latitude=fix.latitude;s.longitude=fix.longitude;s.located_day=yyyymmdd(t);
            if(moved) {
                const auto named=reverse_place(fix.latitude,fix.longitude);
                if(stopped())return;
                if(!named.places.empty()){s.place=named.places.front().label();s.country=named.places.front().country_code;}
                else if(!s.place.empty()&&!s.place.ends_with(L" (near)"))s.place+=L" (near)"; // Keep the old name and country (and so the method).
            }
            if(fix.accuracy_m>25'000)note=L"Approximate location: pick your city for best accuracy";
            failed=false;last_success=GetTickCount64();
            {std::lock_guard lock(mutex);settings=s;} // So due() doesn't ask again today.
        } else {
            failed=true; // Tried again in 15 minutes and on resume; the last place stays in use.
            note=fix.status==LocateStatus::Denied?L"Location is off for desktop apps":
                 fix.status==LocateStatus::Disabled?L"Location is off on this PC":L"Couldn't get this PC's location";
        }
        if(s.located)publish(s,t,note);
        else{std::lock_guard lock(mutex);snapshot=PrayerSnapshot{};snapshot.error=L"Set your location";snapshot.note=note;}
        post();
    }
    // Website source: tries each way of reading the times (or only the chosen
    // one), then the calculated fallback. Only the configured website and the
    // widget hosts it names are contacted.
    void fetch_website(const PrayerSettings& s,const SYSTEMTIME& t) {
        if(s.website.empty()){{std::lock_guard lock(mutex);snapshot=PrayerSnapshot{};snapshot.error=L"Add your masjid's website";}failed=false;last_success=GetTickCount64();post();return;}
        std::optional<PrayerDay> day;std::wstring method,error;
        const auto wants=[&](PrayerMethod m){return s.method==0||s.method==static_cast<int>(m);};
        std::string page;bool page_loaded=false;
        const auto load_page=[&]()->bool{
            if(!page_loaded){page_loaded=true;if(auto p=http_get(s.website))page=std::move(*p);else error=L"Couldn't reach the masjid website";}
            return !page.empty();
        };
        const auto accept=[&](std::optional<PrayerDay> d,PrayerMethod m){if(d&&sensible(*d)){day=d;method=prayer_method_name(m);return true;}return false;};
        if(wants(PrayerMethod::GalaxyStream)) {
            std::optional<std::wstring> widget;
            if(s.website.find(L"PrayerTimesJS.asp")!=std::wstring::npos)widget=s.website;else if(load_page())widget=find_widget(page);
            if(widget)if(auto script=http_get(*widget))accept(parse_galaxystream(*script),PrayerMethod::GalaxyStream);
        }
        if(!day&&wants(PrayerMethod::AthanPlus)) {
            std::optional<std::wstring> widget;
            if(s.website.find(L"athanplus.com")!=std::wstring::npos||s.website.find(L"masjidal.com")!=std::wstring::npos)widget=s.website;
            else if(load_page()){widget=embedded_url(page,"athanplus.com/masjid/widgets");if(!widget)widget=embedded_url(page,"masjidal.com/widget");}
            if(widget)if(auto html=http_get(*widget))accept(parse_listing(*html,false),PrayerMethod::AthanPlus);
        }
        if(!day&&wants(PrayerMethod::Mawaqit)) {
            std::optional<std::string> source;
            if(s.website.find(L"mawaqit.net")!=std::wstring::npos){if(load_page())source=page;}
            else if(load_page())if(const auto link=embedded_url(page,"mawaqit.net/"))source=http_get(*link);
            if(source)accept(parse_mawaqit(*source,t.wMonth,t.wDay),PrayerMethod::Mawaqit);
        }
        if(!day&&wants(PrayerMethod::Table)&&load_page())accept(parse_listing(page,true),PrayerMethod::Table);
        if(!day&&wants(PrayerMethod::Text)&&load_page())accept(parse_listing(page,false),PrayerMethod::Text);
        if(stopped())return;
        if(day) {
            day->year=t.wYear;day->month=t.wMonth;day->day=t.wDay;
            const auto updated=local_time_text();save_cache(s,*day,updated,method);
            failed=false;last_success=GetTickCount64();
            std::lock_guard lock(mutex);snapshot.have=true;snapshot.day=*day;snapshot.updated=updated;snapshot.method=method;snapshot.error.clear();snapshot.note.clear();
        } else {
            failed=true;
            if(error.empty())error=page_loaded&&!page.empty()?L"No prayer times found on the website":L"Couldn't read the prayer times";
            bool today=false;{std::lock_guard lock(mutex);snapshot.error=error;today=snapshot.have&&is_today(snapshot.day,t);}
            // No times for today: calculate Adhan times for the saved location.
            if(!today&&s.calculate) {
                if(s.located){const auto keep=error;publish(s,t,{});std::lock_guard lock(mutex);snapshot.error=keep;snapshot.located=PrayerSettings{};}
                else{std::lock_guard lock(mutex);snapshot.note=L"Add a location to calculate times";}
            }
        }
        post();
    }
    void fetch() {
        last_attempt=GetTickCount64();
        PrayerSettings s;{std::lock_guard lock(mutex);s=settings;}
        SYSTEMTIME t{};GetLocalTime(&t);
        if(s.source==PrayerSource::Location)fetch_location(s,t);else fetch_website(s,t);
    }
    void run() {
        post();
        for(;;) {
            bool force=false;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,[&]{return stopping||requested;});
                if(stopping)break;
                force=forced;requested=forced=false;busy=true;
            }
            if(due(force))fetch();
            std::lock_guard lock(mutex);busy=false;
        }
        {std::lock_guard lock(mutex);finished=true;}
        wake.notify_all();
    }
};
}

struct PrayerService::Impl {
    std::shared_ptr<State> state;   // The running worker's, or null.
    std::shared_ptr<State> retired; // The last stopped one, waited for briefly on exit.
    PrayerSnapshot idle;            // Returned while stopped.
};
PrayerService::PrayerService():impl_(std::make_unique<Impl>()){}
PrayerService::~PrayerService() {
    stop();
    // Give a worker that is mid-request a moment to wind down before statics
    // (such as the Nominatim cache) are destroyed; never hang the exit.
    if(const auto state=impl_->retired){std::unique_lock lock(state->mutex);state->wake.wait_for(lock,std::chrono::milliseconds(500),[&]{return state->finished;});}
}
void PrayerService::start(HWND notify,const PrayerSettings& settings) {
    stop();
    auto state=std::make_shared<State>();state->settings=settings;state->notify=notify;
    // Cheap enough for the UI thread, and the dock shows times without a gap.
    state->load_cache();
    if(settings.source==PrayerSource::Location&&settings.located){SYSTEMTIME t{};GetLocalTime(&t);state->publish(settings,t,{});state->snapshot.located=PrayerSettings{};}
    std::thread([state]{state->run();}).detach();
    impl_->state=std::move(state);
    refresh(false);
}
void PrayerService::stop() {
    auto& state=impl_->state;if(!state)return;
    state->notify=nullptr;state->cancel=true;
    {std::lock_guard lock(state->mutex);state->stopping=true;}
    state->wake.notify_all();
    impl_->retired=std::move(state);
}
void PrayerService::refresh(bool force) {
    const auto& state=impl_->state;if(!state)return;
    {std::lock_guard lock(state->mutex);if(state->busy&&!force)return;state->requested=true;state->forced|=force;}
    state->wake.notify_all();
}
PrayerSnapshot PrayerService::snapshot() const {
    const auto& state=impl_->state;if(!state)return impl_->idle;
    std::lock_guard lock(state->mutex);return state->snapshot;
}
}
