#include "windows/app_update.h"
#include "windows/http.h"
#include <shellapi.h>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace minima {
namespace {
constexpr wchar_t settings_key[]=L"Software\\MinimaDock\\Settings";
constexpr wchar_t latest_release[]=L"https://api.github.com/repos/AbdulBasithMohammed/MinimaDock/releases/latest";
constexpr std::string_view download_prefix="https://github.com/AbdulBasithMohammed/MinimaDock/releases/download/";
constexpr std::chrono::minutes first_check{1},check_interval{24*60},retry_interval{60};

std::wstring widen(const std::string& s){return std::wstring(s.begin(),s.end());}
std::wstring local_time_text() {
    wchar_t text[64]{};GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,nullptr,nullptr,text,64);return text;
}
// The JSON string value after `"key":` starting at `from` (GitHub escapes no URL characters).
std::optional<std::string> json_string(const std::string& json,std::string_view key,size_t& from) {
    const auto name="\""+std::string(key)+"\"";
    auto at=json.find(name,from);if(at==std::string::npos)return std::nullopt;
    at=json.find(':',at+name.size());if(at==std::string::npos)return std::nullopt;
    const auto open=json.find('"',at);if(open==std::string::npos)return std::nullopt;
    const auto close=json.find('"',open+1);if(close==std::string::npos||close-open>600)return std::nullopt;
    from=close+1;return json.substr(open+1,close-open-1);
}
// Only plain release download URLs from this repository are ever fetched.
bool safe_download(const std::string& url) {
    if(url.rfind(download_prefix,0)!=0)return false;
    for(const char c:url)if(!(std::isalnum(static_cast<unsigned char>(c))||c=='/'||c=='.'||c==':'||c=='-'||c=='_'))return false;
    return true;
}
struct Release {std::wstring version;std::string zip,sha;};
std::optional<Release> parse_release(const std::string& json) {
    size_t at=0;const auto tag=json_string(json,"tag_name",at);if(!tag||tag->empty())return std::nullopt;
    Release r;r.version=widen(tag->front()=='v'||tag->front()=='V'?tag->substr(1):*tag);
    for(const auto c:r.version)if(!std::iswdigit(c)&&c!=L'.')return std::nullopt;
    for(size_t from=0;;) {
        const auto url=json_string(json,"browser_download_url",from);if(!url)break;
        if(!safe_download(*url))continue;
        const auto name=url->substr(url->rfind('/')+1);
        if(name.rfind("MinimaDock-",0)!=0)continue;
        if(name.size()>4&&name.compare(name.size()-4,4,".zip")==0)r.zip=*url;
        else if(name.size()>11&&name.compare(name.size()-11,11,".zip.sha256")==0)r.sha=*url;
    }
    if(r.zip.empty()||r.sha.empty())return std::nullopt;
    return r;
}
}

int compare_versions(const std::wstring& a,const std::wstring& b) {
    const auto parts=[](const std::wstring& v){std::vector<unsigned long> n;size_t i=0;while(i<v.size()){n.push_back(std::wcstoul(v.c_str()+i,nullptr,10));const auto dot=v.find(L'.',i);if(dot==std::wstring::npos)break;i=dot+1;}while(n.size()<4)n.push_back(0);return n;};
    const auto x=parts(a),y=parts(b);
    for(size_t i=0;i<std::min(x.size(),y.size());++i)if(x[i]!=y[i])return x[i]<y[i]?-1:1;
    return 0;
}
bool load_auto_update_setting() {
    DWORD value=1,size=sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,settings_key,L"AutoUpdate",RRF_RT_REG_DWORD,nullptr,&value,&size);
    return value!=0;
}
void save_auto_update_setting(bool on) {
    const DWORD value=on?1:0;
    RegSetKeyValueW(HKEY_CURRENT_USER,settings_key,L"AutoUpdate",REG_DWORD,&value,sizeof(value));
}

