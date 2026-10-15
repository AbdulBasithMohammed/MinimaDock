#include "windows/file_search.h"
#include "platform/com_ptr.h"
#include "taskbar/launcher_model.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propsys.h>
#include <oledb.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <cwctype>

namespace minima {
namespace {
// Windows Search's OLE DB provider (Search.CollatorDSO) and the default dialect.
constexpr CLSID collator_data_source={0x9E175B8B,0xF52A,0x11D8,{0xB9,0xA5,0x50,0x50,0x54,0x50,0x30,0x30}};
constexpr GUID default_dialect={0xc8b521fb,0x5cf3,0x11ce,{0xad,0xe5,0x00,0xaa,0x00,0x44,0x77,0x3d}};
constexpr PROPERTYKEY item_path_display={{0xE3E0584C,0xB788,0x4A5A,{0xBB,0x20,0x7F,0x5A,0x44,0xC9,0xAC,0xDD}},7};

std::shared_ptr<void> own_bitmap(HBITMAP bitmap){return bitmap?std::shared_ptr<void>(bitmap,[](void* b){DeleteObject(static_cast<HBITMAP>(b));}):nullptr;}
// Some thumbnail providers return 32-bit bitmaps with no alpha at all; make those opaque.
void fix_alpha(HBITMAP bitmap) {
    DIBSECTION dib{};
    if(GetObjectW(bitmap,sizeof(dib),&dib)!=sizeof(dib)||dib.dsBm.bmBitsPixel!=32||!dib.dsBm.bmBits)return;
    auto* pixels=static_cast<std::uint8_t*>(dib.dsBm.bmBits);const size_t count=static_cast<size_t>(dib.dsBm.bmWidth)*static_cast<size_t>(std::abs(dib.dsBm.bmHeight));
    for(size_t i=0;i<count;++i)if(pixels[i*4+3])return;
    for(size_t i=0;i<count;++i)pixels[i*4+3]=255;
}
std::shared_ptr<void> shell_image(const std::wstring& path,int size,SIIGBF flags,int* width=nullptr,int* height=nullptr) {
    ComPtr<IShellItemImageFactory> images;HBITMAP image=nullptr;
    if(FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(images.put())))||FAILED(images->GetImage(SIZE{size,size},flags,&image)))return nullptr;
    fix_alpha(image);
    BITMAP info{};if(GetObjectW(image,sizeof(info),&info)){if(width)*width=info.bmWidth;if(height)*height=std::abs(info.bmHeight);}
    return own_bitmap(image);
}
ULONGLONG ticks(FILETIME time){return (static_cast<ULONGLONG>(time.dwHighDateTime)<<32)|time.dwLowDateTime;}
bool stat(FileHit& hit) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(!GetFileAttributesExW(hit.path.c_str(),GetFileExInfoStandard,&data))return false;
    hit.folder=(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    hit.modified=ticks(data.ftLastWriteTime);hit.size=(static_cast<ULONGLONG>(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
    return (data.dwFileAttributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM))==0;
}
std::wstring leaf(const std::wstring& path) {
    auto end=path.size();while(end>3&&(path[end-1]==L'\\'||path[end-1]==L'/'))--end;
    const auto slash=path.find_last_of(L"\\/",end-1);
    return slash==std::wstring::npos?path.substr(0,end):path.substr(slash+1,end-slash-1);
}
// Places nobody means when they search for a file.
bool noise(const std::wstring& path) {
    const auto p=launcher::lower(path);
    for(const auto* part:{L"\\appdata\\",L"\\node_modules\\",L"\\.git\\",L"\\$recycle.bin\\",L"\\windows\\winsxs\\",L"\\__pycache__\\"})if(p.find(part)!=std::wstring::npos)return true;
    return p.find(L"\\.")!=std::wstring::npos; // Dot folders such as .cache or .vscode.
}

// Windows' Recent items (the .lnk files in %APPDATA%\Microsoft\Windows\Recent), newest first.
std::vector<FileHit> recent_files() {
    std::vector<std::pair<ULONGLONG,std::wstring>> links;
    PWSTR folder=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Recent,0,nullptr,&folder)))return {};
    const std::wstring base=folder;CoTaskMemFree(folder);
    WIN32_FIND_DATAW data{};const auto find=FindFirstFileW((base+L"\\*.lnk").c_str(),&data);
    if(find==INVALID_HANDLE_VALUE)return {};
    do links.push_back({ticks(data.ftLastWriteTime),base+L"\\"+data.cFileName});while(FindNextFileW(find,&data));
    FindClose(find);
    std::sort(links.begin(),links.end(),[](const auto& a,const auto& b){return a.first>b.first;});
    std::vector<FileHit> hits;
    for(const auto& [used,link]:links) {
        if(hits.size()>=150)break;
        ComPtr<IShellLinkW> shortcut;ComPtr<IPersistFile> file;wchar_t target[MAX_PATH]{};
        if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(shortcut.put())))||
           FAILED(shortcut->QueryInterface(IID_PPV_ARGS(file.put())))||FAILED(file->Load(link.c_str(),STGM_READ))||
           shortcut->GetPath(target,MAX_PATH,nullptr,SLGP_RAWPATH)!=S_OK||!*target)continue;
        wchar_t expanded[MAX_PATH]{};ExpandEnvironmentStringsW(target,expanded,MAX_PATH);
        FileHit hit;hit.path=expanded;hit.name=leaf(hit.path);hit.recent=true;
        if(hit.name.empty()||hit.path.size()<=3||noise(hit.path)||!stat(hit))continue;
        if(std::any_of(hits.begin(),hits.end(),[&](const FileHit& h){return _wcsicmp(h.path.c_str(),hit.path.c_str())==0;}))continue;
        hit.modified=std::max(hit.modified,used);
        hits.push_back(std::move(hit));
    }
    return hits;
}

