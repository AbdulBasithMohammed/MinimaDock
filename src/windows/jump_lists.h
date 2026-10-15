#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <string>
#include <vector>
#include "platform/com_ptr.h"
#include "windows/inventory.h"

namespace minima {
// One entry of an app's Windows jump list: a destination (IShellItem), a task or
// custom link (IShellLink), or a task separator.
struct JumpItem {
    enum class Kind {Item,Link,Separator};
    Kind kind=Kind::Item;
    std::wstring name;
    ComPtr<IUnknown> object;
    ComPtr<IShellItem> image; // Item whose Shell icon represents the entry.
    NativeIcon icon;          // Explicit link icon, when the link names one.
};
struct JumpGroup {
    enum class Kind {Pinned,Recent,Frequent,Custom,Tasks};
    Kind kind=Kind::Recent;
    std::wstring name;
    std::vector<JumpItem> items;
};
// The AppUserModelID Explorer files the app's jump list under, explicit or
// implicit (resolved from the window, shortcut or executable as Explorer does).
std::wstring jump_list_app_id(const std::vector<HWND>& windows,const std::wstring& launch_path,
                              const std::wstring& executable,const std::wstring& known);
// Pinned, the app's own categories (Recent/Frequent/custom) and Tasks, in
// Explorer's order. `max_items` caps each destination category.
std::vector<JumpGroup> read_jump_list(const std::wstring& app_id,unsigned max_items,UINT icon_pixels);
// Explorer's "Number of recent items to display in Jump Lists" (default 10).
unsigned jump_list_item_limit();
bool invoke_jump_item(HWND owner,const JumpItem& item);
// "Pin to this list" / "Unpin from this list".
bool pin_jump_item(const std::wstring& app_id,const JumpItem& item,bool pin);
// "Remove from this list": history comes from the automatic list, custom
// categories from the app's own list.
bool remove_jump_item(const std::wstring& app_id,JumpGroup::Kind group,const JumpItem& item);
// --jump-list-report: read-only diagnostic of the lists resolved for open windows
// (AppIDs, category names and item counts; no item names).
int run_jump_list_report(const wchar_t* report);
}
