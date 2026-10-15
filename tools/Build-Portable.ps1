param([string]$Toolchain=(Join-Path $PSScriptRoot '..\.tools\llvm-mingw-20261006-ucrt-x86_64'))
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$compiler=Join-Path $Toolchain 'bin\x86_64-w64-mingw32-clang++.exe'
if(-not (Test-Path -LiteralPath $compiler)) { throw 'Portable toolchain missing. Use the MSVC CMake presets or the verified LLVM release described in docs/development.md.' }
Push-Location $repo
try {
    New-Item -ItemType Directory -Path build\portable -Force | Out-Null
    # --no-insert-timestamp: identical sources give an identical binary, so an
    # executable Windows already accepted keeps the same hash when rebuilt.
    $flags=@('-Wl,--no-insert-timestamp','-std=c++20','-Wall','-Wextra','-Wpedantic','-Werror','-O2','-static','-DUNICODE','-D_UNICODE','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-D_WIN32_WINNT=0x0A00','-Isrc')
    & $compiler @flags src\taskbar\state_machine.cpp tests\state_machines\model_tests.cpp -o build\portable\model_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Model build failed.'}
    & .\build\portable\model_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Model tests failed.'}
    & $compiler @flags tests\state_machines\calendar_tests.cpp -o build\portable\calendar_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Calendar model build failed.'}
    & .\build\portable\calendar_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Calendar model tests failed.'}
    & $compiler @flags tests\state_machines\media_marquee_tests.cpp -o build\portable\media_marquee_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Media marquee model build failed.'}
    & .\build\portable\media_marquee_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Media marquee model tests failed.'}
    & $compiler @flags tests\state_machines\prayer_tests.cpp -o build\portable\prayer_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Prayer times model build failed.'}
    & .\build\portable\prayer_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Prayer times model tests failed.'}
    & $compiler @flags tests\state_machines\launcher_tests.cpp -o build\portable\launcher_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Launcher model build failed.'}
    & .\build\portable\launcher_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Launcher model tests failed.'}
    & $compiler @flags tests\state_machines\prayer_methods_tests.cpp -o build\portable\prayer_methods_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Prayer methods build failed.'}
    & .\build\portable\prayer_methods_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Prayer methods tests failed.'}
    & $compiler @flags tests\state_machines\media_intent_tests.cpp -o build\portable\media_intent_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Media intent model build failed.'}
    & .\build\portable\media_intent_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Media intent model tests failed.'}
    & $compiler @flags tests\integration\pin_state_tests.cpp src\rendering\calendar.cpp src\rendering\glass.cpp src\rendering\previews.cpp src\rendering\jump_list.cpp src\rendering\prayer_toast.cpp src\rendering\customize.cpp src\windows\jump_lists.cpp src\windows\connectivity.cpp src\windows\updates.cpp src\windows\prayer_times.cpp src\windows\app_update.cpp src\windowspp_update.cpp src\windows\launcher_index.cpp src\windows\file_search.cpp src\windows\windows_key.cpp src\windows\volume.cpp src\windows\media.cpp src\windows\tray.cpp src\windows\inventory.cpp src\taskbar\state_machine.cpp -ld2d1 -ldwrite -ldwmapi -ld3d11 -lcoremessaging -ld3d11 -lshell32 -lole32 -loleaut32 -ladvapi32 -lwindowscodecs -luuid -lgdi32 -lruntimeobject -lshlwapi -lwlanapi -lbluetoothapis -lsetupapi -lcfgmgr32 -lwinmm -lwinhttp -lpowrprof -lcomdlg32 -o build\portable\pin_state_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Pinned-window regression build failed.'}
    & .\build\portable\pin_state_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Pinned-window regression failed.'}
    & $compiler @flags tests\integration\media_tests.cpp src\windows\media.cpp -lole32 -lshell32 -lruntimeobject -luuid -o build\portable\media_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Media fixture build failed.'}
    & $compiler @flags tests\integration\tray_tests.cpp src\windows\tray.cpp -lole32 -loleaut32 -lshell32 -luuid -o build\portable\tray_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Tray fixture build failed.'}
    & $compiler @flags tests\integration\media_ui_tests.cpp src\rendering\calendar.cpp src\rendering\glass.cpp src\rendering\previews.cpp src\rendering\jump_list.cpp src\rendering\prayer_toast.cpp src\rendering\customize.cpp src\windows\jump_lists.cpp src\windows\connectivity.cpp src\windows\updates.cpp src\windows\prayer_times.cpp src\windows\app_update.cpp src\windowspp_update.cpp src\windows\launcher_index.cpp src\windows\file_search.cpp src\windows\windows_key.cpp src\windows\volume.cpp src\windows\media.cpp src\windows\tray.cpp src\windows\inventory.cpp src\taskbar\state_machine.cpp -ld2d1 -ldwrite -ldwmapi -ld3d11 -lcoremessaging -ld3d11 -lshell32 -lole32 -loleaut32 -ladvapi32 -lwindowscodecs -luuid -lgdi32 -lruntimeobject -lshlwapi -lwlanapi -lbluetoothapis -lsetupapi -lcfgmgr32 -lwinmm -lwinhttp -lpowrprof -lcomdlg32 -o build\portable\media_ui_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Media UI build failed.'}
    & .\build\portable\media_ui_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Media UI tests failed.'}
    & $compiler @flags tests\integration\context_menu_tests.cpp -lole32 -lshell32 -luuid -ladvapi32 -lgdi32 -o build\portable\context_menu_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Context menu fixture build failed.'}
    & .\build\portable\context_menu_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Context menu fixture failed.'}
    & $compiler @flags tests\integration\calendar_ui_tests.cpp src\rendering\glass.cpp src\taskbar\state_machine.cpp -ld2d1 -ld3d11 -lcoremessaging -ldwmapi -lruntimeobject -ldwrite -lole32 -ladvapi32 -lwindowscodecs -luuid -lgdi32 -lshell32 -o build\portable\calendar_ui_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Calendar UI build failed.'}
    & .\build\portable\calendar_ui_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Calendar UI tests failed.'}
    & $compiler @flags tests\integration\desktop_tests.cpp -ldwmapi -o build\portable\desktop_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Desktop toggle test build failed.'}
    & .\build\portable\desktop_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Desktop toggle tests failed.'}
    & $compiler @flags tests\integration\work_area_tests.cpp -ldwmapi -o build\portable\work_area_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Work area test build failed.'}
    & .\build\portable\work_area_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Work area geometry tests failed.'}
    & $compiler @flags tests\integration\work_area_probe.cpp -ldwmapi -o build\portable\work_area_probe.exe
    if($LASTEXITCODE -ne 0){throw 'Work area probe build failed.'}
    & $compiler @flags -municode src\recovery\guard.cpp -ladvapi32 -lshell32 -o build\portable\MinimaDockGuard.exe
    if($LASTEXITCODE -ne 0){throw 'Guard build failed.'}
    & $compiler @flags -municode -mwindows src\recovery\taskbar_trial.cpp -ladvapi32 -lshell32 -ldwmapi -o build\portable\MinimaDockTrial.exe
    if($LASTEXITCODE -ne 0){throw 'Taskbar trial build failed.'}
    $windres=Join-Path $Toolchain 'bin\llvm-windres.exe'
    & $windres --target=x86_64-w64-mingw32 -I src\app src\app\MinimaDock.rc -O coff -o build\portable\MinimaDock.res.o
    if($LASTEXITCODE -ne 0){throw 'Resource build failed.'}
    & $compiler @flags -municode -mwindows build\portable\MinimaDock.res.o src\app\main.cpp src\windows\quick_report.cpp src\rendering\preview.cpp src\rendering\calendar.cpp src\rendering\glass.cpp src\rendering\previews.cpp src\rendering\jump_list.cpp src\rendering\prayer_toast.cpp src\rendering\customize.cpp src\windows\jump_lists.cpp src\windows\connectivity.cpp src\windows\updates.cpp src\windows\prayer_times.cpp src\windows\app_update.cpp src\windowspp_update.cpp src\windows\launcher_index.cpp src\windows\file_search.cpp src\windows\windows_key.cpp src\windows\volume.cpp src\windows\media.cpp src\windows\tray.cpp src\windows\inventory.cpp src\taskbar\state_machine.cpp -ld2d1 -ldwrite -ldwmapi -ld3d11 -lcoremessaging -ld3d11 -lshell32 -lole32 -loleaut32 -ladvapi32 -lwindowscodecs -luuid -lruntimeobject -lshlwapi -lwlanapi -lbluetoothapis -lsetupapi -lcfgmgr32 -lwinmm -lwinhttp -lpowrprof -lcomdlg32 -o build\portable\MinimaDock.exe
    if($LASTEXITCODE -ne 0){throw 'Preview build failed.'}
    Write-Output 'Native x64 release build complete: build/portable'
} finally { Pop-Location }