// File names in the Windows Search index that start words with every query word.
std::vector<std::wstring> index_files(const std::wstring& query) {
    std::wstring terms;
    for(size_t from=0;from<query.size();) {
        auto end=query.find(L' ',from);if(end==std::wstring::npos)end=query.size();
        std::wstring word;for(const auto c:query.substr(from,end-from))if(c!=L'\''&&c!=L'"'&&c!=L'*')word+=c;
        if(!word.empty())terms+=(terms.empty()?L"":L" AND ")+(L"\""+word+L"*\"");
        from=end+1;
    }
    if(terms.empty())return {};
    PWSTR profile=nullptr;std::wstring scope=L"file:";
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile,0,nullptr,&profile))){scope+=profile;std::replace(scope.begin(),scope.end(),L'\\',L'/');}
    CoTaskMemFree(profile);
    for(auto& c:scope)if(c==L'\'')c=L' ';
    const std::wstring sql=L"SELECT TOP 80 System.ItemPathDisplay FROM SystemIndex WHERE SCOPE='"+scope+L"' AND CONTAINS(System.FileName,'"+terms+L"') ORDER BY System.DateModified DESC";
    std::vector<std::wstring> paths;
    ComPtr<IDBInitialize> source;ComPtr<IDBCreateSession> sessions;ComPtr<IDBCreateCommand> commands;ComPtr<ICommandText> command;ComPtr<IRowset> rows;ComPtr<IGetRow> rowsets;
    if(FAILED(CoCreateInstance(collator_data_source,nullptr,CLSCTX_INPROC_SERVER,IID_IDBInitialize,reinterpret_cast<void**>(source.put())))||FAILED(source->Initialize()))return paths;
    if(FAILED(source->QueryInterface(IID_IDBCreateSession,reinterpret_cast<void**>(sessions.put())))||
       FAILED(sessions->CreateSession(nullptr,IID_IDBCreateCommand,reinterpret_cast<IUnknown**>(commands.put())))||
       FAILED(commands->CreateCommand(nullptr,IID_ICommandText,reinterpret_cast<IUnknown**>(command.put())))||
       FAILED(command->SetCommandText(default_dialect,sql.c_str()))||
       FAILED(command->Execute(nullptr,IID_IRowset,nullptr,nullptr,reinterpret_cast<IUnknown**>(rows.put())))||!rows||
       FAILED(rows->QueryInterface(IID_IGetRow,reinterpret_cast<void**>(rowsets.put())))){source->Uninitialize();return paths;}
    for(;;) {
        HROW buffer[16];HROW* handles=buffer;DBCOUNTITEM fetched=0;
        if(FAILED(rows->GetNextRows(DB_NULL_HCHAPTER,0,16,&fetched,&handles))||!fetched)break;
        for(DBCOUNTITEM i=0;i<fetched;++i) {
            ComPtr<IPropertyStore> store;
            if(FAILED(rowsets->GetRowFromHROW(nullptr,handles[i],IID_IPropertyStore,reinterpret_cast<IUnknown**>(store.put()))))continue;
            PROPVARIANT value;PropVariantInit(&value);
            if(SUCCEEDED(store->GetValue(item_path_display,&value))&&value.vt==VT_LPWSTR&&value.pwszVal)paths.emplace_back(value.pwszVal);
            PropVariantClear(&value);
        }
        rows->ReleaseRows(fetched,handles,nullptr,nullptr,nullptr);
        if(fetched<16)break;
    }
    rows.reset();rowsets.reset();command.reset();commands.reset();sessions.reset();source->Uninitialize();
    return paths;
}

