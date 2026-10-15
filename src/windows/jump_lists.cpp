#include "windows/jump_lists.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <knownfolders.h>
#include <propsys.h>
#include <propkey.h>
#include <algorithm>
#include <cwctype>
#include <string_view>

// Explorer keeps each app's jump list in two stores: the automatic list (Pinned,
// Recent, Frequent) and the app's custom list (its own categories and Tasks).
// Windows exposes no public reader, so this uses the same Shell interfaces as
// Open-Shell's jump lists and System Informer's app resolver. Every interface
// is reached by QueryInterface on its known IID: an unknown Windows build simply
// yields an empty list instead of a crash.
namespace minima {
namespace {
struct APPDESTCATEGORY {int type;union{wchar_t* name;int sub_type;};int count;int pad[10];};
constexpr CLSID clsid_destination_list={0x77f10cf0,0x3db5,0x4966,{0xb5,0x20,0xb7,0xc5,0x4f,0xd3,0x5e,0xd6}};
constexpr CLSID clsid_automatic_destination_list={0xf0ae1542,0xf497,0x484b,{0xa1,0x75,0xa2,0x0d,0xb0,0x91,0x44,0xba}};
constexpr CLSID clsid_application_resolver={0x660b90c8,0x73a9,0x4b58,{0x8c,0xae,0x35,0x5b,0x7f,0x55,0x34,0x1b}};
constexpr IID iid_destination_lists[]={
    {0x507101cd,0xf6ad,0x46c8,{0x8e,0x20,0xee,0xb9,0xe6,0xba,0xc4,0x7f}},  // Windows 10 1511+
    {0xfebd543d,0x1f7b,0x4b38,{0x94,0x0b,0x59,0x33,0xbd,0x2c,0xb2,0x1b}},  // Windows 10 RTM
    {0x03f1eed2,0x8676,0x430b,{0xab,0xe1,0x76,0x5c,0x1d,0x8f,0xe1,0x47}}}; // Windows 7/8
constexpr IID iid_automatic_list_10={0xe9c5ef8d,0xfd41,0x4f72,{0xba,0x87,0xeb,0x03,0xba,0xd5,0x81,0x7c}};
constexpr IID iid_automatic_list_7={0xbc10dce3,0x62f2,0x4bc6,{0xaf,0x37,0xdb,0x46,0xed,0x78,0x73,0xc4}};
constexpr IID iid_application_resolver={0xde25675a,0x72de,0x44b4,{0x93,0x73,0x05,0x17,0x04,0x50,0xc1,0x40}}; // Windows 8+
constexpr PROPERTYKEY key_host_environment={{0x9f4c2855,0x9f79,0x4b39,{0xa8,0xd0,0xe1,0xd4,0x2d,0xe1,0xd5,0xf3}},14};
constexpr PROPERTYKEY key_activation_context={{0x9f4c2855,0x9f79,0x4b39,{0xa8,0xd0,0xe1,0xd4,0x2d,0xe1,0xd5,0xf3}},20};

// Slots this code never calls are declared without parameters: only their
// position in the vtable matters.
struct IDestinationListPrivate:IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetMinItems()=0;
    virtual HRESULT STDMETHODCALLTYPE SetApplicationID(LPCWSTR app_id)=0;
    virtual HRESULT STDMETHODCALLTYPE GetSlotCount()=0;
    virtual HRESULT STDMETHODCALLTYPE GetCategoryCount(UINT* count)=0;
    virtual HRESULT STDMETHODCALLTYPE GetCategory(UINT index,int flags,APPDESTCATEGORY* category)=0;
    virtual HRESULT STDMETHODCALLTYPE DeleteCategory()=0;
    virtual HRESULT STDMETHODCALLTYPE EnumerateCategoryDestinations(UINT index,REFIID riid,void** result)=0;
    virtual HRESULT STDMETHODCALLTYPE RemoveDestination(IUnknown* item)=0;
};
struct IAutomaticDestinationList7:IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR app_id,LPCWSTR link,LPCWSTR reserved)=0;
    virtual HRESULT STDMETHODCALLTYPE HasList(BOOL* has)=0;
    virtual HRESULT STDMETHODCALLTYPE GetList(int type,unsigned max,REFIID riid,void** result)=0;
    virtual HRESULT STDMETHODCALLTYPE AddUsagePoint()=0;
    virtual HRESULT STDMETHODCALLTYPE PinItem(IUnknown* item,int index)=0; // -1 pins, -2 unpins.
    virtual HRESULT STDMETHODCALLTYPE IsPinned()=0;
    virtual HRESULT STDMETHODCALLTYPE RemoveDestination(IUnknown* item)=0;
};
struct IAutomaticDestinationList10:IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR app_id,LPCWSTR link,LPCWSTR reserved)=0;
    virtual HRESULT STDMETHODCALLTYPE HasList(BOOL* has)=0;
    virtual HRESULT STDMETHODCALLTYPE GetList(int type,unsigned max,unsigned flags,REFIID riid,void** result)=0;
    virtual HRESULT STDMETHODCALLTYPE AddUsagePoint()=0;
    virtual HRESULT STDMETHODCALLTYPE PinItem(IUnknown* item,int index)=0;
    virtual HRESULT STDMETHODCALLTYPE IsPinned()=0;
    virtual HRESULT STDMETHODCALLTYPE RemoveDestination(IUnknown* item)=0;
};
struct IApplicationResolver8:IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForShortcut(IShellItem* item,PWSTR* app_id)=0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForShortcutObject(IShellLinkW* link,IShellItem* item,PWSTR* app_id)=0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForWindow(HWND window,PWSTR* app_id,BOOL* pinning_prevented,BOOL* explicit_id,BOOL* embedded_shortcut)=0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForProcess(ULONG pid,PWSTR* app_id,BOOL* pinning_prevented,BOOL* explicit_id,BOOL* embedded_shortcut)=0;
    virtual HRESULT STDMETHODCALLTYPE GetShortcutForProcess(ULONG pid,IShellItem** item)=0;
    virtual HRESULT STDMETHODCALLTYPE GetBestShortcutForAppID(PCWSTR app_id,IShellItem** item)=0;
    virtual HRESULT STDMETHODCALLTYPE GetBestShortcutAndAppIDForAppPath(PCWSTR path,IShellItem** item,PWSTR* app_id)=0;
};
enum ListType {pinned_list=0,recent_list=1,frequent_list=2};

