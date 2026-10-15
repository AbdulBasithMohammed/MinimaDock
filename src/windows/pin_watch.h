#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>

namespace minima {
// Posts `message` to `notify` when Windows' taskbar pins change (Explorer's
// "Pin to taskbar" / "Unpin from taskbar", from any app or the desktop).
// Watches the Taskband registry key and the pinned-shortcuts folder.
class PinWatch {
public:
    ~PinWatch(){stop();}
    void start(HWND notify,UINT message,const std::wstring& folder) {
        stop();
        stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!stop_event)return;
        worker=std::thread([this,notify,message,folder]{run(notify,message,folder);});
    }
    void stop() {
        if(stop_event)SetEvent(stop_event);
        if(worker.joinable())worker.join();
        if(stop_event){CloseHandle(stop_event);stop_event=nullptr;}
    }
private:
    HANDLE stop_event=nullptr;std::thread worker;
    void run(HWND notify,UINT message,std::wstring folder) {
        HKEY key=nullptr;
        RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband",0,KEY_NOTIFY,&key);
        HANDLE registry=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        HANDLE files=folder.empty()?INVALID_HANDLE_VALUE:FindFirstChangeNotificationW(folder.c_str(),FALSE,FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_LAST_WRITE);
        for(;;) {
            if(key&&registry)RegNotifyChangeKeyValue(key,FALSE,REG_NOTIFY_CHANGE_LAST_SET,registry,TRUE);
            HANDLE waits[3]{stop_event};DWORD count=1;
            if(key&&registry)waits[count++]=registry;
            if(files!=INVALID_HANDLE_VALUE)waits[count++]=files;
            const auto result=WaitForMultipleObjects(count,waits,FALSE,INFINITE);
            if(result==WAIT_OBJECT_0||result==WAIT_FAILED)break;
            if(files!=INVALID_HANDLE_VALUE&&waits[result-WAIT_OBJECT_0]==files)FindNextChangeNotification(files);
            PostMessageW(notify,message,0,0);
        }
        if(files!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(files);
        if(registry)CloseHandle(registry);
        if(key)RegCloseKey(key);
    }
};
}