std::wstring extension(const std::wstring& path) {
    const auto dot=path.find_last_of(L'.'),slash=path.find_last_of(L"\\/");
    return dot==std::wstring::npos||(slash!=std::wstring::npos&&dot<slash)?L"":launcher::lower(path.substr(dot));
}
bool text_file(const std::wstring& path) {
    static const wchar_t* kinds[]={L".txt",L".md",L".log",L".json",L".xml",L".csv",L".ini",L".cfg",L".conf",L".yaml",L".yml",L".toml",L".ps1",L".psm1",L".py",
        L".js",L".ts",L".tsx",L".jsx",L".cpp",L".cc",L".h",L".hpp",L".c",L".cs",L".java",L".kt",L".rs",L".go",L".rb",L".php",L".lua",L".html",L".htm",L".css",
        L".scss",L".bat",L".cmd",L".sh",L".sql",L".gitignore",L".editorconfig",L".svg",L".srt",L".tex"};
    const auto e=extension(path);
    return std::any_of(std::begin(kinds),std::end(kinds),[&](const wchar_t* k){return e==k;});
}
std::vector<std::wstring> text_lines(const std::wstring& path) {
    std::vector<std::wstring> lines;
    const auto file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return lines;
    std::string bytes(4096,'\0');DWORD read=0;
    ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr);CloseHandle(file);bytes.resize(read);
    std::wstring text;
    if(bytes.size()>=2&&static_cast<unsigned char>(bytes[0])==0xFF&&static_cast<unsigned char>(bytes[1])==0xFE) {
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data()+2),(bytes.size()-2)/2);
    } else {
        size_t start=bytes.size()>=3&&bytes.compare(0,3,"\xEF\xBB\xBF")==0?3:0;
        if(std::find(bytes.begin()+static_cast<std::ptrdiff_t>(start),bytes.end(),'\0')!=bytes.end())return lines; // Binary.
        const int length=MultiByteToWideChar(CP_UTF8,0,bytes.data()+start,static_cast<int>(bytes.size()-start),nullptr,0);
        text.resize(static_cast<size_t>(std::max(0,length)));
        if(length>0)MultiByteToWideChar(CP_UTF8,0,bytes.data()+start,static_cast<int>(bytes.size()-start),text.data(),length);
    }
    std::wstring current;
    for(const auto c:text) {
        if(c==L'\n'){lines.push_back(current);current.clear();if(lines.size()>=16)break;}
        else if(c==L'\t')current+=L"    ";
        else if(c!=L'\r'&&current.size()<120)current+=c;
    }
    if(lines.size()<16&&!current.empty())lines.push_back(current);
    return lines;
}
}

std::vector<FileHit> find_files(const std::wstring& raw,size_t limit) {
    std::wstring query=raw;
    while(!query.empty()&&query.back()==L' ')query.pop_back();
    while(!query.empty()&&query.front()==L' ')query.erase(query.begin());
    auto recent=recent_files();
    std::vector<FileHit> hits;
    if(query.empty()) {
        for(auto& hit:recent){if(hits.size()>=limit)break;hits.push_back(std::move(hit));}
    } else {
        // Recently used files that match rank above the rest.
        for(auto& hit:recent)if(const double s=launcher::match(hit.name,query);s>=0){hit.score=s+30;hits.push_back(std::move(hit));}
        for(const auto& path:index_files(query)) {
            if(noise(path)||std::any_of(hits.begin(),hits.end(),[&](const FileHit& h){return _wcsicmp(h.path.c_str(),path.c_str())==0;}))continue;
            FileHit hit;hit.path=path;hit.name=leaf(path);
            const double s=launcher::match(hit.name,query);
            if(s<0||!stat(hit))continue;
            hit.score=s;hits.push_back(std::move(hit));
        }
        std::stable_sort(hits.begin(),hits.end(),[](const FileHit& a,const FileHit& b){return a.score!=b.score?a.score>b.score:a.modified>b.modified;});
        if(hits.size()>limit)hits.resize(limit);
    }
    for(auto& hit:hits)hit.image=shell_image(hit.path,64,static_cast<SIIGBF>(SIIGBF_ICONONLY|SIIGBF_BIGGERSIZEOK));
    return hits;
}