std::wstring take(PWSTR value){std::wstring result;if(value){result=value;CoTaskMemFree(value);}return result;}
std::wstring upper(std::wstring value){for(auto& c:value)c=static_cast<wchar_t>(std::towupper(c));return value;}
std::wstring indirect(const std::wstring& value) {
    if(value.empty()||value[0]!=L'@')return value;
    wchar_t text[512]{};return SUCCEEDED(SHLoadIndirectString(value.c_str(),text,512,nullptr))?text:value;
}
std::wstring property_text(IPropertyStore* store,const PROPERTYKEY& key) {
    PROPVARIANT value;PropVariantInit(&value);std::wstring result;
    if(SUCCEEDED(store->GetValue(key,&value))&&value.vt==VT_LPWSTR&&value.pwszVal)result=value.pwszVal;
    PropVariantClear(&value);return result;
}
bool property_flag(IPropertyStore* store,const PROPERTYKEY& key) {
    PROPVARIANT value;PropVariantInit(&value);
    const bool result=SUCCEEDED(store->GetValue(key,&value))&&value.vt==VT_BOOL&&value.boolVal;
    PropVariantClear(&value);return result;
}
class AutomaticList {
public:
    explicit AutomaticList(const std::wstring& app_id) {
        ComPtr<IUnknown> unknown;
        if(app_id.empty()||FAILED(CoCreateInstance(clsid_automatic_destination_list,nullptr,CLSCTX_INPROC_SERVER,IID_IUnknown,reinterpret_cast<void**>(unknown.put()))))return;
        if(SUCCEEDED(unknown->QueryInterface(iid_automatic_list_10,reinterpret_cast<void**>(modern_.put())))&&FAILED(modern_->Initialize(app_id.c_str(),nullptr,nullptr)))modern_.reset();
        if(!modern_&&SUCCEEDED(unknown->QueryInterface(iid_automatic_list_7,reinterpret_cast<void**>(legacy_.put())))&&FAILED(legacy_->Initialize(app_id.c_str(),nullptr,nullptr)))legacy_.reset();
    }
    ComPtr<IObjectCollection> list(ListType type,unsigned max) {
        ComPtr<IObjectCollection> result;void** out=reinterpret_cast<void**>(result.put());
        HRESULT hr=E_NOINTERFACE;
        if(modern_)hr=modern_->GetList(type,max,1,__uuidof(IObjectCollection),out);
        else if(legacy_)hr=legacy_->GetList(type,max,__uuidof(IObjectCollection),out);
        if(FAILED(hr))result.reset();return result;
    }
    bool pin(IUnknown* item,bool pin){return SUCCEEDED(modern_?modern_->PinItem(item,pin?-1:-2):legacy_?legacy_->PinItem(item,pin?-1:-2):E_NOINTERFACE);}
    bool remove(IUnknown* item){return SUCCEEDED(modern_?modern_->RemoveDestination(item):legacy_?legacy_->RemoveDestination(item):E_NOINTERFACE);}
private:
    ComPtr<IAutomaticDestinationList10> modern_;
    ComPtr<IAutomaticDestinationList7> legacy_;
};
ComPtr<IDestinationListPrivate> custom_list(const std::wstring& app_id) {
    ComPtr<IDestinationListPrivate> result;ComPtr<IUnknown> unknown;
    if(app_id.empty()||FAILED(CoCreateInstance(clsid_destination_list,nullptr,CLSCTX_INPROC_SERVER,IID_IUnknown,reinterpret_cast<void**>(unknown.put()))))return result;
    for(const auto& iid:iid_destination_lists)if(SUCCEEDED(unknown->QueryInterface(iid,reinterpret_cast<void**>(result.put()))))break;
    if(result&&FAILED(result->SetApplicationID(app_id.c_str())))result.reset();
    return result;
}
ComPtr<IApplicationResolver8> resolver() {
    ComPtr<IApplicationResolver8> result;
    if(FAILED(CoCreateInstance(clsid_application_resolver,nullptr,CLSCTX_INPROC_SERVER,iid_application_resolver,reinterpret_cast<void**>(result.put()))))result.reset();
    return result;
}
NativeIcon link_icon(IShellLinkW* link,UINT pixels) {
    wchar_t path[MAX_PATH]{};int index=0;
    if(FAILED(link->GetIconLocation(path,MAX_PATH,&index))||!path[0])return {};
    wchar_t expanded[MAX_PATH]{};if(!ExpandEnvironmentStringsW(path,expanded,MAX_PATH))return {};
    HICON icon=nullptr;
    if(SHDefExtractIconW(expanded,index,0,&icon,nullptr,pixels)!=S_OK||!icon)return {};
    return NativeIcon(icon,[](void* handle){DestroyIcon(static_cast<HICON>(handle));});
}
// Fills `out` from a destination object; `key` identifies it for de-duplication.
bool make_item(IUnknown* object,bool tasks,UINT pixels,JumpItem& out,std::wstring& key) {
    ComPtr<IShellItem> item;
    if(SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(item.put())))) {
        PWSTR name=nullptr;if(FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY,&name)))return false;
        out.kind=JumpItem::Kind::Item;out.name=take(name);
        PWSTR parsing=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&parsing)))key=upper(take(parsing));
        out.image=std::move(item);return !out.name.empty();
    }
    ComPtr<IShellLinkW> link;if(FAILED(object->QueryInterface(IID_PPV_ARGS(link.put()))))return false;
    out.kind=JumpItem::Kind::Link;
    ComPtr<IPropertyStore> store;if(FAILED(link->QueryInterface(IID_PPV_ARGS(store.put()))))store.reset();
    if(store&&tasks&&property_flag(store.get(),PKEY_AppUserModel_IsDestListSeparator)){out.kind=JumpItem::Kind::Separator;return true;}
    if(store)out.name=indirect(property_text(store.get(),PKEY_Title));
    PIDLIST_ABSOLUTE target=nullptr;
    if(SUCCEEDED(link->GetIDList(&target))&&target) {
        PWSTR text=nullptr;
        if(out.name.empty()&&SUCCEEDED(SHGetNameFromIDList(target,SIGDN_NORMALDISPLAY,&text)))out.name=take(text);
        if(SUCCEEDED(SHGetNameFromIDList(target,SIGDN_DESKTOPABSOLUTEPARSING,&text)))key=upper(take(text));
        if(FAILED(SHCreateItemFromIDList(target,IID_PPV_ARGS(out.image.put()))))out.image.reset();
        CoTaskMemFree(target);
    }
    if(store)key+=L"|"+property_text(store.get(),PKEY_Link_Arguments);
    out.icon=link_icon(link.get(),pixels);
    return !out.name.empty();
}
}

