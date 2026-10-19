#include "windows/geolocation.h"
#include "windows/http.h"
#include "windows/media_abi.h"
#include "platform/com_ptr.h"
#include <roapi.h>
#include <asyncinfo.h>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace minima {
namespace {
using media_abi::Async;

// Windows.Devices.Geolocation, hand-declared because the toolchain's WinRT
// headers do not compile as C++. Method order and IIDs follow
// windows.devices.geolocation.h. Geoposition and Geocoordinate arrive as typed
// results, so only the Geolocator needs a QueryInterface.
struct TimeSpan {INT64 duration;}; // 100 ns units.
struct IGeocoordinate : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Latitude(DOUBLE* value)=0;
    virtual HRESULT STDMETHODCALLTYPE get_Longitude(DOUBLE* value)=0;
    virtual HRESULT STDMETHODCALLTYPE get_Altitude(IInspectable** value)=0;
    virtual HRESULT STDMETHODCALLTYPE get_Accuracy(DOUBLE* value)=0;
};
struct IGeoposition : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Coordinate(IGeocoordinate** value)=0;
};
struct IGeolocator : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DesiredAccuracy(INT32* value)=0;
    virtual HRESULT STDMETHODCALLTYPE put_DesiredAccuracy(INT32 value)=0;
    virtual HRESULT STDMETHODCALLTYPE get_MovementThreshold(DOUBLE* value)=0;
    virtual HRESULT STDMETHODCALLTYPE put_MovementThreshold(DOUBLE value)=0;
    virtual HRESULT STDMETHODCALLTYPE get_ReportInterval(UINT32* value)=0;
    virtual HRESULT STDMETHODCALLTYPE put_ReportInterval(UINT32 value)=0;
    virtual HRESULT STDMETHODCALLTYPE get_LocationStatus(INT32* value)=0;
    virtual HRESULT STDMETHODCALLTYPE GetGeopositionAsync(Async<IGeoposition*>** operation)=0;
    virtual HRESULT STDMETHODCALLTYPE GetGeopositionAsyncWithAgeAndTimeout(TimeSpan maximum_age,TimeSpan timeout,Async<IGeoposition*>** operation)=0;
};
constexpr IID iid_geolocator={0xa9c3bf62,0x4524,0x4989,{0x8a,0xa9,0xde,0x01,0x9d,0x2e,0x55,0x1f}};
constexpr INT32 position_disabled=3; // PositionStatus::Disabled: location is off for this PC.

// Waits for the operation while watching the cancel flag; cancels on timeout.
HRESULT wait(Async<IGeoposition*>* operation,IGeoposition** value,int timeout_ms,const std::atomic<bool>* cancel,bool* cancelled) {
    ComPtr<IAsyncInfo> info;HRESULT hr=operation->QueryInterface(__uuidof(IAsyncInfo),reinterpret_cast<void**>(info.put()));if(FAILED(hr))return hr;
    AsyncStatus status=Started;
    for(int waited=0;SUCCEEDED(hr=info->get_Status(&status))&&status==Started;waited+=20) {
        if(waited>=timeout_ms||(cancel&&cancel->load())){*cancelled=cancel&&cancel->load();info->Cancel();return HRESULT_FROM_WIN32(WAIT_TIMEOUT);}
        Sleep(20);
    }
    if(FAILED(hr))return hr;
    if(status==Completed)return operation->GetResults(value);
    if(status==Error){info->get_ErrorCode(&hr);return FAILED(hr)?hr:E_FAIL;}
    return E_ABORT;
}

// Nominatim's usage policy: an identifying agent, at most one request a
// second, and cached answers. https://operations.osmfoundation.org/policies/nominatim/
std::mutex nominatim_lock;
ULONGLONG last_request=0;
std::map<std::wstring,PlaceLookup> answers;

