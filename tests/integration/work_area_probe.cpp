#include "recovery/work_area.h"
#include <iostream>
#include <iomanip>
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        const auto areas=minima::monitor_work_areas();
        const auto rect=[](const RECT& r){std::wcout<<L"{\"Left\":"<<r.left<<L",\"Top\":"<<r.top<<L",\"Right\":"<<r.right<<L",\"Bottom\":"<<r.bottom<<L'}';};
        std::wcout<<L'[';
        bool first=true;
        for(const auto& a:areas){if(!first)std::wcout<<L',';first=false;std::wcout<<L"{\"Device\":"<<std::quoted(a.device)<<L",\"Bounds\":";rect(a.bounds);std::wcout<<L",\"Work\":";rect(a.original);std::wcout<<L'}';}
        std::wcout<<L"]\n";return 0;
    }catch(...){return 1;}
}
