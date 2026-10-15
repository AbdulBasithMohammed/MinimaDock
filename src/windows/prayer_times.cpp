#include "windows/prayer_times.h"
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
struct Internet {HINTERNET handle=nullptr;~Internet(){if(handle)WinHttpCloseHandle(handle);}};
// One HTTPS GET with a 4 MB cap; redirects are followed by WinHTTP.
std::optional<std::string> http_get(const std::wstring& url) {
    URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);
    wchar_t host[256]{},path[2048]{},extra[2048]{};
    parts.lpszHostName=host;parts.dwHostNameLength=256;parts.lpszUrlPath=path;parts.dwUrlPathLength=2048;parts.lpszExtraInfo=extra;parts.dwExtraInfoLength=2048;
    if(!WinHttpCrackUrl(url.c_str(),0,0,&parts)||(parts.nScheme!=INTERNET_SCHEME_HTTPS&&parts.nScheme!=INTERNET_SCHEME_HTTP))return std::nullopt;
    constexpr wchar_t agent[]=L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) MinimaDock";
    Internet session;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    session.handle=WinHttpOpen(agent,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
#endif
    if(!session.handle)session.handle=WinHttpOpen(agent,WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session.handle)return std::nullopt;
    WinHttpSetTimeouts(session.handle,8000,8000,15000,15000);
    Internet connection;connection.handle=WinHttpConnect(session.handle,host,parts.nPort,0);if(!connection.handle)return std::nullopt;
    const std::wstring object=std::wstring(path)+extra;
    Internet request;request.handle=WinHttpOpenRequest(connection.handle,L"GET",object.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
    if(!request.handle||!WinHttpSendRequest(request.handle,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.handle,nullptr))return std::nullopt;
    DWORD status=0,size=sizeof(status);
    if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)||status!=200)return std::nullopt;
    std::string body;
    for(;;) {
        DWORD available=0;if(!WinHttpQueryDataAvailable(request.handle,&available))return std::nullopt;
        if(!available)break;if(body.size()+available>4*1024*1024)return std::nullopt;
        const auto at=body.size();body.resize(at+available);DWORD read=0;
        if(!WinHttpReadData(request.handle,body.data()+at,available,&read))return std::nullopt;
        body.resize(at+read);
    }
    return body;
}
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
    return settings;
}
void save_prayer_settings(const PrayerSettings& settings) {
    RegSetKeyValueW(HKEY_CURRENT_USER,settings_key,L"Website",REG_SZ,settings.website.c_str(),static_cast<DWORD>((settings.website.size()+1)*sizeof(wchar_t)));
    const auto flag=[](const wchar_t* name,DWORD value){RegSetKeyValueW(HKEY_CURRENT_USER,settings_key,name,REG_DWORD,&value,sizeof(value));};
    flag(L"Clock",settings.clock);flag(L"Reminders",settings.reminders);flag(L"IqamaReminderMinutes",static_cast<DWORD>(settings.iqama_lead));
}

struct PrayerService::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    PrayerSnapshot snapshot;
    bool requested=false,forced=false,stopping=false,busy=false;
    std::atomic<HWND> notify=nullptr;
    std::thread worker;
    std::wstring website;
    // Worker thread only.
    std::wstring widget;
    ULONGLONG last_success=0,last_attempt=0;bool failed=false;

    void post(){if(const auto window=notify.load())PostMessageW(window,prayer_times_message,0,0);}
    void load_cache() {
        const auto path=cache_path();std::ifstream file(path,std::ios::binary);if(!file)return;
        std::string header;std::getline(file,header);
        const std::string body((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        int year=0,month=0,day=0;char updated[64]{};
        if(std::sscanf(header.c_str(),"%d-%d-%d|%63[^\r\n]",&year,&month,&day,updated)<3)return;
        auto parsed=parse_galaxystream(body);if(!parsed)return;
        parsed->year=year;parsed->month=month;parsed->day=day;
        std::lock_guard lock(mutex);snapshot.have=true;snapshot.day=*parsed;snapshot.updated=widen(updated);
    }
    void save_cache(const std::string& body,const PrayerDay& day,const std::wstring& updated) {
        const auto path=cache_path();if(path.empty())return;
        std::error_code ignored;std::filesystem::create_directories(path.parent_path(),ignored);
        std::ofstream file(path,std::ios::binary|std::ios::trunc);if(!file)return;
        file<<day.year<<'-'<<day.month<<'-'<<day.day<<'|'<<narrow(updated)<<'\n'<<body;
    }
    bool due(bool force) {
        if(force)return true;
        const auto now=GetTickCount64();SYSTEMTIME t{};GetLocalTime(&t);
        PrayerSnapshot s;{std::lock_guard lock(mutex);s=snapshot;}
        const bool today=s.have&&s.day.year==t.wYear&&s.day.month==t.wMonth&&s.day.day==t.wDay;
        if(failed&&now-last_attempt<retry_interval)return false;
        return !today||!last_success||now-last_success>=refresh_interval;
    }
    void fetch() {
        last_attempt=GetTickCount64();std::wstring error;std::optional<PrayerDay> day;std::string body;
        if(widget.empty()) {
            if(website.find(L"PrayerTimesJS.asp")!=std::wstring::npos)widget=website;
            else if(const auto page=http_get(website)) {if(const auto found=find_widget(*page))widget=*found;else error=L"The website has no prayer times widget";}
            else error=L"Couldn't reach the masjid website";
        }
        if(!widget.empty()) {
            if(auto script=http_get(widget)) {
                if((day=parse_galaxystream(*script)))body=std::move(*script);
                else {error=L"The prayer times format changed";widget.clear();}
            } else {error=L"Couldn't download prayer times";widget.clear();}
        }
        if(day) {
            SYSTEMTIME t{};GetLocalTime(&t);day->year=t.wYear;day->month=t.wMonth;day->day=t.wDay;
            const auto updated=local_time_text();save_cache(body,*day,updated);
            failed=false;last_success=GetTickCount64();
            std::lock_guard lock(mutex);snapshot.have=true;snapshot.day=*day;snapshot.updated=updated;snapshot.error.clear();
        } else {
            failed=true;std::lock_guard lock(mutex);snapshot.error=error;
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
void PrayerService::start(HWND notify,std::wstring website) {
    stop();auto& i=*impl_;i.notify=notify;i.website=std::move(website);i.widget.clear();i.stopping=false;
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
