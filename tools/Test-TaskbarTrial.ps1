param([string]$Toolchain=(Join-Path $PSScriptRoot '..\.tools\llvm-mingw-20261006-ucrt-x86_64'))
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$compiler=Join-Path $Toolchain 'bin\x86_64-w64-mingw32-clang++.exe'
Push-Location $repo
try {
    $flags=@('-std=c++20','-Wall','-Wextra','-Wpedantic','-Werror','-O2','-static','-DUNICODE','-D_UNICODE','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-D_WIN32_WINNT=0x0A00','-Isrc')
    & $compiler @flags -municode -mwindows -DMINIMA_TRIAL_TEST src\recovery\taskbar_trial.cpp -ladvapi32 -lshell32 -ldwmapi -o build\portable\MinimaDockTrialTest.exe
    if($LASTEXITCODE -ne 0){throw 'Trial fixture build failed.'}
    & $compiler @flags -municode tests\integration\trial_tests.cpp -o build\portable\TrialTests.exe
    if($LASTEXITCODE -ne 0){throw 'Trial harness build failed.'}
    foreach($scenario in @('revert','crash','hang','timeout','keep','fallback')) {
        & .\build\portable\TrialTests.exe $scenario
        if($LASTEXITCODE -ne 0){throw ('Taskbar trial test failed: '+$scenario)}
    }
} finally {Pop-Location}
