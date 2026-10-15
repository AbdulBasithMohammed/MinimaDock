#include "windows/connectivity.h"
#include "windows/media_abi.h"
#include "platform/com_ptr.h"
#include <roapi.h>
#include <asyncinfo.h>
#include <wlanapi.h>
#include <bluetoothapis.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <propsys.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace minima {
namespace {
using media_abi::Async;
GUID guid(const wchar_t* value){GUID result{};IIDFromString(value,&result);return result;}
struct GuidLess {bool operator()(const GUID& a,const GUID& b) const{return std::memcmp(&a,&b,sizeof(GUID))<0;}};

// Windows.Devices.Radios, hand-declared because the toolchain's WinRT headers
// do not compile as C++. Method order and IIDs follow windows.devices.radios.h.
struct IRadio : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE SetStateAsync(INT32 state,Async<INT32>** operation)=0;
    virtual HRESULT STDMETHODCALLTYPE add_StateChanged(IUnknown* handler,INT64* token)=0;
    virtual HRESULT STDMETHODCALLTYPE remove_StateChanged(INT64 token)=0;
    virtual HRESULT STDMETHODCALLTYPE get_State(INT32* state)=0;
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING* name)=0;
    virtual HRESULT STDMETHODCALLTYPE get_Kind(INT32* kind)=0;
};
struct IRadioStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetRadiosAsync(Async<IInspectable*>** operation)=0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceSelector(HSTRING* selector)=0;
    virtual HRESULT STDMETHODCALLTYPE FromIdAsync(HSTRING id,Async<IInspectable*>** operation)=0;
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(Async<INT32>** operation)=0;
};
struct IRadioList : IInspectable { // IVectorView<Radio>
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index,IInspectable** item)=0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size)=0;
};
constexpr INT32 radio_on=1,radio_off=2,kind_wifi=1,kind_bluetooth=3;
constexpr const wchar_t* iid_radio=L"{252118df-b33e-416a-875f-1cf38ae2d83e}";
constexpr const wchar_t* iid_radio_statics=L"{5fb6a12e-67cb-46ae-aae9-65919f86eff4}";
constexpr const wchar_t* iid_radio_list=L"{65066c36-090b-5466-b8e5-e7565dc34175}";

// Kernel-streaming control of a Bluetooth audio filter. KSPROPSETID_BtAudio
// and its one-shot reconnect/disconnect properties are documented in the WDK
// (ksmedia.h); this is how Windows connects paired headphones on request.
struct KsIdentifier {GUID set;ULONG id;ULONG flags;};
struct IKsControl : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsProperty(KsIdentifier* property,ULONG length,void* data,ULONG data_length,ULONG* returned)=0;
};
constexpr IID iid_ks_control={0x28f54685,0x06fd,0x11d2,{0xb2,0x7a,0x00,0xa0,0xc9,0x22,0x31,0x96}};
constexpr GUID ks_bt_audio={0x7fa06c40,0xb8f6,0x4c7e,{0x85,0x56,0xe8,0xc3,0x3a,0x12,0xe5,0x4d}};
constexpr ULONG ks_reconnect=0,ks_disconnect=1,ks_get=1;
constexpr GUID le_device_interface={0x781aee18,0x7733,0x4ce4,{0xad,0xd0,0x91,0xf4,0x1c,0x67,0xb5,0x92}};
constexpr DEVPROPKEY device_container={{0x8c7ed206,0x3f8a,0x4827,{0xb3,0xab,0xae,0x9e,0x1f,0xae,0xfc,0x6c}},2};
constexpr PROPERTYKEY endpoint_container={{0x8c7ed206,0x3f8a,0x4827,{0xb3,0xab,0xae,0x9e,0x1f,0xae,0xfc,0x6c}},2};

