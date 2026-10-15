#include "windows/prayer_times.h"
#include "taskbar/prayer_methods.h"
#include "windows/http.h"
#include <winhttp.h>
#include <shlobj.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
}

PrayerSettings load_prayer_settings() {
    PrayerSettings settings;
    wchar_t website[1024]{};DWORD size=sizeof(website);
    if(RegGetValueW(HKEY_CURRENT_USER,settings_key,L"Website",RRF_RT_REG_SZ,nullptr,website,&size)==ERROR_SUCCESS&&website[0])settings.website=website;
    const auto flag=[](const wchar_t* name,DWORD fallback){DWORD value=fallback,size=sizeof(value);RegGetValueW(HKEY_CURRENT_USER,settings_key,name,RRF_RT_REG_DWORD,nullptr,&value,&size);return value;};
    settings.clock=flag(L"Clock",1)!=0;settings.reminders=flag(L"Reminders",1)!=0;
    settings.iqama_lead=static_cast<int>(std::min<DWORD>(flag(L"IqamaReminderMinutes",10),60));
    settings.chime=static_cast<int>(std::min<DWORD>(flag(L"Chime",0),2));settings.sunrise=flag(L"Sunrise",1)!=0;
    settings.method=static_cast<int>(std::min<DWORD>(flag(L"Method",0),16));settings.calculate=flag(L"Calculate",1)!=0;
    wchar_t place[128]{};DWORD bytes=sizeof(place);
    if(RegGetValueW(HKEY_CURRENT_USER,settings_key,L"Location",RRF_RT_REG_SZ,nullptr,place,&bytes)==ERROR_SUCCESS) {
        double latitude=0,longitude=0;
        if(swscanf(place,L"%lf , %lf",&latitude,&longitude)==2&&std::abs(latitude)<=90&&std::abs(longitude)<=180){settings.latitude=latitude;settings.longitude=longitude;}
    }
    return settings;
}
void save_prayer_settings(const PrayerSettings& settings) {
    RegSetKeyValueW(HKEY_CURRENT_USER,settings_key,L"Website",REG_SZ,settings.website.c_str(),static_cast<DWORD>((settings.website.size()+1)*sizeof(wchar_t)));
    const auto flag=[](const wchar_t* name,DWORD value){RegSetKeyValueW(HKEY_CURRENT_USER,settings_key,name,REG_DWORD,&value,sizeof(value));};
    flag(L"Clock",settings.clock);flag(L"Reminders",settings.reminders);flag(L"IqamaReminderMinutes",static_cast<DWORD>(settings.iqama_lead));
    flag(L"Chime",static_cast<DWORD>(settings.chime));flag(L"Sunrise",settings.sunrise);flag(L"Method",static_cast<DWORD>(settings.method));flag(L"Calculate",settings.calculate);
    wchar_t place[128]{};swprintf(place,128,L"%.4f, %.4f",settings.latitude,settings.longitude);
    RegSetKeyValueW(HKEY_CURRENT_USER,settings_key,L"Location",REG_SZ,place,static_cast<DWORD>((wcslen(place)+1)*sizeof(wchar_t)));
}