struct AppUpdater::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    HWND notify=nullptr;
    bool automatic=true,requested=false,stopping=false;
    AppUpdateStatus state;Release release;
    void run() {
        auto due=std::chrono::steady_clock::now()+first_check;
        for(;;) {
            {
                std::unique_lock lock(mutex);
                const auto ready=[&]{return stopping||requested;};
                if(automatic)wake.wait_until(lock,due,ready);else wake.wait(lock,[&]{return stopping||requested||automatic;});
                if(stopping)return;
                if(!requested&&(!automatic||std::chrono::steady_clock::now()<due))continue;
                requested=false;state.checking=true;
            }
            post();
            const auto body=http_get(latest_release);
            const auto found=body?parse_release(*body):std::nullopt;
            {
                std::lock_guard lock(mutex);state.checking=false;
                if(found) {
                    release=*found;state.checked=true;state.error.clear();state.latest=found->version;state.checked_at=local_time_text();
                    state.available=compare_versions(found->version,app_version)>0;
                } else state.error=body?L"GitHub's answer could not be read":L"GitHub could not be reached";
                due=std::chrono::steady_clock::now()+(found?check_interval:retry_interval);
            }
            post();
        }
    }
    void post(){if(notify)PostMessageW(notify,app_update_message,0,0);}
};
AppUpdater::AppUpdater():impl_(std::make_unique<Impl>()){}
AppUpdater::~AppUpdater(){stop();}
void AppUpdater::start(HWND notify,bool automatic) {
    stop();auto& i=*impl_;i.notify=notify;i.automatic=automatic;i.stopping=false;i.requested=false;
    i.worker=std::thread([&i]{i.run();});
}
void AppUpdater::stop() {
    auto& i=*impl_;if(!i.worker.joinable())return;
    {std::lock_guard lock(i.mutex);i.stopping=true;}i.wake.notify_all();i.worker.join();
}
void AppUpdater::set_automatic(bool automatic){{std::lock_guard lock(impl_->mutex);impl_->automatic=automatic;}impl_->wake.notify_all();}
void AppUpdater::check(){{std::lock_guard lock(impl_->mutex);impl_->requested=true;}impl_->wake.notify_all();}
AppUpdateStatus AppUpdater::status() const{std::lock_guard lock(impl_->mutex);return impl_->state;}
bool AppUpdater::install(HWND owner) const {
    Release r;{std::lock_guard lock(impl_->mutex);if(!impl_->state.available)return false;r=impl_->release;}
    if(!safe_download(r.zip)||!safe_download(r.sha))return false;
    wchar_t temp[MAX_PATH+1]{};if(!GetTempPathW(MAX_PATH+1,temp))return false;
    const auto folder=std::filesystem::path(temp)/L"MinimaDock-update";
    std::error_code ignored;std::filesystem::create_directories(folder,ignored);
    const auto script=folder/L"update.ps1";
    {
        std::ofstream file(script,std::ios::binary|std::ios::trunc);if(!file)return false;
        const std::string version(r.version.begin(),r.version.end());
        file<<"$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'\r\n"
              "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12\r\n"
              "$host.UI.RawUI.WindowTitle='Updating MinimaDock'\r\n"
              "try {\r\n"
              "    Write-Host 'Downloading MinimaDock "<<version<<"...'\r\n"
              "    $zip=Join-Path $PSScriptRoot 'MinimaDock.zip'\r\n"
              "    Invoke-WebRequest -UseBasicParsing -Uri '"<<r.zip<<"' -OutFile $zip\r\n"
              "    $sum=Invoke-WebRequest -UseBasicParsing -Uri '"<<r.sha<<"'\r\n"
              "    $text=if($sum.Content -is [byte[]]){[Text.Encoding]::ASCII.GetString($sum.Content)}else{[string]$sum.Content}\r\n"
              "    if((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne ($text.Trim() -split '\\s+')[0]){throw 'The download did not match its checksum. Nothing was changed.'}\r\n"
              "    $files=Join-Path $PSScriptRoot 'files'\r\n"
              "    if(Test-Path -LiteralPath $files){Remove-Item -LiteralPath $files -Recurse -Force}\r\n"
              "    Expand-Archive -LiteralPath $zip -DestinationPath $files\r\n"
              "    $installer=Get-ChildItem -LiteralPath $files -Recurse -Filter 'Install-MinimaDock.ps1' | Select-Object -First 1\r\n"
              "    if(-not $installer){throw 'The download has no installer. Nothing was changed.'}\r\n"
              "    & $installer.FullName -Action Install\r\n"
              "    Start-Sleep -Seconds 4\r\n"
              "} catch {\r\n"
              "    Write-Host $_.Exception.Message -ForegroundColor Red\r\n"
              "    Read-Host 'Press Enter to close'\r\n"
              "}\r\n";
        if(!file)return false;
    }
    wchar_t windows[MAX_PATH]{};if(!GetWindowsDirectoryW(windows,MAX_PATH))return false;
    const auto shell=std::filesystem::path(windows)/L"System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    const auto arguments=L"-NoProfile -ExecutionPolicy Bypass -File \""+script.wstring()+L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(owner,L"open",shell.c_str(),arguments.c_str(),folder.c_str(),SW_SHOWNORMAL))>32;
}
}
