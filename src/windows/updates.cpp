#include "windows/updates.h"
#include "platform/com_ptr.h"
#include <wuapi.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace minima {
namespace {
bool key_exists(const wchar_t* path) {
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,path,0,KEY_READ|KEY_WOW64_64KEY,&key)!=ERROR_SUCCESS)return false;
    RegCloseKey(key);return true;
}
UpdateStatus read_status() {
    UpdateStatus status;status.known=true;
    status.restart=key_exists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired")||
                   key_exists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending");
    ComPtr<IUpdateSession> session;ComPtr<IUpdateSearcher> searcher;ComPtr<ISearchResult> result;ComPtr<IUpdateCollection> updates;
    if(FAILED(CoCreateInstance(__uuidof(UpdateSession),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IUpdateSession),reinterpret_cast<void**>(session.put())))||
       FAILED(session->CreateUpdateSearcher(searcher.put())))return status;
    searcher->put_Online(VARIANT_FALSE); // Cached results of Windows' own last scan.
    const auto criteria=SysAllocString(L"IsInstalled=0 and IsHidden=0 and Type='Software'");
    if(criteria&&SUCCEEDED(searcher->Search(criteria,result.put()))&&result&&SUCCEEDED(result->get_Updates(updates.put()))&&updates) {
        LONG count=0;if(SUCCEEDED(updates->get_Count(&count)))status.pending=count;
    }
    SysFreeString(criteria);
    return status;
}
}
struct UpdateMonitor::Impl {
    mutable std::mutex mutex;
    UpdateStatus status;
    std::atomic<HWND> notify=nullptr;
    std::atomic<bool> running=false;
    std::thread worker;
    ULONGLONG last=0;
};
UpdateMonitor::UpdateMonitor():impl_(std::make_unique<Impl>()){}
UpdateMonitor::~UpdateMonitor(){stop();}
void UpdateMonitor::start(HWND notify){impl_->notify=notify;}
void UpdateMonitor::stop(){impl_->notify=nullptr;if(impl_->worker.joinable())impl_->worker.join();}
void UpdateMonitor::refresh(bool force) {
    auto& i=*impl_;const auto now=GetTickCount64();
    if(i.running||(!force&&i.last&&now-i.last<5*60*1000))return;
    if(i.worker.joinable())i.worker.join();
    i.last=now;i.running=true;
    i.worker=std::thread([&i]{
        const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        const auto status=read_status();
        if(SUCCEEDED(apartment))CoUninitialize();
        {std::lock_guard lock(i.mutex);i.status=status;}
        i.running=false;
        if(const auto window=i.notify.load())PostMessageW(window,updates_changed_message,0,0);
    });
}
UpdateStatus UpdateMonitor::status() const{std::lock_guard lock(impl_->mutex);return impl_->status;}
}