// Radio operations complete within milliseconds; this runs on the worker.
template<class T> HRESULT wait(Async<T>* operation,T* value) {
    if(!operation)return E_POINTER;
    ComPtr<IAsyncInfo> info;HRESULT hr=operation->QueryInterface(__uuidof(IAsyncInfo),reinterpret_cast<void**>(info.put()));if(FAILED(hr))return hr;
    AsyncStatus status=Started;
    for(int i=0;i<600&&SUCCEEDED(hr=info->get_Status(&status))&&status==Started;++i)Sleep(5);
    if(FAILED(hr))return hr;
    if(status==Started){info->Cancel();return HRESULT_FROM_WIN32(WAIT_TIMEOUT);}
    if(status==Completed)return operation->GetResults(value);
    if(status==Error){info->get_ErrorCode(&hr);return FAILED(hr)?hr:E_FAIL;}
    return E_ABORT;
}
struct Radios {
    ComPtr<IRadioStatics> statics;
    ComPtr<IRadio> wifi,bluetooth;
    bool asked=false;
    void load() {
        if(!statics) {
            constexpr wchar_t name[]=L"Windows.Devices.Radios.Radio";HSTRING type=nullptr;
            if(FAILED(WindowsCreateString(name,static_cast<UINT32>(std::size(name)-1),&type)))return;
            const auto hr=RoGetActivationFactory(type,guid(iid_radio_statics),reinterpret_cast<void**>(statics.put()));
            WindowsDeleteString(type);if(FAILED(hr)){statics.reset();return;}
        }
        if(!asked){asked=true;ComPtr<Async<INT32>> operation;INT32 access=0;if(SUCCEEDED(statics->RequestAccessAsync(operation.put())))wait(operation.get(),&access);}
        ComPtr<Async<IInspectable*>> operation;IInspectable* raw=nullptr;
        if(FAILED(statics->GetRadiosAsync(operation.put()))||FAILED(wait(operation.get(),&raw))||!raw)return;
        ComPtr<IInspectable> holder;*holder.put()=raw;
        ComPtr<IRadioList> list;if(FAILED(holder->QueryInterface(guid(iid_radio_list),reinterpret_cast<void**>(list.put()))))return;
        wifi.reset();bluetooth.reset();UINT32 count=0;list->get_Size(&count);
        for(UINT32 i=0;i<count;++i) {
            ComPtr<IInspectable> item;ComPtr<IRadio> radio;INT32 kind=0;
            if(FAILED(list->GetAt(i,item.put()))||!item||FAILED(item->QueryInterface(guid(iid_radio),reinterpret_cast<void**>(radio.put())))||FAILED(radio->get_Kind(&kind)))continue;
            if(kind==kind_wifi&&!wifi)wifi=std::move(radio);
            else if(kind==kind_bluetooth&&!bluetooth)bluetooth=std::move(radio);
        }
    }
    static bool on(IRadio* radio){INT32 state=0;return radio&&SUCCEEDED(radio->get_State(&state))&&state==radio_on;}
    static bool set(IRadio* radio,bool value) {
        ComPtr<Async<INT32>> operation;INT32 access=0;
        return radio&&SUCCEEDED(radio->SetStateAsync(value?radio_on:radio_off,operation.put()))&&SUCCEEDED(wait(operation.get(),&access))&&access==1;
    }
};
std::wstring ssid_text(const DOT11_SSID& ssid) {
    const auto bytes=reinterpret_cast<const char*>(ssid.ucSSID);const int length=static_cast<int>(std::min<ULONG>(ssid.uSSIDLength,DOT11_SSID_MAX_LENGTH));
    if(!length)return {};
    UINT page=CP_UTF8;int size=MultiByteToWideChar(page,MB_ERR_INVALID_CHARS,bytes,length,nullptr,0);
    if(size<=0){page=CP_ACP;size=MultiByteToWideChar(page,0,bytes,length,nullptr,0);}
    std::wstring text(static_cast<size_t>(std::max(0,size)),L'\0');
    if(size>0)MultiByteToWideChar(page,page==CP_UTF8?MB_ERR_INVALID_CHARS:0,bytes,length,text.data(),size);
    for(auto& c:text)if(c<32)c=L' ';
    return text;
}
std::wstring xml_escape(const std::wstring& value) {
    std::wstring result;
    for(const auto c:value){switch(c){case L'&':result+=L"&amp;";break;case L'<':result+=L"&lt;";break;case L'>':result+=L"&gt;";break;case L'"':result+=L"&quot;";break;case L'\'':result+=L"&apos;";break;default:result+=c;}}
    return result;
}
std::wstring hex_utf8(const std::wstring& value) {
    const int size=WideCharToMultiByte(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string bytes(static_cast<size_t>(std::max(0,size)),'\0');
    if(size>0)WideCharToMultiByte(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),bytes.data(),size,nullptr,nullptr);
    constexpr wchar_t digits[]=L"0123456789ABCDEF";std::wstring result;
    for(const auto b:bytes){const auto v=static_cast<unsigned char>(b);result+=digits[v>>4];result+=digits[v&15];}
    return result;
}
// WLAN profile names for the networks a password (or nothing) can join.
const wchar_t* profile_auth(int auth,bool secured) {
    if(!secured)return auth==1?L"open":nullptr;
    switch(auth){case 4:return L"WPAPSK";case 7:return L"WPA2PSK";case 9:return L"WPA3SAE";case 10:return L"OWE";default:return nullptr;}
}
const wchar_t* profile_cipher(int cipher,bool secured) {
    if(!secured)return L"none";
    switch(cipher){case 2:return L"TKIP";case 8:return L"GCMP";case 9:return L"GCMP256";default:return L"AES";}
}
BluetoothDevice::Kind kind_from_class(ULONG cod) {
    switch((cod>>8)&0x1f) {
    case 1:return BluetoothDevice::Kind::Computer;
    case 2:return BluetoothDevice::Kind::Phone;
    case 4:return BluetoothDevice::Kind::Audio;
    case 5:{const auto minor=(cod>>6)&3;return minor==2?BluetoothDevice::Kind::Mouse:minor?BluetoothDevice::Kind::Keyboard:BluetoothDevice::Kind::Other;}
    default:return BluetoothDevice::Kind::Other;
    }
}
std::wstring lower(std::wstring value){for(auto& c:value)c=static_cast<wchar_t>(towlower(c));return value;}
bool node_container(DEVINST node,GUID& container) {
    DEVPROPTYPE type=0;ULONG size=sizeof(container);
    return CM_Get_DevNode_PropertyW(node,&device_container,&type,reinterpret_cast<PBYTE>(&container),&size,0)==CR_SUCCESS&&type==DEVPROP_TYPE_GUID;
}
// Paired classic devices' containers, keyed by their 12-digit address.
std::map<std::wstring,GUID> classic_containers() {
    std::map<std::wstring,GUID> result;ULONG length=0;
    constexpr wchar_t filter[]=L"BTHENUM";constexpr ULONG flags=CM_GETIDLIST_FILTER_ENUMERATOR|CM_GETIDLIST_FILTER_PRESENT;
    if(CM_Get_Device_ID_List_SizeW(&length,filter,flags)!=CR_SUCCESS||!length)return result;
    std::wstring ids(length,L'\0');if(CM_Get_Device_ID_ListW(filter,ids.data(),length,flags)!=CR_SUCCESS)return result;
    for(const wchar_t* id=ids.c_str();*id;id+=wcslen(id)+1) {
        constexpr wchar_t prefix[]=L"BTHENUM\\DEV_";constexpr size_t n=std::size(prefix)-1;
        if(wcslen(id)<n+12||_wcsnicmp(id,prefix,n)!=0)continue;
        DEVINST node=0;GUID container{};
        if(CM_Locate_DevNodeW(&node,const_cast<wchar_t*>(id),CM_LOCATE_DEVNODE_NORMAL)==CR_SUCCESS&&node_container(node,container))result[lower(std::wstring(id+n,12))]=container;
    }
    return result;
}
struct AudioSwitch {std::vector<ComPtr<IKsControl>> controls;bool active=false;};
// Bluetooth audio endpoints grouped by device container (as ToothTray does):
// each endpoint's connector leads to the Bluetooth KS filter that reconnects.
std::map<GUID,AudioSwitch,GuidLess> audio_switches() {
    std::map<GUID,AudioSwitch,GuidLess> result;
    ComPtr<IMMDeviceEnumerator> devices;ComPtr<IMMDeviceCollection> endpoints;
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(devices.put())))||
       FAILED(devices->EnumAudioEndpoints(eAll,DEVICE_STATE_ACTIVE|DEVICE_STATE_UNPLUGGED,endpoints.put())))return result;
    UINT count=0;endpoints->GetCount(&count);
    for(UINT i=0;i<count;++i) {
        ComPtr<IMMDevice> endpoint;ComPtr<IPropertyStore> store;
        if(FAILED(endpoints->Item(i,endpoint.put()))||FAILED(endpoint->OpenPropertyStore(STGM_READ,store.put())))continue;
        PROPVARIANT value;PropVariantInit(&value);GUID container{};bool known=false;
        if(SUCCEEDED(store->GetValue(endpoint_container,&value))&&value.vt==VT_CLSID&&value.puuid){container=*value.puuid;known=true;}
        PropVariantClear(&value);if(!known)continue;
        ComPtr<IDeviceTopology> topology;UINT connectors=0;
        if(FAILED(endpoint->Activate(__uuidof(IDeviceTopology),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(topology.put())))||FAILED(topology->GetConnectorCount(&connectors)))continue;
        for(UINT c=0;c<connectors;++c) {
            ComPtr<IConnector> connector,other;ComPtr<IPart> part;ComPtr<IDeviceTopology> filter;LPWSTR id=nullptr;
            if(FAILED(topology->GetConnector(c,connector.put()))||FAILED(connector->GetConnectedTo(other.put()))||!other||
               FAILED(other->QueryInterface(__uuidof(IPart),reinterpret_cast<void**>(part.put())))||FAILED(part->GetTopologyObjects(filter.put()) /* SDK: GetTopologyObject */)||FAILED(filter->GetDeviceId(&id))||!id)continue;
            constexpr wchar_t bluetooth[]=L"{2}.\\\\?\\bth"; // bthenum (A2DP) or bthhfenum (hands-free)
            if(_wcsnicmp(id,bluetooth,std::size(bluetooth)-1)==0) {
                ComPtr<IMMDevice> device;ComPtr<IKsControl> control;
                if(SUCCEEDED(devices->GetDevice(id,device.put()))&&SUCCEEDED(device->Activate(iid_ks_control,CLSCTX_ALL,nullptr,reinterpret_cast<void**>(control.put())))) {
                    auto& entry=result[container];entry.controls.push_back(std::move(control));
                    DWORD state=0;if(SUCCEEDED(endpoint->GetState(&state))&&state==DEVICE_STATE_ACTIVE)entry.active=true;
                }
            }
            CoTaskMemFree(id);
        }
    }
    return result;
}
}