FilePreview make_file_preview(const std::wstring& path) {
    FilePreview preview;preview.path=path;
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES)return preview;
    preview.folder=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    SHFILEINFOW info{};
    if(SHGetFileInfoW(path.c_str(),0,&info,sizeof(info),SHGFI_TYPENAME))preview.kind=info.szTypeName;
    if(preview.folder) {
        std::vector<std::wstring> folders,files;
        WIN32_FIND_DATAW data{};const auto find=FindFirstFileW((path+L"\\*").c_str(),&data);
        if(find!=INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name=data.cFileName;
                if(name==L"."||name==L".."||(data.dwFileAttributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)))continue;
                ++preview.children;
                if(folders.size()+files.size()<40)((data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?folders:files).push_back(name);
            } while(preview.children<1000&&FindNextFileW(find,&data));
            FindClose(find);
        }
        const auto by_name=[](const std::wstring& a,const std::wstring& b){return _wcsicmp(a.c_str(),b.c_str())<0;};
        std::sort(folders.begin(),folders.end(),by_name);std::sort(files.begin(),files.end(),by_name);
        for(auto& f:folders)preview.lines.push_back(L"\x25B8 "+f);   // ▸ marks folders.
        for(auto& f:files)preview.lines.push_back(f);
        if(preview.lines.size()>9)preview.lines.resize(9);
    } else if(text_file(path)) {
        preview.lines=text_lines(path);preview.text=!preview.lines.empty();
    }
    if(!preview.text)preview.thumbnail=shell_image(path,256,SIIGBF_BIGGERSIZEOK,&preview.width,&preview.height);
    return preview;
}

struct FileSearch::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<HWND> notify=nullptr;
    std::thread worker;
    bool stopping=false,search_wanted=false,preview_wanted=false;
    std::wstring search_query,preview_path;
    std::shared_ptr<const FileResults> results=std::make_shared<FileResults>();
    std::shared_ptr<const FilePreview> preview=std::make_shared<FilePreview>();
    void run() {
        const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        for(;;) {
            bool searching=false;std::wstring query,path;
            {
                std::unique_lock lock(mutex);wake.wait(lock,[&]{return stopping||search_wanted||preview_wanted;});
                if(stopping)break;
                if(search_wanted){searching=true;query=search_query;search_wanted=false;}
                else {path=preview_path;preview_wanted=false;}
            }
            if(searching) {
                auto found=std::make_shared<FileResults>();found->query=query;found->hits=find_files(query);
                {std::lock_guard lock(mutex);if(search_wanted&&search_query!=query)continue;results=std::move(found);} // A newer query is waiting.
                if(const auto window=notify.load())PostMessageW(window,file_search_message,0,0);
            } else {
                auto made=std::make_shared<FilePreview>(make_file_preview(path));
                {std::lock_guard lock(mutex);if(preview_wanted&&preview_path!=path)continue;preview=std::move(made);}
                if(const auto window=notify.load())PostMessageW(window,file_search_message,1,0);
            }
        }
        if(SUCCEEDED(apartment))CoUninitialize();
    }
};
FileSearch::FileSearch():impl_(std::make_unique<Impl>()){}
FileSearch::~FileSearch(){stop();}
void FileSearch::start(HWND notify) {
    stop();impl_->notify=notify;impl_->stopping=false;
    impl_->worker=std::thread([i=impl_.get()]{i->run();});
}
void FileSearch::stop() {
    auto& i=*impl_;i.notify=nullptr;if(!i.worker.joinable())return;
    {std::lock_guard lock(i.mutex);i.stopping=true;}
    i.wake.notify_all();i.worker.join();
}
void FileSearch::search(const std::wstring& query) {
    {std::lock_guard lock(impl_->mutex);impl_->search_query=query;impl_->search_wanted=true;}
    impl_->wake.notify_all();
}
void FileSearch::preview(const std::wstring& path) {
    {std::lock_guard lock(impl_->mutex);if(impl_->preview->path==path&&!impl_->preview_wanted)return;impl_->preview_path=path;impl_->preview_wanted=true;}
    impl_->wake.notify_all();
}
std::shared_ptr<const FileResults> FileSearch::results() const{std::lock_guard lock(impl_->mutex);return impl_->results;}
std::shared_ptr<const FilePreview> FileSearch::preview_result() const{std::lock_guard lock(impl_->mutex);return impl_->preview;}

