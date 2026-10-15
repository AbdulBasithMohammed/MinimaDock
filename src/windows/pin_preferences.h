#pragma once
#include "windows/inventory.h"
#include <algorithm>
#include <array>
#include <cwctype>

namespace minima {
// Dock-only overrides: never rewrite Explorer's Taskband registry or shortcuts.
inline constexpr wchar_t pin_preferences_key[]=L"Software\\MinimaDock\\Pins";
inline std::wstring pin_identity(const Shortcut& pin) {
    if(!pin.app_id.empty())return L"app:"+pin.app_id;
    auto value=pin.target.empty()?pin.path:pin.target;
    std::transform(value.begin(),value.end(),value.begin(),towlower);
    return value.empty()?std::wstring{}:L"path:"+value;
}
inline std::wstring pin_string(HKEY key,const wchar_t* name) {
    DWORD bytes=0;
    if(RegGetValueW(key,nullptr,name,RRF_RT_REG_SZ,nullptr,nullptr,&bytes)!=ERROR_SUCCESS||bytes>65536||bytes<sizeof(wchar_t))return {};
    std::wstring value(bytes/sizeof(wchar_t),L'\0');
    if(RegGetValueW(key,nullptr,name,RRF_RT_REG_SZ,nullptr,value.data(),&bytes)!=ERROR_SUCCESS)return {};
    value.resize(wcsnlen(value.c_str(),value.size()));return value;
}
// `rank` is the user's drag order (0 = never arranged); `order` is creation order.
struct PinPreference {Shortcut pin;bool pinned;ULONGLONG order;ULONGLONG rank=0;};
inline std::vector<PinPreference> read_pin_preferences(const wchar_t* root=pin_preferences_key) {
    HKEY key=nullptr;std::vector<PinPreference> values;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,root,0,KEY_READ,&key)!=ERROR_SUCCESS)return values;
    for(DWORD i=0;i<1024;++i) {
        wchar_t name[64]{};DWORD length=64;
        if(RegEnumKeyExW(key,i,name,&length,nullptr,nullptr,nullptr,nullptr)!=ERROR_SUCCESS)break;
        HKEY item=nullptr;if(RegOpenKeyExW(key,name,0,KEY_READ,&item)!=ERROR_SUCCESS)continue;
        DWORD state=0,bytes=sizeof(state);ULONGLONG order=0,rank=0;DWORD order_bytes=sizeof(order),rank_bytes=sizeof(rank);
        if(RegGetValueW(item,nullptr,L"State",RRF_RT_REG_DWORD,nullptr,&state,&bytes)==ERROR_SUCCESS&&(state==1||state==2)) {
            Shortcut pin{pin_string(item,L"Name"),pin_string(item,L"Path"),false,pin_string(item,L"Target"),{},pin_string(item,L"AppId")};
            RegGetValueW(item,nullptr,L"Order",RRF_RT_REG_QWORD,nullptr,&order,&order_bytes);
            RegGetValueW(item,nullptr,L"Rank",RRF_RT_REG_QWORD,nullptr,&rank,&rank_bytes);
            const auto identity=pin_identity(pin);
            if(!pin.title.empty()&&!pin.path.empty()&&!identity.empty()&&pin_string(item,L"Identity")==identity)values.push_back({std::move(pin),state==1,order,rank});
        }
        RegCloseKey(item);
    }
    RegCloseKey(key);
    std::stable_sort(values.begin(),values.end(),[](const auto& a,const auto& b){return a.order<b.order;});return values;
}
// Next stable order: one past the largest stored value. Unlike a tick count this
// survives reboots and keeps new pins after older ones.
inline ULONGLONG next_pin_order(const wchar_t* root) {
    HKEY key=nullptr;ULONGLONG highest=0;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,root,0,KEY_READ,&key)!=ERROR_SUCCESS)return 1;
    for(DWORD i=0;i<1024;++i) {
        wchar_t name[64]{};DWORD length=64;
        if(RegEnumKeyExW(key,i,name,&length,nullptr,nullptr,nullptr,nullptr)!=ERROR_SUCCESS)break;
        ULONGLONG order=0;DWORD bytes=sizeof(order);
        if(RegGetValueW(key,name,L"Order",RRF_RT_REG_QWORD,nullptr,&order,&bytes)==ERROR_SUCCESS)highest=std::max(highest,order);
    }
    RegCloseKey(key);return highest+1;
}
// Identity determines a safe fixed-size key; the full identity is verified on reading.
inline std::wstring pin_key(const std::wstring& identity,const wchar_t* root) {
    ULONGLONG hash=14695981039346656037ULL;
    for(const auto c:identity){hash^=static_cast<unsigned short>(c);hash*=1099511628211ULL;}
    wchar_t name[32]{};swprintf(name,32,L"%016llx",hash);
    return std::wstring(root)+L"\\"+name;
}
inline bool save_pin_preference(const Shortcut& pin,bool pinned,const wchar_t* root=pin_preferences_key) {
    const auto identity=pin_identity(pin);if(identity.empty()||pin.path.empty()||pin.title.empty())return false;
    const auto path=pin_key(identity,root);HKEY key=nullptr;const auto next_order=next_pin_order(root);
    if(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    const auto stored=pin_string(key,L"Identity");
    if(!stored.empty()&&stored!=identity){RegCloseKey(key);return false;}
    bool ok=true;
    const auto put=[&](const wchar_t* field,const std::wstring& value){if(RegSetValueExW(key,field,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)))!=ERROR_SUCCESS)ok=false;};
    put(L"Identity",identity);put(L"Name",pin.title);put(L"Path",pin.path);put(L"Target",pin.target);put(L"AppId",pin.app_id);
    ULONGLONG order=0;DWORD bytes=sizeof(order);
    if(RegGetValueW(key,nullptr,L"Order",RRF_RT_REG_QWORD,nullptr,&order,&bytes)!=ERROR_SUCCESS){order=next_order;if(RegSetValueExW(key,L"Order",0,REG_QWORD,reinterpret_cast<const BYTE*>(&order),sizeof(order))!=ERROR_SUCCESS)ok=false;}
    const DWORD state=pinned?1:2;
    if(ok)ok=RegSetValueExW(key,L"State",0,REG_DWORD,reinterpret_cast<const BYTE*>(&state),sizeof(state))==ERROR_SUCCESS;
    RegCloseKey(key);return ok;
}
// "Pin to taskbar" in Windows after "Unpin from dock": Windows' newer choice wins,
// so the dock-only unpin for that identity is removed.
inline void forget_unpin(const std::wstring& identity,const wchar_t* root=pin_preferences_key) {
    if(identity.empty())return;
    const auto path=pin_key(identity,root);HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,path.c_str(),0,KEY_READ,&key)!=ERROR_SUCCESS)return;
    DWORD state=0,bytes=sizeof(state);
    const bool unpinned=RegGetValueW(key,nullptr,L"State",RRF_RT_REG_DWORD,nullptr,&state,&bytes)==ERROR_SUCCESS&&state==2&&pin_string(key,L"Identity")==identity;
    RegCloseKey(key);
    if(unpinned)RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str());
}
// Drag-to-reorder: pins every item (dock-only override) and stores its rank.
inline bool save_pin_ranks(const std::vector<Shortcut>& pins,const wchar_t* root=pin_preferences_key) {
    bool ok=true;
    for(size_t i=0;i<pins.size();++i) {
        if(!save_pin_preference(pins[i],true,root)){ok=false;continue;}
        HKEY key=nullptr;const ULONGLONG rank=i+1;
        if(RegOpenKeyExW(HKEY_CURRENT_USER,pin_key(pin_identity(pins[i]),root).c_str(),0,KEY_WRITE,&key)!=ERROR_SUCCESS){ok=false;continue;}
        if(RegSetValueExW(key,L"Rank",0,REG_QWORD,reinterpret_cast<const BYTE*>(&rank),sizeof(rank))!=ERROR_SUCCESS)ok=false;
        RegCloseKey(key);
    }
    return ok;
}
inline void apply_pin_preferences(Inventory& inventory,const wchar_t* root=pin_preferences_key) {
    const auto preferences=read_pin_preferences(root);
    for(const auto& preference:preferences) {
        const auto identity=pin_identity(preference.pin);
        const auto same=[&](const Shortcut& pin){return pin_identity(pin)==identity;};
        const auto found=std::find_if(inventory.pinned.begin(),inventory.pinned.end(),same);
        if(!preference.pinned) {if(found!=inventory.pinned.end())inventory.pinned.erase(found);}
        else if(found==inventory.pinned.end())inventory.pinned.push_back(preference.pin);
    }
    // Arranged pins follow their rank; never-arranged pins keep their relative
    // order after them, so users who never drag see no change.
    const auto rank=[&](const Shortcut& pin){
        const auto identity=pin_identity(pin);
        for(const auto& preference:preferences)if(preference.pinned&&preference.rank&&pin_identity(preference.pin)==identity)return preference.rank;
        return ~0ULL;
    };
    std::stable_sort(inventory.pinned.begin(),inventory.pinned.end(),[&](const Shortcut& a,const Shortcut& b){return rank(a)<rank(b);});
}
}