struct Connectivity::Impl {
    struct WlanEvent {DWORD code=0;DWORD reason=0;std::wstring profile;};
    mutable std::mutex mutex;
    std::condition_variable wake;
    ConnectivitySnapshot snapshot;
    std::deque<std::function<void()>> commands;
    std::deque<WlanEvent> events;
    bool requested=false,lists=false,devices=false,stopping=false;
    std::atomic<HWND> notify=nullptr;
    std::thread worker;
    // Worker thread only.
    Radios radios;
    HANDLE wlan=nullptr;GUID adapter{};bool has_adapter=false;
    std::wstring pending_profile;      // Created for the current attempt; removed if it fails.
    std::map<std::wstring,GUID> containers; // Device key -> container.
    ULONGLONG busy_until=0;

    static void CALLBACK wlan_changed(PWLAN_NOTIFICATION_DATA data,PVOID context) {
        if(!data||data->NotificationSource!=WLAN_NOTIFICATION_SOURCE_ACM)return;
        auto& self=*static_cast<Impl*>(context);WlanEvent event;event.code=data->NotificationCode;
        if((event.code==wlan_notification_acm_connection_complete||event.code==wlan_notification_acm_connection_attempt_fail)&&
           data->pData&&data->dwDataSize>=sizeof(WLAN_CONNECTION_NOTIFICATION_DATA)) {
            const auto& connection=*static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(data->pData);
            event.reason=connection.wlanReasonCode;event.profile=connection.strProfileName;
        }
        {std::lock_guard lock(self.mutex);self.events.push_back(std::move(event));}
        self.wake.notify_all();
    }
    void open_wlan() {
        DWORD version=0;
        if(WlanOpenHandle(2,nullptr,&version,&wlan)!=ERROR_SUCCESS){wlan=nullptr;return;}
        WlanRegisterNotification(wlan,WLAN_NOTIFICATION_SOURCE_ACM,TRUE,&Impl::wlan_changed,this,nullptr,nullptr);
        pick_adapter();
    }
    void close_wlan() {
        if(!wlan)return;
        WlanRegisterNotification(wlan,WLAN_NOTIFICATION_SOURCE_NONE,TRUE,nullptr,nullptr,nullptr,nullptr);
        WlanCloseHandle(wlan,nullptr);wlan=nullptr;
    }
    void pick_adapter() {
        has_adapter=false;PWLAN_INTERFACE_INFO_LIST list=nullptr;
        if(wlan&&WlanEnumInterfaces(wlan,nullptr,&list)==ERROR_SUCCESS&&list){if(list->dwNumberOfItems){adapter=list->InterfaceInfo[0].InterfaceGuid;has_adapter=true;}}
        if(list)WlanFreeMemory(list);
    }
    void read_wifi(ConnectivitySnapshot& s,bool networks) {
        s.wifi_connected=false;s.wifi_name.clear();
        if(!wlan)return;if(!has_adapter)pick_adapter();if(!has_adapter)return;
        DWORD size=0;PVOID data=nullptr;WLAN_OPCODE_VALUE_TYPE type{};
        if(WlanQueryInterface(wlan,&adapter,wlan_intf_opcode_interface_state,nullptr,&size,&data,&type)==ERROR_SUCCESS&&data)
            s.wifi_connected=*static_cast<WLAN_INTERFACE_STATE*>(data)==wlan_interface_state_connected;
        if(data){WlanFreeMemory(data);data=nullptr;}
        // The connected network's name is location data on current Windows.
        if(s.wifi_connected&&WlanQueryInterface(wlan,&adapter,wlan_intf_opcode_current_connection,nullptr,&size,&data,&type)==ERROR_SUCCESS&&data)
            s.wifi_name=ssid_text(static_cast<WLAN_CONNECTION_ATTRIBUTES*>(data)->wlanAssociationAttributes.dot11Ssid);
        if(data)WlanFreeMemory(data);
        if(!networks)return;
        PWLAN_AVAILABLE_NETWORK_LIST list=nullptr;
        const auto result=WlanGetAvailableNetworkList(wlan,&adapter,0,nullptr,&list);
        if(result==ERROR_ACCESS_DENIED){s.location_denied=true;s.networks.clear();if(list)WlanFreeMemory(list);return;}
        if(result!=ERROR_SUCCESS||!list){if(list)WlanFreeMemory(list);return;}
        s.location_denied=false;std::vector<WifiNetwork> found;
        for(DWORD i=0;i<list->dwNumberOfItems;++i) {
            const auto& entry=list->Network[i];WifiNetwork network;network.ssid=ssid_text(entry.dot11Ssid);
            if(network.ssid.empty())continue; // Hidden networks need their name typed: Settings.
            network.signal=static_cast<int>(entry.wlanSignalQuality);network.secured=entry.bSecurityEnabled!=FALSE;
            network.connected=(entry.dwFlags&WLAN_AVAILABLE_NETWORK_CONNECTED)!=0;network.saved=(entry.dwFlags&WLAN_AVAILABLE_NETWORK_HAS_PROFILE)!=0;
            network.profile=entry.strProfileName;network.auth=static_cast<int>(entry.dot11DefaultAuthAlgorithm);network.cipher=static_cast<int>(entry.dot11DefaultCipherAlgorithm);
            network.password_supported=network.saved||profile_auth(network.auth,network.secured)!=nullptr;
            const auto same=std::find_if(found.begin(),found.end(),[&](const WifiNetwork& n){return n.ssid==network.ssid;});
            if(same==found.end()){found.push_back(std::move(network));continue;}
            // One row per name: keep the connected/saved entry and the best signal.
            const int signal=std::max(same->signal,network.signal);
            if((network.connected&&!same->connected)||(network.saved&&!same->saved&&!same->connected))*same=std::move(network);
            same->signal=signal;
        }
        WlanFreeMemory(list);
        std::stable_sort(found.begin(),found.end(),[](const WifiNetwork& a,const WifiNetwork& b){return a.connected!=b.connected?a.connected:a.signal>b.signal;});
        s.networks=std::move(found);
        if(s.wifi_name.empty())for(const auto& n:s.networks)if(n.connected)s.wifi_name=n.ssid;
    }
    std::vector<BluetoothDevice> read_bluetooth() {
        std::vector<BluetoothDevice> result;containers.clear();
        const auto classic=classic_containers();const auto audio=audio_switches();
        const auto mark=[&](BluetoothDevice& device,const GUID* container) {
            if(!container)return;containers[device.key]=*container;
            if(const auto found=audio.find(*container);found!=audio.end()&&!found->second.controls.empty()){device.switchable=true;device.kind=BluetoothDevice::Kind::Audio;}
        };
        BLUETOOTH_DEVICE_SEARCH_PARAMS params{};params.dwSize=sizeof(params);
        params.fReturnAuthenticated=TRUE;params.fReturnRemembered=TRUE;params.fReturnConnected=TRUE;params.fReturnUnknown=FALSE;params.fIssueInquiry=FALSE;
        BLUETOOTH_DEVICE_INFO info{};info.dwSize=sizeof(info);
        if(const auto search=BluetoothFindFirstDevice(&params,&info)) {
            do {
                if(!info.fRemembered&&!info.fAuthenticated)continue;
                BluetoothDevice device;device.name=info.szName;device.connected=info.fConnected!=FALSE;device.kind=kind_from_class(info.ulClassofDevice);
                wchar_t address[13]{};swprintf(address,13,L"%012llx",static_cast<unsigned long long>(info.Address.ullLong));
                device.key=std::wstring(L"C:")+address;
                const auto container=classic.find(address);mark(device,container!=classic.end()?&container->second:nullptr);
                if(device.name.empty())device.name=L"Bluetooth device";
                result.push_back(std::move(device));
            } while(BluetoothFindNextDevice(search,&info));
            BluetoothFindDeviceClose(search);
        }
        // Bluetooth LE devices (mice, keyboards, earbuds' LE side).
        const auto set=SetupDiGetClassDevsW(&le_device_interface,nullptr,nullptr,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);
        if(set!=INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA data{};data.cbSize=sizeof(data);
            for(DWORD i=0;SetupDiEnumDeviceInfo(set,i,&data);++i) {
                wchar_t name[256]{};
                if(!SetupDiGetDeviceRegistryPropertyW(set,&data,SPDRP_FRIENDLYNAME,nullptr,reinterpret_cast<PBYTE>(name),sizeof(name)-sizeof(wchar_t),nullptr)&&
                   !SetupDiGetDeviceRegistryPropertyW(set,&data,SPDRP_DEVICEDESC,nullptr,reinterpret_cast<PBYTE>(name),sizeof(name)-sizeof(wchar_t),nullptr))continue;
                wchar_t id[MAX_DEVICE_ID_LEN]{};if(!SetupDiGetDeviceInstanceIdW(set,&data,id,MAX_DEVICE_ID_LEN,nullptr))continue;
                ULONG status=0,problem=0;
                const bool connected=CM_Get_DevNode_Status(&status,&problem,data.DevInst,0)==CR_SUCCESS&&!(status&DN_DEVICE_DISCONNECTED);
                // Dual-mode devices appear twice ("Name" and "LE-Name"): merge them.
                std::wstring label=name;if(label.size()>3&&_wcsnicmp(label.c_str(),L"LE-",3)==0)label.erase(0,3);
                const auto same=std::find_if(result.begin(),result.end(),[&](const BluetoothDevice& d){return lower(d.name)==lower(label);});
                if(same!=result.end()){same->connected|=connected;continue;}
                BluetoothDevice device;device.name=label;device.key=std::wstring(L"L:")+id;device.connected=connected;
                GUID container{};mark(device,node_container(data.DevInst,container)?&container:nullptr);
                result.push_back(std::move(device));
            }
            SetupDiDestroyDeviceInfoList(set);
        }
        std::stable_sort(result.begin(),result.end(),[](const BluetoothDevice& a,const BluetoothDevice& b){return a.connected!=b.connected?a.connected:lower(a.name)<lower(b.name);});
        return result;
    }
    void publish(bool networks,bool bluetooth) {
        ConnectivitySnapshot s;{std::lock_guard lock(mutex);s=snapshot;}
        if(!radios.wifi||!radios.bluetooth||bluetooth)radios.load();
        if(!has_adapter)pick_adapter();
        s.wifi_present=radios.wifi||has_adapter;s.wifi_on=radios.wifi?Radios::on(radios.wifi.get()):has_adapter;
        s.bluetooth_present=static_cast<bool>(radios.bluetooth);s.bluetooth_on=Radios::on(radios.bluetooth.get());
        read_wifi(s,networks&&s.wifi_on);
        if(!s.wifi_on){s.networks.clear();s.wifi_connected=false;s.wifi_name.clear();s.connecting.clear();}
        if(bluetooth)s.devices=s.bluetooth_on?read_bluetooth():std::vector<BluetoothDevice>{};
        if(!s.bluetooth_on)s.devices.clear();
        if(GetTickCount64()>=busy_until)s.bluetooth_busy.clear();
        {std::lock_guard lock(mutex);snapshot=std::move(s);}
        if(const auto window=notify.load())PostMessageW(window,connectivity_changed_message,0,0);
    }
    void handle(const WlanEvent& event,bool& networks) {
        switch(event.code) {
        case wlan_notification_acm_scan_complete:case wlan_notification_acm_scan_fail:
            {std::lock_guard lock(mutex);snapshot.scanning=false;}networks=true;break;
        case wlan_notification_acm_connection_complete: {
            const bool failed=event.reason!=WLAN_REASON_CODE_SUCCESS;std::wstring message;
            if(failed) {
                wchar_t reason[256]{};WlanReasonCodeToString(event.reason,256,reason,nullptr);
                message=L"Couldn't connect. "+std::wstring(reason);
                // A network joined with a new password is not kept when it fails.
                if(!pending_profile.empty()&&event.profile==pending_profile&&wlan&&has_adapter)WlanDeleteProfile(wlan,&adapter,pending_profile.c_str(),nullptr);
            }
            if(event.profile==pending_profile||!failed)pending_profile.clear();
            {std::lock_guard lock(mutex);snapshot.connecting.clear();snapshot.wifi_message=message;}
            networks=true;break;
        }
        case wlan_notification_acm_interface_arrival:case wlan_notification_acm_interface_removal:pick_adapter();networks=true;break;
        case wlan_notification_acm_disconnected:case wlan_notification_acm_connection_start:networks=true;break;
        default:break;
        }
    }
    void run() {
        const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        open_wlan();
        for(;;) {
            std::deque<std::function<void()>> work;std::deque<WlanEvent> changes;bool networks=false,bluetooth=false,refresh=false;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,[&]{return stopping||requested||!commands.empty()||!events.empty();});
                if(stopping)break;
                work.swap(commands);changes.swap(events);refresh=requested;networks=lists;bluetooth=devices;requested=lists=devices=false;
            }
            for(auto& command:work)command();
            for(const auto& change:changes)handle(change,networks);
            if(refresh||!work.empty()||!changes.empty())publish(networks,bluetooth);
        }
        close_wlan();radios=Radios{};
        if(SUCCEEDED(apartment))CoUninitialize();
    }
    void enqueue(std::function<void()> command,bool networks,bool bluetooth) {
        {std::lock_guard lock(mutex);commands.push_back(std::move(command));requested=true;lists|=networks;devices|=bluetooth;}
        wake.notify_all();
    }
    void connect_profile(const std::wstring& profile) {
        WLAN_CONNECTION_PARAMETERS parameters{};parameters.wlanConnectionMode=wlan_connection_mode_profile;
        parameters.strProfile=profile.c_str();parameters.dot11BssType=dot11_BSS_type_infrastructure;
        const auto result=WlanConnect(wlan,&adapter,&parameters,nullptr);
        if(result!=ERROR_SUCCESS){std::lock_guard lock(mutex);snapshot.connecting.clear();snapshot.wifi_message=L"Couldn't connect to this network.";}
    }
};

