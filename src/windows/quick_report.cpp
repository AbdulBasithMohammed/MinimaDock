#include "windows/connectivity.h"
#include "windows/updates.h"
#include "windows/volume.h"
#include "windows/prayer_times.h"
#include <string>

namespace minima {
// --quick-settings-report <file>: read-only check of what quick settings can
// read on this PC. Counts and flags only; no network or device names.
int run_quick_settings_report(const wchar_t* report) {
    Connectivity connectivity;UpdateMonitor updates;VolumeControl volume;
    connectivity.start(nullptr);connectivity.refresh(true);updates.refresh(true);volume.start(nullptr);
    ConnectivitySnapshot s;
    for(int i=0;i<40;++i){Sleep(250);s=connectivity.snapshot();if(i>=12&&(s.wifi_present||s.bluetooth_present))break;}
    UpdateStatus u;for(int i=0;i<80&&!(u=updates.status()).known;++i)Sleep(250);
    // Prayer times: a real download from the configured masjid website.
    PrayerService prayers;prayers.start(nullptr,load_prayer_settings());prayers.refresh(true);
    PrayerSnapshot p;for(int i=0;i<120;++i){Sleep(250);p=prayers.snapshot();if(!p.updated.empty()&&(p.day.year||!p.error.empty()))break;}
    prayers.stop();
    size_t switchable=0,connected=0,saved=0;
    for(const auto& d:s.devices){switchable+=d.switchable;connected+=d.connected;}
    for(const auto& n:s.networks)saved+=n.saved;
    std::wstring text=L"wifi present="+std::to_wstring(s.wifi_present)+L" on="+std::to_wstring(s.wifi_on)+L" connected="+std::to_wstring(s.wifi_connected)+
        L" named="+std::to_wstring(!s.wifi_name.empty())+L" location_denied="+std::to_wstring(s.location_denied)+L" networks="+std::to_wstring(s.networks.size())+L" saved="+std::to_wstring(saved)+L"\n";
    text+=L"bluetooth present="+std::to_wstring(s.bluetooth_present)+L" on="+std::to_wstring(s.bluetooth_on)+L" devices="+std::to_wstring(s.devices.size())+
        L" connected="+std::to_wstring(connected)+L" audio_switchable="+std::to_wstring(switchable)+L"\n";
    text+=L"mic available="+std::to_wstring(volume.mic_available())+L" muted="+std::to_wstring(volume.mic_muted())+L"\n";
    const auto minutes=[](int m){return m<0?std::wstring(L"-"):std::to_wstring(m/60)+L":"+(m%60<10?L"0":L"")+std::to_wstring(m%60);};
    text+=L"prayer have="+std::to_wstring(p.have)+L" date="+std::to_wstring(p.day.year)+L"-"+std::to_wstring(p.day.month)+L"-"+std::to_wstring(p.day.day)+L" error="+p.error+L"\n";
    if(p.have)for(const auto s:{Salah::Fajr,Salah::Sunrise,Salah::Zuhr,Salah::Asr,Salah::Maghrib,Salah::Isha,Salah::Jumah1})
        text+=std::wstring(L"  ")+salah_name(s)+L" begin="+minutes(p.day[s].begin)+L" adhan="+minutes(p.day[s].adhan)+L" iqama="+minutes(p.day[s].iqama)+L"\n";
    text+=L"updates known="+std::to_wstring(u.known)+L" restart="+std::to_wstring(u.restart)+L" pending="+std::to_wstring(u.pending)+L"\n";
    volume.stop();connectivity.stop();updates.stop();
    const int size=WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string bytes(static_cast<size_t>(size>0?size:0),'\0');
    if(size>0)WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),bytes.data(),size,nullptr,nullptr);
    const auto file=CreateFileW(report,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return 1;
    DWORD written=0;const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)!=FALSE;
    CloseHandle(file);return ok?0:1;
}
}