std::wstring user_language() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    return GetUserDefaultLocaleName(name,LOCALE_NAME_MAX_LENGTH)>0?name:L"";
}
PlaceLookup nominatim(const std::wstring& url) {
    std::lock_guard lock(nominatim_lock); // Also serializes requests, which keeps the spacing honest.
    if(const auto found=answers.find(url);found!=answers.end())return found->second;
    const auto since=GetTickCount64()-last_request;
    if(last_request&&since<1000)Sleep(static_cast<DWORD>(1000-since));
    HttpOptions options;options.agent=L"MinimaDock/1.0 (Windows desktop shell)";options.language=user_language();options.timeout_ms=5000;
    const auto answer=http_request(url,options);
    last_request=GetTickCount64();
    PlaceLookup result;
    if(answer.status==0)result.error=PlaceError::Offline;
    else if(answer.status==403||answer.status==429||answer.status==503)result.error=PlaceError::Busy;
    else if(answer.status!=200)result.error=PlaceError::Failed;
    else if(auto places=parse_places(answer.body))result.places=std::move(*places);
    else result.error=PlaceError::Failed; // Not JSON: a captive portal or an error page.
    if(result.error==PlaceError::None) {
        if(answers.size()>=64)answers.clear();
        answers.emplace(url,result);
    }
    return result;
}
}

GeoFix locate_device(const std::atomic<bool>* cancel) {
    struct Apartment {HRESULT initialized=RoInitialize(RO_INIT_MULTITHREADED);~Apartment(){if(SUCCEEDED(initialized))RoUninitialize();}} apartment;
    GeoFix fix;
    constexpr wchar_t runtime[]=L"Windows.Devices.Geolocation.Geolocator";
    HSTRING name=nullptr;
    if(FAILED(WindowsCreateString(runtime,static_cast<UINT32>(std::size(runtime)-1),&name)))return fix;
    ComPtr<IInspectable> instance;const auto activated=RoActivateInstance(name,instance.put());WindowsDeleteString(name);
    ComPtr<IGeolocator> locator;
    if(FAILED(activated)||FAILED(instance->QueryInterface(iid_geolocator,reinterpret_cast<void**>(locator.put()))))return fix;
    INT32 status=0;
    if(SUCCEEDED(locator->get_LocationStatus(&status))&&status==position_disabled){fix.status=LocateStatus::Disabled;return fix;}
    // A fix from the last 10 minutes is good enough for prayer times and is instant.
    ComPtr<Async<IGeoposition*>> operation;
    if(FAILED(locator->GetGeopositionAsyncWithAgeAndTimeout(TimeSpan{10LL*60*10'000'000},TimeSpan{10LL*10'000'000},operation.put())))return fix;
    ComPtr<IGeoposition> position;bool cancelled=false;
    const auto hr=wait(operation.get(),position.put(),11000,cancel,&cancelled);
    if(cancelled){fix.status=LocateStatus::Cancelled;return fix;}
    if(hr==E_ACCESSDENIED) {
        // Denied for desktop apps, or switched off while we asked.
        fix.status=SUCCEEDED(locator->get_LocationStatus(&status))&&status==position_disabled?LocateStatus::Disabled:LocateStatus::Denied;
        return fix;
    }
    ComPtr<IGeocoordinate> coordinate;
    if(FAILED(hr)||!position||FAILED(position->get_Coordinate(coordinate.put()))||!coordinate)return fix;
    DOUBLE latitude=0,longitude=0,accuracy=0;
    if(FAILED(coordinate->get_Latitude(&latitude))||FAILED(coordinate->get_Longitude(&longitude))||!valid_coordinates(latitude,longitude))return fix;
    coordinate->get_Accuracy(&accuracy);
    fix.status=LocateStatus::Ok;fix.latitude=latitude;fix.longitude=longitude;fix.accuracy_m=accuracy;
    return fix;
}

PlaceLookup search_places(std::wstring_view query) {
    return nominatim(L"https://nominatim.openstreetmap.org/search?format=jsonv2&addressdetails=1&limit=5&q="+percent_encode(query));
}

PlaceLookup reverse_place(double latitude,double longitude) {
    // Two decimals (about 1 km) is plenty to name a town and keeps the exact position on this PC.
    wchar_t point[64];std::swprintf(point,64,L"&lat=%.2f&lon=%.2f",latitude,longitude);
    auto result=nominatim(std::wstring(L"https://nominatim.openstreetmap.org/reverse?format=jsonv2&addressdetails=1&zoom=10")+point);
    if(result.places.size()>1)result.places.resize(1);
    return result;
}
}
