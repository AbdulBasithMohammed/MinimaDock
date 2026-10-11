#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <filesystem>

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::wstring image_path(HANDLE p) {
    std::wstring path(32768, L'\0'); DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(p, 0, path.data(), &length)) throw std::runtime_error("Cannot query shell image.");
    path.resize(length); return path;
}
std::wstring owner(HANDLE p) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(p, TOKEN_QUERY, &raw)) throw std::runtime_error("Cannot read process owner.");
    Handle token(raw); DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size)) throw std::runtime_error("Cannot read token user.");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid)) throw std::runtime_error("Cannot format owner SID.");
    std::wstring result(sid); LocalFree(sid); return result;
}
std::wstring quote(const std::wstring& s) {
    // Windows CommandLineToArgvW quoting, including trailing backslashes.
    std::wstring result = L"\""; size_t slashes = 0;
    for (const auto ch : s) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') result.append(slashes * 2 + 1, L'\\'); else result.append(slashes, L'\\');
        slashes = 0; result.push_back(ch);
    }
    result.append(slashes * 2, L'\\'); result.push_back(L'\"'); return result;
}
void recover(const std::filesystem::path& bundle) {
    wchar_t windows[MAX_PATH]{}; if (!GetWindowsDirectoryW(windows, MAX_PATH)) throw std::runtime_error("Cannot locate Windows.");
    auto powershell = std::filesystem::path(windows) / L"System32/WindowsPowerShell/v1.0/powershell.exe";
    const auto script = bundle / L"Restore.ps1";
    if (!std::filesystem::is_regular_file(script) || !std::filesystem::is_regular_file(bundle / L"baseline.json")) throw std::runtime_error("Recovery bundle is missing. Open Task Manager and run explorer.exe for emergency desktop access.");
    auto command = quote(powershell.wstring()) + L" -NoLogo -NoProfile -ExecutionPolicy Bypass -File " + quote(script.wstring()) + L" -Bundle " + quote(bundle.wstring());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION info{};
    // Use a new console so recovery errors remain visible even after the shell crashes.
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, bundle.c_str(), &startup, &info)) throw std::runtime_error("Unable to launch recovery.");
    Handle process(info.hProcess), thread(info.hThread);
    const auto result = WaitForSingleObject(process.value, 30000);
    DWORD code = 1;
    if (result != WAIT_OBJECT_0 || !GetExitCodeProcess(process.value, &code) || code != 0) throw std::runtime_error("Recovery needs attention. No automatic sign-out was performed. Run Restore-Windows.cmd from Task Manager.");
}
unsigned long long number(const wchar_t* value) {
    wchar_t* end = nullptr; const auto result = wcstoull(value, &end, 0);
    if (!end || *end || value == end) throw std::runtime_error("Invalid numeric argument.");
    return result;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 10 || wcscmp(argv[1], L"--watch") != 0) {
            std::wcerr << L"MinimaDockGuard --watch PID HWND CREATION_FILETIME TIMEOUT_SECONDS BUNDLE READY_HANDLE KEEP_HANDLE REVERT_HANDLE\n"
                       << L"Internal trial supervisor. Activation is not implemented in this build.\n";
            return 2;
        }
        const auto pid_value = number(argv[2]);
        if (!pid_value || pid_value > MAXDWORD) throw std::runtime_error("Invalid PID.");
        const auto pid = static_cast<DWORD>(pid_value);
        const auto hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(number(argv[3])));
        const auto expected_time = number(argv[4]);
        const auto seconds = number(argv[5]);
        if (seconds < 10 || seconds > 600) throw std::runtime_error("Trial timeout must be 10 to 600 seconds.");
        const auto bundle = std::filesystem::absolute(argv[6]);
        // Events are inherited unnamed kernel objects supplied by the trial coordinator.
        // They are never opened by a guessable name or exposed as global keep/revert commands.
        Handle ready(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(number(argv[7]))));
        Handle keep(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(number(argv[8]))));
        Handle revert(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(number(argv[9]))));
        DWORD flags = 0;
        for (const auto event : {ready.value, keep.value, revert.value}) {
            if (!event || !GetHandleInformation(event, &flags) || !(flags & HANDLE_FLAG_INHERIT)) throw std::runtime_error("Trial decision handles were not inherited.");
        }
        Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process.value) throw std::runtime_error("Cannot open shell process.");
        DWORD ours = 0, theirs = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &ours) || !ProcessIdToSessionId(pid, &theirs) || ours != theirs || owner(process.value) != owner(GetCurrentProcess())) throw std::runtime_error("Shell user/session mismatch.");
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
        const auto expected_image = std::filesystem::path(module).parent_path() /
#ifdef MINIMA_GUARD_TEST
            L"MinimaDockGuardFixture.exe";
#else
            L"MinimaDock.exe";
#endif
        if (_wcsicmp(image_path(process.value).c_str(), expected_image.c_str()) != 0) throw std::runtime_error("Shell image does not match colocated binary.");
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process.value, &created, &exited, &kernel, &user)) throw std::runtime_error("Cannot validate creation time.");
        ULARGE_INTEGER ticks{}; ticks.LowPart=created.dwLowDateTime; ticks.HighPart=created.dwHighDateTime;
        if (ticks.QuadPart != expected_time) throw std::runtime_error("Shell PID creation time mismatch.");
        DWORD window_pid=0; GetWindowThreadProcessId(hwnd,&window_pid);
        if (!IsWindow(hwnd) || window_pid != pid) throw std::runtime_error("Liveness window does not belong to shell.");
        if (!SetEvent(ready.value)) throw std::runtime_error("Cannot acknowledge guard readiness.");
        const auto deadline = GetTickCount64() + seconds * 1000;
        bool accepted = false; unsigned missed = 0;
        HANDLE waits[] = { process.value, revert.value, keep.value };
        for (;;) {
            const auto result = WaitForMultipleObjects(accepted ? 2 : 3, waits, FALSE, 3000);
            if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1) break;
            if (result == WAIT_OBJECT_0 + 2) { accepted = true; continue; }
            if (result != WAIT_TIMEOUT) throw std::runtime_error("Guard wait failed.");
            if (!accepted && GetTickCount64() >= deadline) break;
            DWORD_PTR reply = 0;
            // WM_NULL must be handled by the shell UI thread, not a worker heartbeat.
            if (!IsWindow(hwnd) || !SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &reply)) ++missed; else missed = 0;
            if (missed >= 2) break;
        }
        recover(bundle);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
#ifndef MINIMA_GUARD_TEST
        MessageBoxA(nullptr, e.what(), "MinimaDock recovery guard", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
        return 1;
    }
}
