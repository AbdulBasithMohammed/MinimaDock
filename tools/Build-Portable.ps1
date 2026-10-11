param([string]$Toolchain=(Join-Path $PSScriptRoot '..\.tools\llvm-mingw-20261006-ucrt-x86_64'))
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$compiler=Join-Path $Toolchain 'bin\x86_64-w64-mingw32-clang++.exe'
if(-not (Test-Path -LiteralPath $compiler)) { throw 'Portable toolchain missing. Use the MSVC CMake presets or the verified LLVM release described in docs/development.md.' }
Push-Location $repo
try {
    New-Item -ItemType Directory -Path build\portable -Force | Out-Null
    $flags=@('-std=c++20','-Wall','-Wextra','-Wpedantic','-Werror','-O2','-static','-DUNICODE','-D_UNICODE','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-D_WIN32_WINNT=0x0A00','-Isrc')
    & $compiler @flags src\taskbar\state_machine.cpp tests\state_machines\model_tests.cpp -o build\portable\model_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Model build failed.'}
    & .\build\portable\model_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Model tests failed.'}
    & $compiler @flags tests\integration\pin_state_tests.cpp src\windows\inventory.cpp src\taskbar\state_machine.cpp -ld2d1 -ldwrite -ldwmapi -lshell32 -lole32 -ladvapi32 -lwindowscodecs -luuid -lgdi32 -o build\portable\pin_state_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Pinned-window regression build failed.'}
    & .\build\portable\pin_state_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Pinned-window regression failed.'}
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
    & $compiler @flags -municode -mwindows src\app\main.cpp src\rendering\preview.cpp src\windows\inventory.cpp src\taskbar\state_machine.cpp -ld2d1 -ldwrite -ldwmapi -lshell32 -lole32 -ladvapi32 -lwindowscodecs -luuid -o build\portable\MinimaDock.exe
    if($LASTEXITCODE -ne 0){throw 'Preview build failed.'}
    Write-Output 'Native x64 release build complete: build/portable'
} finally { Pop-Location }
