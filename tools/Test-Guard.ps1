param([string]$Toolchain=(Join-Path $PSScriptRoot '..\.tools\llvm-mingw-20261006-ucrt-x86_64'))
$ErrorActionPreference='Stop'
$compiler=Join-Path $Toolchain 'bin\x86_64-w64-mingw32-clang++.exe'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $repo
try {
    $flags=@('-std=c++20','-Wall','-Wextra','-Wpedantic','-Werror','-O2','-static','-municode','-DUNICODE','-D_UNICODE','-DNOMINMAX')
    & $compiler @flags -DMINIMA_GUARD_TEST src\recovery\guard.cpp -ladvapi32 -lshell32 -o build\portable\MinimaDockGuardTest.exe
    if($LASTEXITCODE -ne 0){throw 'Guard test build failed.'}
    & $compiler @flags tests\integration\guard_fixture.cpp -o build\portable\MinimaDockGuardFixture.exe
    if($LASTEXITCODE -ne 0){throw 'Guard fixture build failed.'}
    & $compiler @flags tests\integration\guard_tests.cpp -o build\portable\guard_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Guard harness build failed.'}
    & .\build\portable\guard_tests.exe
    if($LASTEXITCODE -ne 0){throw 'Native guard tests failed.'}
} finally { Pop-Location }