std::wstring jump_list_app_id(const std::vector<HWND>& windows,const std::wstring& launch_path,
                              const std::wstring& executable,const std::wstring& known) {
    const auto apps=resolver();
    if(apps)for(const auto window:windows) {
        PWSTR id=nullptr;
        if(IsWindow(window)&&SUCCEEDED(apps->GetAppIDForWindow(window,&id,nullptr,nullptr,nullptr)))if(auto result=take(id);!result.empty())return result;
    }
    constexpr std::wstring_view apps_folder=L"shell:AppsFolder\\";
    if(launch_path.size()>apps_folder.size()&&_wcsnicmp(launch_path.c_str(),apps_folder.data(),apps_folder.size())==0)return launch_path.substr(apps_folder.size());
    if(apps&&!launch_path.empty()) {
        ComPtr<IShellItem> item;PWSTR id=nullptr;
        if(SUCCEEDED(SHCreateItemFromParsingName(launch_path.c_str(),nullptr,IID_PPV_ARGS(item.put())))&&SUCCEEDED(apps->GetAppIDForShortcut(item.get(),&id)))
            if(auto result=take(id);!result.empty())return result;
    }
    if(apps&&!executable.empty()) {
        PWSTR id=nullptr;
        if(SUCCEEDED(apps->GetBestShortcutAndAppIDForAppPath(executable.c_str(),nullptr,&id)))if(auto result=take(id);!result.empty())return result;
    }
    return known;
}