bool show_in_folder(const std::wstring& path) {
    PIDLIST_ABSOLUTE item=nullptr;
    if(FAILED(SHParseDisplayName(path.c_str(),nullptr,&item,0,nullptr)))return false;
    const bool ok=SUCCEEDED(SHOpenFolderAndSelectItems(item,0,nullptr,0));
    CoTaskMemFree(item);return ok;
}
bool open_with(HWND owner,const std::wstring& path) {
    OPENASINFO info{};info.pcszFile=path.c_str();info.oaifInFlags=OAIF_ALLOW_REGISTRATION|OAIF_EXEC;
    return SUCCEEDED(SHOpenWithDialog(owner,&info));
}
bool show_properties(HWND owner,const std::wstring& path) {
    SHELLEXECUTEINFOW info{};info.cbSize=sizeof(info);info.fMask=SEE_MASK_INVOKEIDLIST;info.hwnd=owner;info.lpVerb=L"properties";info.lpFile=path.c_str();info.nShow=SW_SHOW;
    return ShellExecuteExW(&info)!=FALSE;
}
bool copy_file(HWND owner,const std::wstring& path) {
    // CF_HDROP: Paste in File Explorer copies the file.
    const size_t bytes=sizeof(DROPFILES)+(path.size()+2)*sizeof(wchar_t);
    const auto memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,bytes);if(!memory)return false;
    if(auto* drop=static_cast<DROPFILES*>(GlobalLock(memory))) {
        drop->pFiles=sizeof(DROPFILES);drop->fWide=TRUE;
        std::copy(path.begin(),path.end(),reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop)+sizeof(DROPFILES)));
        GlobalUnlock(memory);
    }
    if(!OpenClipboard(owner)){GlobalFree(memory);return false;}
    EmptyClipboard();const bool ok=SetClipboardData(CF_HDROP,memory)!=nullptr;
    CloseClipboard();if(!ok)GlobalFree(memory);
    return ok;
}
bool open_terminal(const std::wstring& folder) {
    // Windows Terminal when it is installed, else PowerShell started in the folder.
    // The folder is the working directory, never part of the command line: wt
    // splits its arguments at ";" even inside quotes, so a folder name could
    // otherwise add a command of its own.
    if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",L"wt.exe",L"-d .",folder.c_str(),SW_SHOWNORMAL))>32)return true;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",L"powershell.exe",L"-NoExit",folder.c_str(),SW_SHOWNORMAL))>32;
}
std::wstring app_file_path(const std::wstring& id) {
    std::wstring path;
    if(id.size()>3&&id[0]==L'{') {
        const auto close=id.find(L'}');
        if(close!=std::wstring::npos&&close+1<id.size()&&id[close+1]==L'\\') {
            GUID folder{};PWSTR base=nullptr;
            if(SUCCEEDED(CLSIDFromString(id.substr(0,close+1).c_str(),&folder))&&SUCCEEDED(SHGetKnownFolderPath(folder,0,nullptr,&base)))path=std::wstring(base)+id.substr(close+1);
            CoTaskMemFree(base);
        }
    } else if(id.size()>3&&id[1]==L':'&&id[2]==L'\\')path=id;
    return !path.empty()&&GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES?path:L"";
}
std::wstring format_size(ULONGLONG bytes) {
    const wchar_t* units[]={L"bytes",L"KB",L"MB",L"GB",L"TB"};double value=static_cast<double>(bytes);size_t unit=0;
    while(value>=1024&&unit<4){value/=1024;++unit;}
    wchar_t text[48]{};
    if(unit==0)swprintf(text,48,L"%llu bytes",bytes);else swprintf(text,48,value<10?L"%.1f %ls":L"%.0f %ls",value,units[unit]);
    return text;
}
std::wstring format_date(ULONGLONG filetime) {
    FILETIME utc{static_cast<DWORD>(filetime),static_cast<DWORD>(filetime>>32)},local{};SYSTEMTIME time{};
    if(!filetime||!FileTimeToLocalFileTime(&utc,&local)||!FileTimeToSystemTime(&local,&time))return {};
    wchar_t date[64]{},clock[32]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,0,&time,L"d MMM yyyy",date,64,nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,&time,nullptr,clock,32);
    return std::wstring(date)+L", "+clock;
}
}
