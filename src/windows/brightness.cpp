#include "windows/brightness.h"
#include "platform/com_ptr.h"
#include <wbemcli.h>
#include <oleauto.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace minima {
namespace {
struct Variant {
    VARIANT v;
    Variant(){VariantInit(&v);}
    ~Variant(){VariantClear(&v);}
    Variant(const Variant&)=delete;
    Variant& operator=(const Variant&)=delete;
};
struct Bstr {
    BSTR s;
    explicit Bstr(const wchar_t* text):s(SysAllocString(text)){}
    ~Bstr(){SysFreeString(s);}
    Bstr(const Bstr&)=delete;
    Bstr& operator=(const Bstr&)=delete;
};
// First active instance of a WMI class, or nothing.
ComPtr<IWbemClassObject> first(IWbemServices* wmi,const wchar_t* query) {
    ComPtr<IEnumWbemClassObject> results;ComPtr<IWbemClassObject> object;
    const Bstr language(L"WQL"),text(query);
    if(FAILED(wmi->ExecQuery(language.s,text.s,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,results.put())))return object;
    ULONG count=0;
    if(results->Next(2000,1,object.put(),&count)!=WBEM_S_NO_ERROR||!count)object.reset();
    return object;
}
}

struct Brightness::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool quit=false,read=false;
    int pending=-1;                 // Latest requested level, -1 when none.
    std::atomic<HWND> notify{nullptr};
    std::atomic<bool> available{false};
    std::atomic<int> level{0};

    void publish(bool now_available,int now_level) {
        const bool was_available=available.exchange(now_available);
        const int was_level=level.exchange(now_level);
        if(was_available!=now_available||was_level!=now_level)if(const auto w=notify.load())PostMessageW(w,brightness_changed_message,0,0);
    }
    void run() {
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        ComPtr<IWbemLocator> locator;ComPtr<IWbemServices> wmi;
        if(SUCCEEDED(CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_IWbemLocator,reinterpret_cast<void**>(locator.put())))) {
            const Bstr space(L"ROOT\\WMI");
            if(SUCCEEDED(locator->ConnectServer(space.s,nullptr,nullptr,nullptr,0,nullptr,nullptr,wmi.put())))
                CoSetProxyBlanket(wmi.get(),RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE);
            else wmi.reset();
        }
        bool first_pass=true;
        while(true) {
            int target=-1;bool reread=first_pass;first_pass=false;
            {
                std::unique_lock lock(mutex);
                if(!reread)wake.wait(lock,[&]{return quit||read||pending>=0;});
                if(quit)break;
                target=std::exchange(pending,-1);reread=reread||std::exchange(read,false);
            }
            if(!wmi){publish(false,0);continue;}
            if(target>=0) {
                set_level(wmi.get(),target);
                publish(true,target);
                // Throttle: a drag produces a value per mouse move.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if(reread)read_level(wmi.get());
        }
        wmi.reset();locator.reset();
        if(SUCCEEDED(com))CoUninitialize();
    }
    void read_level(IWbemServices* wmi) {
        const auto monitor=first(wmi,L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");
        if(!monitor){publish(false,0);return;}
        Variant value;
        if(FAILED(monitor->Get(L"CurrentBrightness",0,&value.v,nullptr,nullptr))){publish(false,0);return;}
        int percent=0;
        if(value.v.vt==VT_UI1)percent=value.v.bVal;else if(value.v.vt==VT_I4)percent=static_cast<int>(value.v.lVal);
        publish(true,std::clamp(percent,0,100));
    }
    static void set_level(IWbemServices* wmi,int percent) {
        const auto methods=first(wmi,L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");if(!methods)return;
        Variant path;if(FAILED(methods->Get(L"__PATH",0,&path.v,nullptr,nullptr))||path.v.vt!=VT_BSTR)return;
        ComPtr<IWbemClassObject> type,signature,input;
        const Bstr name(L"WmiMonitorBrightnessMethods"),method(L"WmiSetBrightness");
        if(FAILED(wmi->GetObject(name.s,0,nullptr,type.put(),nullptr)))return;
        if(FAILED(type->GetMethod(method.s,0,signature.put(),nullptr))||!signature)return;
        if(FAILED(signature->SpawnInstance(0,input.put())))return;
        Variant timeout,brightness;
        timeout.v.vt=VT_I4;timeout.v.lVal=0;
        brightness.v.vt=VT_UI1;brightness.v.bVal=static_cast<BYTE>(std::clamp(percent,0,100));
        input->Put(L"Timeout",0,&timeout.v,0);
        input->Put(L"Brightness",0,&brightness.v,0);
        wmi->ExecMethod(path.v.bstrVal,method.s,0,nullptr,input.get(),nullptr,nullptr);
    }
};

Brightness::Brightness():impl_(std::make_unique<Impl>()){}
Brightness::~Brightness(){stop();}
void Brightness::start(HWND notify) {
    if(impl_->worker.joinable())return;
    impl_->notify=notify;impl_->quit=false;
    impl_->worker=std::thread([impl=impl_.get()]{impl->run();});
}
void Brightness::stop() {
    if(!impl_||!impl_->worker.joinable())return;
    impl_->notify=nullptr;
    {std::lock_guard lock(impl_->mutex);impl_->quit=true;}
    impl_->wake.notify_one();impl_->worker.join();
}
bool Brightness::available() const{return impl_->available.load();}
int Brightness::level() const{return impl_->level.load();}
void Brightness::refresh() {
    {std::lock_guard lock(impl_->mutex);impl_->read=true;}
    impl_->wake.notify_one();
}
void Brightness::set(int percent) {
    {std::lock_guard lock(impl_->mutex);impl_->pending=std::clamp(percent,0,100);}
    impl_->level=std::clamp(percent,0,100); // The slider follows the pointer, not WMI.
    impl_->wake.notify_one();
}
}
