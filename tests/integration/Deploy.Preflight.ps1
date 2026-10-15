$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\..\tools\Dock-LaunchCheck.ps1')
$root=Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\build'))) ('preflight-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
$exe=Join-Path $root 'MinimaDock.exe'
[IO.File]::WriteAllText($exe,'fixture')
$script:mode='success';$checks=0
# Only simulate the OS/renderer boundary; exercise the actual error handling and
# temporary-frame cleanup without launching or modifying a Windows shell.
function Invoke-DockRenderProbe([string]$Executable,[string]$Snapshot) {
    if($script:mode -eq 'success'){[IO.File]::WriteAllText($Snapshot,'rendered frame');return 0}
    if($script:mode -eq 'no-frame'){return 0}
    if($script:mode -eq 'render-failure'){[IO.File]::WriteAllText($Snapshot,'partial frame');return 1}
    if($script:mode -eq 'image-status'){return -1073740760}
    if($script:mode -eq 'generic'){throw [ComponentModel.Win32Exception]::new(2)}
    throw [ComponentModel.Win32Exception]::new([int]$script:mode)
}
try {
    Assert-DockLaunchable $exe;$checks++
    foreach($mode in @('577','1260','4551','image-status','generic','no-frame','render-failure')) {
        $script:mode=$mode;$message=$null
        try {Assert-DockLaunchable $exe}catch{$message=$_.Exception.Message}
        if(-not $message){throw ('Expected failure for '+$mode)}
        if($mode -in @('577','1260','4551','image-status') -and $message -notmatch 'Windows Application Control blocked'){throw ('Missing policy explanation for '+$mode)}
        if($message -notmatch '(taskbar was not changed|changing the current taskbar)'){throw ('Missing preservation explanation for '+$mode)}
        if(@(Get-ChildItem -LiteralPath $root -Filter '*.png').Count -ne 0){throw ('Probe frame leaked for '+$mode)}
        $checks++
    }
    'Deploy preflight passed: '+$checks+' success/block/error cases and temporary-frame cleanup.'
} finally {
    # Exact, non-recursive cleanup of only this fixture's known files.
    Get-ChildItem -LiteralPath $root -File | ForEach-Object {Remove-Item -LiteralPath $_.FullName -Force}
    Remove-Item -LiteralPath $root
}
