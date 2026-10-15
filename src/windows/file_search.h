#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace minima {
// Posted to the notify window: wParam 0 = new search results, 1 = new preview.
inline constexpr UINT file_search_message=WM_APP+28;
struct FileHit {
    std::wstring name,path;
    bool folder=false,recent=false;
    ULONGLONG modified=0,size=0;   // FILETIME ticks; bytes.
    double score=0;
    std::shared_ptr<void> image;   // 64 px icon (HBITMAP, premultiplied), or null.
};
struct FileResults {std::wstring query;std::vector<FileHit> hits;};
struct FilePreview {
    std::wstring path,kind;
    std::shared_ptr<void> thumbnail;    // Up to 256 px (HBITMAP, premultiplied), or null.
    int width=0,height=0;
    bool text=false,folder=false;
    std::vector<std::wstring> lines;    // Start of a text file, or a folder's first items.
    size_t children=0;                  // Items in a folder (capped).
};
// Files for the launcher: Windows' Recent items plus the Windows Search index
// (file names only, on this PC). Runs on its own thread; the latest request wins.
class FileSearch {
public:
    FileSearch();
    ~FileSearch();
    FileSearch(const FileSearch&)=delete;
    FileSearch& operator=(const FileSearch&)=delete;
    void start(HWND notify);
    void stop();
    void search(const std::wstring& query);   // "" = recent files.
    void preview(const std::wstring& path);
    std::shared_ptr<const FileResults> results() const;
    std::shared_ptr<const FilePreview> preview_result() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Synchronous lookup used by --file-search-report.
std::vector<FileHit> find_files(const std::wstring& query,size_t limit=8);
FilePreview make_file_preview(const std::wstring& path);

// File actions.
bool show_in_folder(const std::wstring& path);
bool open_with(HWND owner,const std::wstring& path);
bool show_properties(HWND owner,const std::wstring& path);
bool copy_file(HWND owner,const std::wstring& path);
bool open_terminal(const std::wstring& folder);
// A desktop app's file ("{known folder}\app.exe" or a full path), or "" for Store apps.
std::wstring app_file_path(const std::wstring& id);
std::wstring format_size(ULONGLONG bytes);
std::wstring format_date(ULONGLONG filetime);
}
