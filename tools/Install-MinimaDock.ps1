# Installs MinimaDock for the current user and starts it at every sign-in.
# Explorer keeps running; its taskbar is hidden while MinimaDock runs and comes
# back on Ctrl+Alt+Backspace, Uninstall, or if the dock keeps failing.
#   Install    copy out\ binaries to %LOCALAPPDATA%\Programs\MinimaDock and start them
#   Uninstall  bring back the Windows taskbar, remove sign-in start and files
#   Stop       bring back the Windows taskbar until the next sign-in
#   Start      start the installed MinimaDock again
#   Status     report whether it is running and starts with Windows
param([ValidateSet('Install','Uninstall','Stop','Start','Status')][string]$Action='Install')
$ErrorActionPreference='Stop'
$root=Join-Path $env:LOCALAPPDATA 'Programs\MinimaDock'
$versions=Join-Path $root 'versions'
$recordFile=Join-Path $root 'install.json'
$runKey='HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$uninstallKey='HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\MinimaDock'
$manager=Join-Path $root 'Manage-MinimaDock.ps1'

function Get-Install {
    if(-not (Test-Path -LiteralPath $recordFile)){return $null}
    $state=Get-Content -LiteralPath $recordFile -Raw | ConvertFrom-Json
    if($state.Format -cne 'MinimaDockInstall/1'){throw 'Invalid install record.'}
    $directory=[IO.Path]::GetFullPath($state.Directory)
    if(-not $directory.StartsWith($versions+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Install record points outside the MinimaDock folder.'}
    $guard=Join-Path $directory 'MinimaDockTrial.exe'
    if(-not (Test-Path -LiteralPath $guard) -or (Get-FileHash -LiteralPath $guard -Algorithm SHA256).Hash -cne $state.GuardSha256){throw 'The MinimaDock supervisor is missing or has changed. Keep the install folder intact.'}
    [pscustomobject]@{Directory=$directory;Guard=$guard;Dock=(Join-Path $directory 'MinimaDock.exe')}
}
function Invoke-Supervisor($Install,[string]$Decision) {
    $command=Start-Process -FilePath $Install.Guard -ArgumentList ('--'+$Decision) -WorkingDirectory $Install.Directory -WindowStyle Hidden -PassThru
    try {if(-not $command.WaitForExit(12000)){throw 'MinimaDock did not answer. Press Ctrl+Alt+Backspace to bring back the Windows taskbar.'};$command.ExitCode}finally{$command.Dispose()}
}
function Start-Supervisor($Install) {
    $process=Start-Process -FilePath $Install.Guard -ArgumentList '--run' -WorkingDirectory $Install.Directory -WindowStyle Hidden -PassThru
    try {
        for($i=0;$i -lt 120;$i++) {
            Start-Sleep -Milliseconds 500
            $process.Refresh();if($process.HasExited){throw ('MinimaDock exited during startup: '+$process.ExitCode)}
            if((Invoke-Supervisor $Install 'status') -eq 2){return}
        }
        throw 'MinimaDock did not report readiness.'
    } finally {$process.Dispose()}
}
function Write-Record($Directory) {
    $record=[ordered]@{Format='MinimaDockInstall/1';Directory=$Directory;GuardSha256=(Get-FileHash -LiteralPath (Join-Path $Directory 'MinimaDockTrial.exe')).Hash;DockSha256=(Get-FileHash -LiteralPath (Join-Path $Directory 'MinimaDock.exe')).Hash;Installed=(Get-Date).ToString('o')}
    $temp=$recordFile+'.tmp'
    [IO.File]::WriteAllText($temp,($record|ConvertTo-Json),(New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temp -Destination $recordFile -Force
}
function Set-Registration($Install) {
    if(-not (Test-Path $runKey)){New-Item -Path $runKey -Force | Out-Null}
    Set-ItemProperty -Path $runKey -Name 'MinimaDock' -Value ('"'+$Install.Guard+'" --run')
    $info=(Get-Item -LiteralPath $Install.Dock).VersionInfo
    New-Item -Path $uninstallKey -Force | Out-Null
    $size=[int]((Get-ChildItem -LiteralPath $Install.Directory -File | Measure-Object Length -Sum).Sum/1KB)
    $values=[ordered]@{DisplayName='MinimaDock';DisplayVersion=$info.ProductVersion;Publisher=$info.CompanyName;InstallLocation=$root;DisplayIcon=($Install.Dock+',0')
        UninstallString=('"'+$env:SystemRoot+'\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "'+$manager+'" -Action Uninstall')}
    foreach($name in $values.Keys){Set-ItemProperty -Path $uninstallKey -Name $name -Value $values[$name]}
    foreach($name in 'NoModify','NoRepair'){Set-ItemProperty -Path $uninstallKey -Name $name -Value 1 -Type DWord}
    Set-ItemProperty -Path $uninstallKey -Name 'EstimatedSize' -Value $size -Type DWord
}
# A version folder still holding a recovery journal has not been fully restored; never delete it.
function Remove-OldVersions([string[]]$Keep) {
    if(-not (Test-Path -LiteralPath $versions)){return}
    Get-ChildItem -LiteralPath $versions -Directory | Where-Object {$Keep -notcontains $_.FullName} | ForEach-Object {
        if(Test-Path -LiteralPath (Join-Path $_.FullName 'taskbar-trial.state')){return}
        Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$principal=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Run this as your normal unelevated desktop user.'}

if($Action -eq 'Status') {
    $install=Get-Install
    if(-not $install){'MinimaDock is not installed.';exit 0}
    $code=Invoke-Supervisor $install 'status'
    $starts=$null -ne (Get-ItemProperty -Path $runKey -Name 'MinimaDock' -ErrorAction SilentlyContinue)
    switch($code){2 {'MinimaDock is running.'} 1 {'A timed MinimaDock trial is running.'} 3 {'MinimaDock is not running; the Windows taskbar is shown.'} default {'Unexpected status '+$code}}
    if($starts){'Starts when you sign in.'}else{'Does not start when you sign in.'}
    exit 0
}
if($Action -eq 'Stop') {
    $install=Get-Install;if(-not $install){throw 'MinimaDock is not installed.'}
    if((Invoke-Supervisor $install 'revert') -ne 0){throw 'MinimaDock could not restore the Windows taskbar. Press Ctrl+Alt+Backspace.'}
    'Windows taskbar restored until you sign in again.';exit 0
}
if($Action -eq 'Start') {
    $install=Get-Install;if(-not $install){throw 'MinimaDock is not installed.'}
    if((Invoke-Supervisor $install 'status') -eq 2){'MinimaDock is already running.';exit 0}
    Start-Supervisor $install;'MinimaDock started.';exit 0
}
if($Action -eq 'Uninstall') {
    $install=Get-Install
    if($install -and (Invoke-Supervisor $install 'revert') -ne 0){throw 'MinimaDock could not restore the Windows taskbar, so nothing was removed. Press Ctrl+Alt+Backspace and try again.'}
    Remove-ItemProperty -Path $runKey -Name 'MinimaDock' -ErrorAction SilentlyContinue
    if(Test-Path $uninstallKey){Remove-Item -Path $uninstallKey -Recurse -Force}
    Remove-OldVersions @()
    Remove-Item -LiteralPath $recordFile -Force -ErrorAction SilentlyContinue
    $left=@(if(Test-Path -LiteralPath $versions){Get-ChildItem -LiteralPath $versions -Directory})
    if($left.Count -eq 0) {
        # This script may be running from the folder; remove it once PowerShell exits.
        Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList ('/c ping -n 3 127.0.0.1 >nul & rmdir /s /q "'+$root+'"') -WindowStyle Hidden
    } else {'Kept '+$left[0].FullName+' because it still holds a recovery journal.'}
    'MinimaDock uninstalled. The Windows taskbar is back. Your settings remain under HKCU\Software\MinimaDock.'
    exit 0
}

# ---- Install or update ----
# A release zip keeps the binaries next to this script (app\); the repository uses out\.
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$source=if(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'MinimaDock.exe')){$PSScriptRoot}else{Join-Path $repo 'out'}
foreach($name in 'MinimaDock.exe','MinimaDockTrial.exe'){if(-not (Test-Path -LiteralPath (Join-Path $source $name))){throw ('MinimaDock files are missing ('+$name+'). Unzip the whole download and run "Install MinimaDock.cmd" again.')}}
if([Environment]::OSVersion.Version.Build -lt 22000){throw 'MinimaDock needs Windows 11.'}
if(-not [Environment]::Is64BitOperatingSystem){throw 'MinimaDock needs 64-bit Windows.'}
$bundle=Join-Path $repo 'recovery-bundle\Restore.ps1'
if(Test-Path -LiteralPath $bundle) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $bundle -VerifyOnly
    if($LASTEXITCODE -ne 0){throw 'Original shell recovery baseline could not be verified.'}
}
$previous=Get-Install
$hash=(Get-FileHash -LiteralPath (Join-Path $source 'MinimaDock.exe')).Hash.Substring(0,8).ToLowerInvariant()
$directory=Join-Path $versions ((Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+$hash)
New-Item -ItemType Directory -Path $directory -Force | Out-Null
foreach($name in 'MinimaDock.exe','MinimaDockTrial.exe') {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $directory $name)
    if((Get-FileHash -LiteralPath (Join-Path $source $name)).Hash -cne (Get-FileHash -LiteralPath (Join-Path $directory $name)).Hash){throw 'Install copy verification failed.'}
}
foreach($notice in 'LICENSE','THIRD_PARTY_NOTICES.md'){foreach($folder in $source,$repo){$file=Join-Path $folder $notice;if(Test-Path -LiteralPath $file){Copy-Item -LiteralPath $file -Destination $directory;break}}}
# Probe the exact installed executable before touching the running dock.
. (Join-Path $PSScriptRoot 'Dock-LaunchCheck.ps1')
Assert-DockLaunchable (Join-Path $directory 'MinimaDock.exe')
# Retire a running temporary trial and the previous installed version.
$testScript=Join-Path $PSScriptRoot 'Taskbar-Test.ps1'
if((Test-Path -LiteralPath (Join-Path $source 'active-taskbar-test.json')) -and (Test-Path -LiteralPath $testScript)) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $testScript -Action Revert
    if($LASTEXITCODE -ne 0){throw 'The running taskbar trial could not be reverted; install cancelled.'}
    Remove-Item -LiteralPath (Join-Path $source 'active-taskbar-test.json') -Force
}
if($previous -and (Invoke-Supervisor $previous 'revert') -ne 0){throw 'The installed MinimaDock could not be stopped; update cancelled.'}
if($PSCommandPath -ne $manager){Copy-Item -LiteralPath $PSCommandPath -Destination $manager -Force;Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Dock-LaunchCheck.ps1') -Destination (Join-Path $root 'Dock-LaunchCheck.ps1') -Force}
Write-Record $directory
$install=Get-Install
try {
    Start-Supervisor $install
} catch {
    $failure=$_
    Invoke-Supervisor $install 'revert' | Out-Null
    if($previous) {
        Write-Record $previous.Directory
        try {Start-Supervisor (Get-Install)} catch {}
    } else {Remove-Item -LiteralPath $recordFile -Force}
    throw $failure
}
Set-Registration $install
Remove-OldVersions @($install.Directory)
'MinimaDock '+((Get-Item -LiteralPath $install.Dock).VersionInfo.ProductVersion -replace '\.0$','')+' is installed and running. It starts each time you sign in.'
'Ctrl+Alt+Backspace brings back the Windows taskbar until you sign in again. To remove it, run "Uninstall MinimaDock" or use Settings > Apps.'