struct PrayerService::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    PrayerSnapshot snapshot;
    bool requested=false,forced=false,stopping=false,busy=false;
    std::atomic<HWND> notify=nullptr;
    std::thread worker;
    PrayerSettings settings;
    // Worker thread only.
    ULONGLONG last_success=0,last_attempt=0;bool failed=false;

    void post(){if(const auto window=notify.load())PostMessageW(window,prayer_times_message,0,0);}
    // Cache: "v2|Y-M-D|updated|method" then "salah begin adhan iqama" lines.
    void load_cache() {
        const auto path=cache_path();std::ifstream file(path,std::ios::binary);if(!file)return;
        std::string header;std::getline(file,header);
        PrayerDay day;std::string updated,method;
        if(header.rfind("v2|",0)==0) {
            std::istringstream fields(header.substr(3));std::string date;
            std::getline(fields,date,'|');std::getline(fields,updated,'|');std::getline(fields,method);
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
    void save_cache(const PrayerDay& day,const std::wstring& updated,const std::wstring& method) {
        const auto path=cache_path();if(path.empty())return;
        std::error_code ignored;std::filesystem::create_directories(path.parent_path(),ignored);
        std::ofstream file(path,std::ios::binary|std::ios::trunc);if(!file)return;
        file<<"v2|"<<day.year<<'-'<<day.month<<'-'<<day.day<<'|'<<narrow(updated)<<'|'<<narrow(method)<<'\n';
        for(size_t s=0;s<day.times.size();++s)file<<s<<' '<<day.times[s].begin<<' '<<day.times[s].adhan<<' '<<day.times[s].iqama<<'\n';
    }
    bool due(bool force) {
        if(force)return true;
        const auto now=GetTickCount64();SYSTEMTIME t{};GetLocalTime(&t);
        PrayerSnapshot s;{std::lock_guard lock(mutex);s=snapshot;}
        const bool today=s.have&&s.day.year==t.wYear&&s.day.month==t.wMonth&&s.day.day==t.wDay;
        if(failed&&now-last_attempt<retry_interval&&today)return false;
        if(failed&&now-last_attempt<retry_interval&&!settings.calculate)return false;
        return !today||!last_success||now-last_success>=refresh_interval;
    }
    static double utc_offset_hours() {
        TIME_ZONE_INFORMATION zone{};const auto mode=GetTimeZoneInformation(&zone);
        long bias=zone.Bias+(mode==TIME_ZONE_ID_DAYLIGHT?zone.DaylightBias:mode==TIME_ZONE_ID_STANDARD?zone.StandardBias:0);
        return -static_cast<double>(bias)/60;
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
    // Tries each way of reading the times (or only the chosen one), then the
    // calculated fallback. Only the configured website and the widget hosts it
    // names are contacted.
    void fetch() {
        last_attempt=GetTickCount64();
        PrayerSettings s;{std::lock_guard lock(mutex);s=settings;}
        // No masjid configured: nothing is downloaded, calculated or shown.
        if(s.website.empty()){{std::lock_guard lock(mutex);snapshot=PrayerSnapshot{};}failed=false;last_success=GetTickCount64();post();return;}
        SYSTEMTIME t{};GetLocalTime(&t);
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
        if(day) {
            day->year=t.wYear;day->month=t.wMonth;day->day=t.wDay;
            const auto updated=local_time_text();save_cache(*day,updated,method);
            failed=false;last_success=GetTickCount64();
            std::lock_guard lock(mutex);snapshot.have=true;snapshot.day=*day;snapshot.updated=updated;snapshot.method=method;snapshot.error.clear();
        } else {
            failed=true;
            if(error.empty())error=page_loaded&&!page.empty()?L"No prayer times found on the website":L"Couldn't read the prayer times";
            std::lock_guard lock(mutex);snapshot.error=error;
            const bool today=snapshot.have&&snapshot.day.year==t.wYear&&snapshot.day.month==t.wMonth&&snapshot.day.day==t.wDay;
            // No times for today: calculate them from the location (Adhan only).
            if(!today&&s.calculate) {
                snapshot.have=true;snapshot.day=calculate_day(t.wYear,t.wMonth,t.wDay,s.latitude,s.longitude,utc_offset_hours());
                snapshot.method=L"Calculated (ISNA)";snapshot.updated=local_time_text();
            }
        }
        post();
    }
    void run() {
        load_cache();post();
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
    }
};
PrayerService::PrayerService():impl_(std::make_unique<Impl>()){}
PrayerService::~PrayerService(){stop();}
void PrayerService::start(HWND notify,const PrayerSettings& settings) {
    stop();auto& i=*impl_;i.notify=notify;i.settings=settings;i.stopping=false;i.last_success=0;i.failed=false;
    i.worker=std::thread([&i]{i.run();});
    refresh(false);
}
void PrayerService::stop() {
    auto& i=*impl_;i.notify=nullptr;if(!i.worker.joinable())return;
    {std::lock_guard lock(i.mutex);i.stopping=true;}
    i.wake.notify_all();i.worker.join();
}
void PrayerService::refresh(bool force) {
    auto& i=*impl_;
    {std::lock_guard lock(i.mutex);if(i.busy&&!force)return;i.requested=true;i.forced|=force;}
    i.wake.notify_all();
}
PrayerSnapshot PrayerService::snapshot() const{std::lock_guard lock(impl_->mutex);return impl_->snapshot;}
}