std::vector<JumpGroup> read_jump_list(const std::wstring& app_id,unsigned max_items,UINT icon_pixels) {
    std::vector<JumpGroup> groups;if(app_id.empty())return groups;
    AutomaticList automatic(app_id);const auto custom=custom_list(app_id);
    std::vector<std::wstring> pinned_keys;
    const auto add=[&](JumpGroup::Kind kind,std::wstring name,IObjectCollection* collection,size_t limit) {
        UINT count=0;if(!collection||FAILED(collection->GetCount(&count)))return;
        JumpGroup group;group.kind=kind;group.name=std::move(name);size_t shown=0;
        for(UINT i=0;i<count&&shown<limit;++i) {
            ComPtr<IUnknown> object;if(FAILED(collection->GetAt(i,IID_IUnknown,reinterpret_cast<void**>(object.put())))||!object)continue;
            JumpItem item;std::wstring key;
            if(!make_item(object.get(),kind==JumpGroup::Kind::Tasks,icon_pixels,item,key))continue;
            if(item.kind!=JumpItem::Kind::Separator) {
                // Pinned destinations are not repeated in the history below them.
                if(kind!=JumpGroup::Kind::Pinned&&!key.empty()&&std::find(pinned_keys.begin(),pinned_keys.end(),key)!=pinned_keys.end())continue;
                if(kind==JumpGroup::Kind::Pinned&&!key.empty())pinned_keys.push_back(key);
                ++shown;
            }
            item.object=std::move(object);group.items.push_back(std::move(item));
        }
        if(shown)groups.push_back(std::move(group));
    };
    const unsigned fetch=max_items+20;
    add(JumpGroup::Kind::Pinned,L"Pinned",automatic.list(pinned_list,64).get(),64);
    UINT categories=0;if(!custom||FAILED(custom->GetCategoryCount(&categories)))categories=0;
    int tasks=-1;
    for(UINT index=0;index<categories;++index) {
        APPDESTCATEGORY category{};if(FAILED(custom->GetCategory(index,1,&category)))continue;
        if(category.type==0) {
            std::wstring name;if(category.name){name=indirect(category.name);CoTaskMemFree(category.name);}
            ComPtr<IObjectCollection> items;
            if(!name.empty()&&SUCCEEDED(custom->EnumerateCategoryDestinations(index,__uuidof(IObjectCollection),reinterpret_cast<void**>(items.put()))))
                add(JumpGroup::Kind::Custom,std::move(name),items.get(),max_items);
        } else if(category.type==1&&(category.sub_type==1||category.sub_type==2)) {
            const bool frequent=category.sub_type==1;
            add(frequent?JumpGroup::Kind::Frequent:JumpGroup::Kind::Recent,frequent?L"Frequent":L"Recent",automatic.list(frequent?frequent_list:recent_list,fetch).get(),max_items);
        } else if(category.type==2&&tasks<0)tasks=static_cast<int>(index);
    }
    // Apps without a custom list get Explorer's default Recent category.
    if(categories==0)add(JumpGroup::Kind::Recent,L"Recent",automatic.list(recent_list,fetch).get(),max_items);
    if(tasks>=0) {
        ComPtr<IObjectCollection> items;
        if(SUCCEEDED(custom->EnumerateCategoryDestinations(static_cast<UINT>(tasks),__uuidof(IObjectCollection),reinterpret_cast<void**>(items.put()))))
            add(JumpGroup::Kind::Tasks,L"Tasks",items.get(),64);
    }
    return groups;
}

