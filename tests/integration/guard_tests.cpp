#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
namespace fs=std::filesystem;
namespace {
struct Process {
    PROCESS_INFORMATION value{};
    ~Process(){ if(value.hProcess){if(WaitForSingleObject(value.hProcess,0)==WAIT_TIMEOUT) {TerminateProcess(value.hProcess,99); WaitForSingleObject(value.hProcess,2000);} CloseHandle(value.hProcess); CloseHandle(value.hThread);} }
};
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void start(const fs::path& image,std::wstring command,Process& p,bool inherit=false) {
    STARTUPINFOW startup{}; startup.cb=sizeof(startup);
    require(CreateProcessW(image.c_str(),command.data(),nullptr,nullptr,inherit,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&p.value)!=FALSE,"Cannot launch test process");
}
std::wstring quote(const fs::path& path){return L"\""+path.wstring()+L"\"";}
void scenario(const fs::path& binaries,const fs::path& root,const std::wstring& mode,bool keep=false,bool bad_ticks=false) {
    const auto bundle=root/(mode+(keep?L"-keep":L"")+(bad_ticks?L"-bad":L"")); fs::create_directory(bundle);
    std::ofstream script(bundle/L"Restore.ps1"); script << "param([string]$Bundle)\n[IO.File]::WriteAllText((Join-Path $Bundle 'recovered.txt'),'recovered')\nexit 0\n"; script.close();
    std::ofstream baseline(bundle/L"baseline.json"); baseline << "test fixture only"; baseline.close();
    const auto info_file=bundle/L"fixture.txt";
    Process fixture;
    start(binaries/L"MinimaDockGuardFixture.exe",quote(binaries/L"MinimaDockGuardFixture.exe")+L" "+mode+L" "+quote(info_file),fixture);
    ULONG_PTR hwnd=0; unsigned long long ticks=0;
    for(unsigned i=0;i<100 && !hwnd;++i) { std::ifstream info(info_file); info>>hwnd>>ticks; if(!hwnd) Sleep(30); }
    require(hwnd!=0,"Fixture handshake failed");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,TRUE};
    HANDLE ready=CreateEventW(&attributes,TRUE,FALSE,nullptr),accept=CreateEventW(&attributes,TRUE,FALSE,nullptr),revert=CreateEventW(&attributes,TRUE,FALSE,nullptr);
    require(ready&&accept&&revert,"Cannot create test events");
    Process guard;
    const auto guard_image=binaries/L"MinimaDockGuardTest.exe";
    const auto args=quote(guard_image)+L" --watch "+std::to_wstring(fixture.value.dwProcessId)+L" "+std::to_wstring(hwnd)+L" "+std::to_wstring(ticks+(bad_ticks?1:0))+L" 10 "+quote(bundle)+L" "+std::to_wstring(reinterpret_cast<ULONG_PTR>(ready))+L" "+std::to_wstring(reinterpret_cast<ULONG_PTR>(accept))+L" "+std::to_wstring(reinterpret_cast<ULONG_PTR>(revert));
    start(guard_image,args,guard,true);
    if(bad_ticks) {
        require(WaitForSingleObject(guard.value.hProcess,5000)==WAIT_OBJECT_0,"Identity rejection must finish promptly");
        require(WaitForSingleObject(ready,0)==WAIT_TIMEOUT,"Bad identity must not arm guard");
        require(!fs::exists(bundle/L"recovered.txt"),"Unarmed guard must not run recovery");
    } else {
        require(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0,"Guard did not arm");
        if(keep) {
            SetEvent(accept); Sleep(12000);
            require(WaitForSingleObject(guard.value.hProcess,0)==WAIT_TIMEOUT,"Keep decision must cancel trial timeout while retaining crash supervision");
            SetEvent(revert);
        }
        if(mode==L"revert") SetEvent(revert);
        require(WaitForSingleObject(guard.value.hProcess,25000)==WAIT_OBJECT_0,"Guard failed to complete recovery within bounded timeout");
        DWORD code=99; GetExitCodeProcess(guard.value.hProcess,&code); require(code==0,"Guard recovery returned failure");
        require(fs::exists(bundle/L"recovered.txt"),"Recovery callback did not run");
    }
    CloseHandle(ready); CloseHandle(accept); CloseHandle(revert);
    std::wcout << L"PASS: " << mode << (keep?L" keep/revert":L"") << (bad_ticks?L" identity rejection":L"") << '\n';
}
}
int wmain() {
    try {
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
        const auto binaries=fs::path(module).parent_path();
        wchar_t temp[32768]{}; GetTempPathW(32768,temp);
        const auto root=fs::path(temp)/(L"MinimaDock guard tests 雪 "+std::to_wstring(GetCurrentProcessId()));
        require(!fs::exists(root),"Test directory already exists"); fs::create_directory(root);
        scenario(binaries,root,L"crash"); scenario(binaries,root,L"hang"); scenario(binaries,root,L"timeout");
        scenario(binaries,root,L"revert"); scenario(binaries,root,L"timeout",true); scenario(binaries,root,L"timeout",false,true);
        // Only this new test directory under the resolved OS temporary directory is removed.
        auto expected_parent=fs::path(temp).lexically_normal();
        if(expected_parent.filename().empty()) expected_parent=expected_parent.parent_path();
        require(fs::absolute(root).lexically_normal().parent_path()==fs::absolute(expected_parent).lexically_normal(),"Unsafe test cleanup path");
        require((GetFileAttributesW(root.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)==0,"Test root unexpectedly became a reparse point");
        fs::remove_all(root);
        std::cout << "6 native guard scenarios passed; no desktop or registry settings were changed.\n"; return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