Connectivity::Connectivity():impl_(std::make_unique<Impl>()){}
Connectivity::~Connectivity(){stop();}
void Connectivity::start(HWND notify) {
    stop();impl_->notify=notify;impl_->stopping=false;
    impl_->worker=std::thread([this]{impl_->run();});
    refresh(false);
}
void Connectivity::stop() {
    auto& i=*impl_;i.notify=nullptr;
    if(!i.worker.joinable())return;
    {std::lock_guard lock(i.mutex);i.stopping=true;i.commands.clear();}
    i.wake.notify_all();i.worker.join();
}
ConnectivitySnapshot Connectivity::snapshot() const{std::lock_guard lock(impl_->mutex);return impl_->snapshot;}
void Connectivity::refresh(bool lists) {
    {std::lock_guard lock(impl_->mutex);impl_->requested=true;impl_->lists|=lists;impl_->devices|=lists;}
    impl_->wake.notify_all();
}
void Connectivity::scan() {
    impl_->enqueue([i=impl_.get()]{
        if(!i->wlan||!i->has_adapter)return;
        if(WlanScan(i->wlan,&i->adapter,nullptr,nullptr,nullptr)==ERROR_SUCCESS){std::lock_guard lock(i->mutex);i->snapshot.scanning=true;}
    },true,false);
}
void Connectivity::set_wifi(bool on){impl_->enqueue([i=impl_.get(),on]{i->radios.load();Radios::set(i->radios.wifi.get(),on);},true,false);}
void Connectivity::set_bluetooth(bool on){impl_->enqueue([i=impl_.get(),on]{i->radios.load();Radios::set(i->radios.bluetooth.get(),on);},false,true);}
void Connectivity::connect_wifi(const WifiNetwork& network,std::wstring password) {
    {std::lock_guard lock(impl_->mutex);impl_->snapshot.connecting=network.ssid;impl_->snapshot.wifi_message.clear();}
    impl_->enqueue([i=impl_.get(),network,password=std::move(password)]() mutable {
        if(!i->wlan||!i->has_adapter)return;
        if(network.saved&&!network.profile.empty()){i->connect_profile(network.profile);SecureZeroMemory(password.data(),password.size()*sizeof(wchar_t));return;}
        const auto auth=profile_auth(network.auth,network.secured);
        if(!auth){std::lock_guard lock(i->mutex);i->snapshot.connecting.clear();i->snapshot.wifi_message=L"This network needs Windows Settings to sign in.";return;}
        const bool key=network.secured&&network.auth!=10;
        std::wstring xml=L"<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>"+xml_escape(network.ssid)+
            L"</name><SSIDConfig><SSID><hex>"+hex_utf8(network.ssid)+L"</hex><name>"+xml_escape(network.ssid)+
            L"</name></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>auto</connectionMode><MSM><security><authEncryption><authentication>"+
            auth+L"</authentication><encryption>"+profile_cipher(network.cipher,network.secured)+L"</encryption><useOneX>false</useOneX></authEncryption>";
        if(key)xml+=L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>"+xml_escape(password)+L"</keyMaterial></sharedKey>";
        xml+=L"</security></MSM></WLANProfile>";
        DWORD reason=0;const auto result=WlanSetProfile(i->wlan,&i->adapter,0,xml.c_str(),nullptr,TRUE,nullptr,&reason);
        SecureZeroMemory(xml.data(),xml.size()*sizeof(wchar_t));SecureZeroMemory(password.data(),password.size()*sizeof(wchar_t));
        if(result!=ERROR_SUCCESS) {
            wchar_t text[256]{};WlanReasonCodeToString(reason,256,text,nullptr);
            std::lock_guard lock(i->mutex);i->snapshot.connecting.clear();i->snapshot.wifi_message=std::wstring(L"Couldn't save this network. ")+text;return;
        }
        i->pending_profile=network.ssid;i->connect_profile(network.ssid);
    },true,false);
}
void Connectivity::disconnect_wifi(){impl_->enqueue([i=impl_.get()]{if(i->wlan&&i->has_adapter)WlanDisconnect(i->wlan,&i->adapter,nullptr);},true,false);}
void Connectivity::connect_bluetooth(const std::wstring& key,bool connect) {
    {std::lock_guard lock(impl_->mutex);impl_->snapshot.bluetooth_busy=key;}
    impl_->enqueue([i=impl_.get(),key,connect]{
        i->busy_until=GetTickCount64()+8000;
        const auto container=i->containers.find(key);if(container==i->containers.end())return;
        auto switches=audio_switches();const auto found=switches.find(container->second);if(found==switches.end())return;
        for(auto& control:found->second.controls) {
            KsIdentifier property{ks_bt_audio,connect?ks_reconnect:ks_disconnect,ks_get};ULONG returned=0;
            control->KsProperty(&property,sizeof(property),nullptr,0,&returned);
        }
    },false,true);
}
}