unsigned jump_list_item_limit() {
    DWORD value=10,size=sizeof(value);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",L"Start_JumpListItems",RRF_RT_REG_DWORD,nullptr,&value,&size)!=ERROR_SUCCESS)value=10;
    return std::min<DWORD>(value,60);
}

bool invoke_jump_item(HWND owner,const JumpItem& entry) {
    if(!entry.object)return false;
    if(entry.kind==JumpItem::Kind::Item) {
        ComPtr<IShellItem> item;PIDLIST_ABSOLUTE id=nullptr;
        if(FAILED(entry.object->QueryInterface(IID_PPV_ARGS(item.put())))||FAILED(SHGetIDListFromObject(item.get(),&id)))return false;
        SHELLEXECUTEINFOW info{};info.cbSize=sizeof(info);info.fMask=SEE_MASK_IDLIST|SEE_MASK_FLAG_LOG_USAGE;
        info.hwnd=owner;info.lpIDList=id;info.nShow=SW_SHOWNORMAL;
        const bool opened=ShellExecuteExW(&info)!=FALSE;CoTaskMemFree(id);return opened;
    }
    if(entry.kind!=JumpItem::Kind::Link)return false;
    // Links run through their default verb, like Explorer. Packaged-app links
    // activate the app itself with the link's activation context.
    ComPtr<IContextMenu> menu;if(FAILED(entry.object->QueryInterface(IID_PPV_ARGS(menu.put()))))menu.reset();
    std::string parameters;
    ComPtr<IPropertyStore> store;
    if(SUCCEEDED(entry.object->QueryInterface(IID_PPV_ARGS(store.put())))) {
        const auto app=property_text(store.get(),PKEY_AppUserModel_ID);ComPtr<IShellItem2> target;ULONG hosted=0;
        if(!app.empty()&&SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder,0,app.c_str(),IID_PPV_ARGS(target.put())))&&
           SUCCEEDED(target->GetUInt32(key_host_environment,&hosted))&&hosted) {
            ComPtr<IContextMenu> app_menu;
            if(SUCCEEDED(target->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(app_menu.put())))) {
                menu=std::move(app_menu);
                const auto context=property_text(store.get(),key_activation_context);
                if(!context.empty()) {
                    const int size=WideCharToMultiByte(CP_ACP,0,context.c_str(),-1,nullptr,0,nullptr,nullptr);
                    if(size>1){parameters.resize(static_cast<size_t>(size));WideCharToMultiByte(CP_ACP,0,context.c_str(),-1,parameters.data(),size,nullptr,nullptr);parameters.pop_back();}
                }
            }
        }
    }
    if(!menu)return false;
    const auto popup=CreatePopupMenu();if(!popup)return false;
    bool invoked=false;
    if(SUCCEEDED(menu->QueryContextMenu(popup,0,1,1000,CMF_DEFAULTONLY))) {
        const auto id=static_cast<int>(GetMenuDefaultItem(popup,FALSE,0));
        if(id>0) {
            CMINVOKECOMMANDINFO command{};command.cbSize=sizeof(command);command.fMask=CMIC_MASK_FLAG_LOG_USAGE;
            command.lpVerb=MAKEINTRESOURCEA(id-1);command.lpParameters=parameters.empty()?nullptr:parameters.c_str();
            command.hwnd=owner;command.nShow=SW_SHOWNORMAL;
            invoked=SUCCEEDED(menu->InvokeCommand(&command));
        }
    }
    DestroyMenu(popup);return invoked;
}

