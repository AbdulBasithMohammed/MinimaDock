#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace minima {
// Posted to the notify window whenever a snapshot field may have changed.
inline constexpr UINT connectivity_changed_message=WM_APP+19;
struct WifiNetwork {
    std::wstring ssid,profile;
    int signal=0;                  // 0-100
    bool secured=false,connected=false,saved=false;
    bool password_supported=true;  // false: enterprise sign-in, handled by Settings
    int auth=0,cipher=0;           // DOT11 algorithms, for a new network's profile.
};
struct BluetoothDevice {
    enum class Kind {Other,Audio,Keyboard,Mouse,Phone,Computer};
    std::wstring name,key;
    Kind kind=Kind::Other;
    bool connected=false;
    bool switchable=false; // Bluetooth audio: connect/disconnect on request.
};
struct ConnectivitySnapshot {
    bool wifi_present=false,wifi_on=false,bluetooth_present=false,bluetooth_on=false;
    bool wifi_connected=false,location_denied=false,scanning=false;
    std::wstring wifi_name;        // Connected network, when Windows reveals it.
    std::wstring connecting;       // SSID being joined.
    std::wstring wifi_message;     // Last Wi-Fi failure, for the network page.
    std::wstring bluetooth_busy;   // Device key with a pending connect/disconnect.
    std::vector<WifiNetwork> networks;
    std::vector<BluetoothDevice> devices;
};
// Wi-Fi and Bluetooth for quick settings: radio on/off (Windows.Devices.Radios),
// Wi-Fi networks (Native Wifi API) and paired Bluetooth devices. All Windows
// calls run on one worker thread; the GUI thread only reads snapshots.
class Connectivity {
public:
    Connectivity();
    ~Connectivity();
    Connectivity(const Connectivity&)=delete;
    Connectivity& operator=(const Connectivity&)=delete;
    void start(HWND notify);
    void stop();
    ConnectivitySnapshot snapshot() const;
    void refresh(bool lists);       // Radio states, and the network/device lists.
    void scan();                    // Ask the Wi-Fi adapter for a fresh scan.
    void set_wifi(bool on);
    void set_bluetooth(bool on);
    // Saved or open networks connect directly; others use `password`.
    void connect_wifi(const WifiNetwork& network,std::wstring password);
    void disconnect_wifi();
    void connect_bluetooth(const std::wstring& key,bool connect);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// --quick-settings-report: counts and flags only (src/windows/quick_report.cpp).
int run_quick_settings_report(const wchar_t* report);
}
