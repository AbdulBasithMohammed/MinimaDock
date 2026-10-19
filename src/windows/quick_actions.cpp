#include "windows/quick_actions.h"
#include <shellapi.h>
#include <tlhelp32.h>
#include <ras.h>
#include <raserror.h>
#include <objbase.h>
#include <cwctype>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace minima {
namespace {
constexpr wchar_t personalize[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
std::wstring layout_label(HKL layout) {
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{},name[16]{};
    const auto language=static_cast<LANGID>(reinterpret_cast<UINT_PTR>(layout)&0xFFFF);
    if(!LCIDToLocaleName(MAKELCID(language,SORT_DEFAULT),locale,LOCALE_NAME_MAX_LENGTH,0))return {};
    if(!GetLocaleInfoEx(locale,LOCALE_SISO639LANGNAME2,name,16))return {};
    std::wstring label=name;
    for(auto& c:label)c=static_cast<wchar_t>(std::towupper(c));
    return label;
}
HKL window_layout(HWND window) {
    if(!IsWindow(window))window=GetForegroundWindow();
    return GetKeyboardLayout(window?GetWindowThreadProcessId(window,nullptr):0);
}
}

DisplayState display_state() {
    DisplayState state;
    UINT32 paths=0,modes=0;
    // All paths: every connected target counts once, whether it is lit or not.
    if(GetDisplayConfigBufferSizes(QDC_ALL_PATHS,&paths,&modes)==ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
        if(QueryDisplayConfig(QDC_ALL_PATHS,&paths,path.data(),&modes,mode.data(),nullptr)==ERROR_SUCCESS) {
            std::set<std::pair<unsigned long long,UINT32>> targets;
            for(UINT32 i=0;i<paths;++i) {
                const auto& t=path[i].targetInfo;
                if(t.targetAvailable)targets.insert({static_cast<unsigned long long>(static_cast<unsigned>(t.adapterId.HighPart))<<32|t.adapterId.LowPart,t.id});
            }
            state.displays=static_cast<int>(targets.size());
        }
    }
    paths=modes=0;
    if(GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT,&paths,&modes)==ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
        DISPLAYCONFIG_TOPOLOGY_ID topology{};
        if(QueryDisplayConfig(QDC_DATABASE_CURRENT,&paths,path.data(),&modes,mode.data(),&topology)==ERROR_SUCCESS)
            switch(topology) {
                case DISPLAYCONFIG_TOPOLOGY_INTERNAL:state.mode=DisplayMode::Internal;break;
                case DISPLAYCONFIG_TOPOLOGY_CLONE:state.mode=DisplayMode::Clone;break;
                case DISPLAYCONFIG_TOPOLOGY_EXTEND:state.mode=DisplayMode::Extend;break;
                case DISPLAYCONFIG_TOPOLOGY_EXTERNAL:state.mode=DisplayMode::External;break;
                default:break;
            }
    }
    return state;
}

void set_display_mode(DisplayMode mode) {
    UINT32 flag=0;
    switch(mode) {
        case DisplayMode::Internal:flag=SDC_TOPOLOGY_INTERNAL;break;
        case DisplayMode::Clone:flag=SDC_TOPOLOGY_CLONE;break;
        case DisplayMode::Extend:flag=SDC_TOPOLOGY_EXTEND;break;
        case DisplayMode::External:flag=SDC_TOPOLOGY_EXTERNAL;break;
        default:return;
    }
    // SetDisplayConfig can take a second or more while monitors retrain.
    std::thread([flag]{
        if(SetDisplayConfig(0,nullptr,0,nullptr,SDC_APPLY|flag)!=ERROR_SUCCESS)open_settings(L"ms-settings:display");
    }).detach();
}

bool dark_mode() {
    DWORD light=1,size=sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,personalize,L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
    return light==0;
}

bool process_running(const wchar_t* exe) {
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return false;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool found=false;
    for(bool more=Process32FirstW(snapshot,&entry)!=FALSE;more&&!found;more=Process32NextW(snapshot,&entry)!=FALSE)
        found=_wcsicmp(entry.szExeFile,exe)==0;
    CloseHandle(snapshot);return found;
}

int keyboard_layouts(){return GetKeyboardLayoutList(0,nullptr);}
std::wstring keyboard_label(HWND window){return layout_label(window_layout(window));}
std::wstring next_keyboard_layout(HWND window) {
    const int count=GetKeyboardLayoutList(0,nullptr);if(count<2)return keyboard_label(window);
    std::vector<HKL> layouts(static_cast<std::size_t>(count));
    GetKeyboardLayoutList(count,layouts.data());
    const auto current=window_layout(window);
    std::size_t i=0;while(i<layouts.size()&&layouts[i]!=current)++i;
    const auto next=layouts[(i+1)%layouts.size()];
    if(!IsWindow(window))window=GetForegroundWindow();
    if(window)PostMessageW(window,WM_INPUTLANGCHANGEREQUEST,0,reinterpret_cast<LPARAM>(next));
    return layout_label(next);
}

bool energy_saver_on(){SYSTEM_POWER_STATUS s{};return GetSystemPowerStatus(&s)&&s.SystemStatusFlag==1;}

bool vpn_connected() {
    RASCONNW connection{};connection.dwSize=sizeof(connection);
    DWORD size=sizeof(connection),count=0;
    const auto result=RasEnumConnectionsW(&connection,&size,&count);
    return result==ERROR_BUFFER_TOO_SMALL||(result==ERROR_SUCCESS&&count>0);
}

bool has_rotation_sensor() {
    AR_STATE state{};
    return GetAutoRotationState(&state)&&!(state&(AR_NOSENSOR|AR_NOT_SUPPORTED));
}

void open_settings(const wchar_t* uri) {
    // ShellExecute may use COM (URI association handlers).
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",uri,nullptr,nullptr,SW_SHOWNORMAL))<=32)
        ShellExecuteW(nullptr,L"open",L"ms-settings:",nullptr,nullptr,SW_SHOWNORMAL);
    if(SUCCEEDED(com))CoUninitialize();
}
}