bool pin_jump_item(const std::wstring& app_id,const JumpItem& item,bool pin) {
    return item.object&&AutomaticList(app_id).pin(item.object.get(),pin);
}

int run_jump_list_report(const wchar_t* report) {
    std::vector<HWND> windows;
    EnumWindows([](HWND window,LPARAM value)->BOOL{
        if(IsWindowVisible(window)&&!GetWindow(window,GW_OWNER)&&GetWindowTextLengthW(window)>0&&!(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOOLWINDOW))
            reinterpret_cast<std::vector<HWND>*>(value)->push_back(window);
        return TRUE;
    },reinterpret_cast<LPARAM>(&windows));
    std::wstring text;std::vector<std::wstring> seen;
    for(const auto window:windows) {
        const auto app_id=jump_list_app_id({window},L"",L"",L"");
        if(app_id.empty()||std::find(seen.begin(),seen.end(),app_id)!=seen.end())continue;
        seen.push_back(app_id);
        text+=app_id+L"\n";
        for(const auto& group:read_jump_list(app_id,jump_list_item_limit(),16)) {
            size_t items=0,separators=0,icons=0;
            for(const auto& item:group.items){if(item.kind==JumpItem::Kind::Separator)++separators;else ++items;if(item.icon||item.image)++icons;}
            text+=L"  ["+std::to_wstring(static_cast<int>(group.kind))+L"] "+group.name+L": "+std::to_wstring(items)+L" items, "+std::to_wstring(separators)+L" separators, "+std::to_wstring(icons)+L" with icons\n";
        }
    }
    text+=L"limit="+std::to_wstring(jump_list_item_limit())+L" windows="+std::to_wstring(windows.size())+L"\n";
    const int size=WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string bytes(static_cast<size_t>(std::max(0,size)),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),bytes.data(),size,nullptr,nullptr);
    const auto file=CreateFileW(report,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return 1;
    DWORD written=0;const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)!=FALSE;
    CloseHandle(file);return ok?0:1;
}

bool remove_jump_item(const std::wstring& app_id,JumpGroup::Kind group,const JumpItem& item) {
    if(!item.object)return false;
    if(group==JumpGroup::Kind::Custom){const auto list=custom_list(app_id);return list&&SUCCEEDED(list->RemoveDestination(item.object.get()));}
    return AutomaticList(app_id).remove(item.object.get());
}
}
