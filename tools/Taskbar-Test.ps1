param([ValidateSet('Deploy','Revert','Keep','Status')][string]$Action='Deploy',[ValidateRange(10,3600)][int]$Seconds=600)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out=Join-Path $repo 'out'
$deployRoot=Join-Path $out 'deployments'
$activeFile=Join-Path $out 'active-taskbar-test.json'
function Get-Deployment {
    if(-not (Test-Path -LiteralPath $activeFile)){return $null}
    $state=Get-Content -LiteralPath $activeFile -Raw | ConvertFrom-Json
    if($state.Format -cne 'MinimaDockTaskbarTest/1'){throw 'Invalid deployment record.'}
    $directory=[IO.Path]::GetFullPath($state.Directory)
    if(-not $directory.StartsWith($deployRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Deployment record is outside the repository deployment folder.'}
    $guard=Join-Path $directory 'MinimaDockTrial.exe'
    if(-not (Test-Path -LiteralPath $guard) -or (Get-FileHash -LiteralPath $guard -Algorithm SHA256).Hash -cne $state.GuardSha256){throw 'Recovery supervisor is missing or has changed. Keep the deployment folder intact.'}
    [pscustomobject]@{Directory=$directory;Guard=$guard}
}
function Invoke-Decision($Deployment,[string]$Decision) {
    if($null -eq $Deployment){if($Decision -eq 'revert'){return 0};throw 'No taskbar test deployment exists.'}
    $command=Start-Process -FilePath $Deployment.Guard -ArgumentList ('--'+$Decision) -WorkingDirectory $Deployment.Directory -WindowStyle Hidden -PassThru
    try {if(-not $command.WaitForExit(12000)){throw 'Taskbar recovery command has not completed. Use Ctrl+Alt+Backspace or Task Manager to run Revert-Taskbar.cmd.'};$command.ExitCode}finally{$command.Dispose()}
}
if($Action -eq 'Revert') {
    $code=Invoke-Decision (Get-Deployment) 'revert'
    if($code -ne 0){throw ('Taskbar revert reported exit code '+$code)}
    'Taskbar test reverted. Original Explorer taskbar visibility and work area restored.';exit 0
}
if($Action -eq 'Keep') {
    $code=Invoke-Decision (Get-Deployment) 'keep'
    if($code -ne 0){throw 'No responsive taskbar test is available to keep.'}
    'Taskbar test kept for this session. Recovery guard remains active; no automatic startup enabled.';exit 0
}
if($Action -eq 'Status') {
    $code=Invoke-Decision (Get-Deployment) 'status'
    switch($code){1 {'Trial active; automatic revert armed.'} 2 {'Kept test active; recovery guard armed.'} 3 {'No active taskbar test.'} default {throw ('Unexpected test status '+$code)}}
    exit 0
}
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=New-Object Security.Principal.WindowsPrincipal($identity)
if($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Deploy as your normal unelevated desktop user.'}
# Baseline verification is read-only. Temporary visibility recovery uses its own journal.
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'recovery-bundle\Restore.ps1') -VerifyOnly
if($LASTEXITCODE -ne 0){throw 'Original shell recovery baseline could not be verified.'}
$previous=Get-Deployment
$previousRecord=if(Test-Path -LiteralPath $activeFile){Get-Content -LiteralPath $activeFile -Raw}else{$null}
$source=$out
foreach($name in @('MinimaDock.exe','MinimaDockTrial.exe')){if(-not(Test-Path -LiteralPath (Join-Path $source $name))){throw ('Build missing: '+$name)}}
$directory=Join-Path $deployRoot ((Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $directory -Force | Out-Null
foreach($name in @('MinimaDock.exe','MinimaDockTrial.exe')){
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $directory $name)
    if((Get-FileHash -LiteralPath (Join-Path $source $name)).Hash -cne (Get-FileHash -LiteralPath (Join-Path $directory $name)).Hash){throw 'Deployment copy verification failed.'}
}
$guard=Join-Path $directory 'MinimaDockTrial.exe'
# Probe this exact copied executable before reverting the previous dock or
# publishing a new recovery pointer. A policy block cannot strand an old trial.
. (Join-Path $PSScriptRoot 'Dock-LaunchCheck.ps1')
Assert-DockLaunchable (Join-Path $directory 'MinimaDock.exe')
if($previous){$code=Invoke-Decision $previous 'revert';if($code -ne 0){throw 'Previous deployment could not be reverted; new deployment cancelled.'}}
$record=[ordered]@{Format='MinimaDockTaskbarTest/1';Directory=$directory;GuardSha256=(Get-FileHash -LiteralPath $guard).Hash;DockSha256=(Get-FileHash -LiteralPath (Join-Path $directory 'MinimaDock.exe')).Hash;Created=(Get-Date).ToString('o');TrialSeconds=$Seconds}
$temp=$activeFile+'.tmp'
[IO.File]::WriteAllText($temp,($record|ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
Move-Item -LiteralPath $temp -Destination $activeFile -Force
$process=$null
try {
    $process=Start-Process -FilePath $guard -ArgumentList @('--trial',"$Seconds") -WorkingDirectory $directory -WindowStyle Hidden -PassThru
    $ready=$false
    for($i=0;$i -lt 40;$i++) {
        Start-Sleep -Milliseconds 500
        $process.Refresh();if($process.HasExited){throw ('Taskbar test exited during startup: '+$process.ExitCode)}
        $code=Invoke-Decision (Get-Deployment) 'status'
        if($code -eq 1 -or $code -eq 2){$ready=$true;break}
    }
    if(-not $ready){throw 'Taskbar test did not report readiness.'}
    'Temporary taskbar test deployed. Explorer remains running. Ctrl+Alt+Backspace reverts; Ctrl+Alt+K keeps this session.'
    'Automatic revert in '+$Seconds+' seconds unless kept. Revert launcher: '+(Join-Path $out 'Revert-Taskbar.cmd')
} catch {
    $startupError=$_
    if($null -eq $process) {
        # No supervisor started: it could not hide any taskbar. Restore the prior
        # pointer so independent Revert still reaches the last usable supervisor.
        if($null -ne $previousRecord) {
            [IO.File]::WriteAllText($temp,$previousRecord,(New-Object Text.UTF8Encoding($false)))
            Move-Item -LiteralPath $temp -Destination $activeFile -Force
        } else {Remove-Item -LiteralPath $activeFile -Force}
    } else {
        $revertCode=Invoke-Decision (Get-Deployment) 'revert'
        if($revertCode -ne 0){throw 'Startup failed and recovery needs attention; retained the new deployment journal. Run Revert-Taskbar.cmd.'}
    }
    throw $startupError
} finally {if($null -ne $process){$process.Dispose()}}
