$ErrorActionPreference='Stop'
Add-Type -Path @((Join-Path $PSScriptRoot 'Desktop.Fixture.cs'),(Join-Path $PSScriptRoot 'Snap.Fixture.cs'))
[void][SnapFixture]::SetThreadDpiAwarenessContext([IntPtr](-4))
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$window=[IntPtr]::Zero;$fixture=[IntPtr]::Zero
$previous=[DesktopActionCheck]::GetForegroundWindow();$cursor=New-Object DesktopActionCheck+Point
[void][DesktopActionCheck]::GetCursorPos([ref]$cursor)
function Action([string]$Name,[int]$Seconds=120){& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools/Taskbar-Test.ps1') -Action $Name -Seconds $Seconds;if($LASTEXITCODE -ne 0){throw ('Trial action failed: '+$Name)}}
function Await([scriptblock]$Check,[string]$Label){for($i=0;$i -lt 100;$i++){if(& $Check){return};Start-Sleep -Milliseconds 30};throw $Label}
function Read([int]$Offset){[DesktopActionCheck]::Send($window,0x8028+$Offset,0,0)}
function Assert-Snap([bool]$Active,[int]$Left,[int]$Right) {
    $expected=if($Active){$area.Monitor.Bottom}else{$area.Work.Bottom}
    Await {[Math]::Abs([SnapFixture]::Frame($fixture).Bottom-$expected) -le 1} 'Snapped pane did not reach the expected work-area bottom.'
    $frame=[SnapFixture]::Frame($fixture)
    if(-not [SnapFixture]::IsWindowArranged($fixture) -or [SnapFixture]::Normal($fixture).ToString() -ne $normal){throw 'Pane lost its native snap state or original restore placement.'}
    if($frame.Left -ne $Left -or $frame.Right -ne $Right -or [Math]::Abs($frame.Top-$area.Work.Top) -gt 1){throw 'Snapped pane changed its width or top anchor.'}
}
try {
    Action Revert
    $fixture=[DesktopActionCheck]::Fixture();$area=[SnapFixture]::Area($fixture);$normal=[SnapFixture]::Normal($fixture).ToString()
    [SnapFixture]::Snap($fixture,$false)
    $left=[SnapFixture]::Frame($fixture);Assert-Snap $false $left.Left $left.Right
    Action Deploy
    Assert-Snap $true $left.Left $left.Right
    $record=Get-Content -LiteralPath (Join-Path $repo 'out/active-taskbar-test.json') -Raw | ConvertFrom-Json
    $dock=Get-Process MinimaDock | Where-Object Path -eq (Join-Path $record.Directory 'MinimaDock.exe')
    $window=[DesktopActionCheck]::Find($dock.Id)
    if(-not [DesktopActionCheck]::ActivateOwnFixture($fixture)){throw 'Cannot foreground own snapped fixture.'}
    Await {(Read 10) -eq 1 -and (Read 9) -eq 0} 'Left snap did not auto-hide the pill.'
    [SnapFixture]::Snap($fixture,$true)
    $right=[SnapFixture]::Frame($fixture);Assert-Snap $true $right.Left $right.Right
    # Reproduce a newly snapped pane using Explorer's stale bottom reservation.
    # Only our own arranged fixture is moved; a location event must repair it.
    $repair=[Diagnostics.Stopwatch]::StartNew()
    [SnapFixture]::ResizeBottom($fixture,$area.Work.Bottom)
    Assert-Snap $true $right.Left $right.Right
    $repair.Stop()
    if($repair.Elapsed.TotalMilliseconds -gt 1000){throw 'Snap repair waited too long for the periodic reconciliation fallback.'}
    'Native snap event repair: '+[Math]::Round($repair.Elapsed.TotalMilliseconds,2)+' ms.'
    if(-not [DesktopActionCheck]::ActivateOwnFixture($fixture)){throw 'Cannot foreground own right snapped fixture.'}
    Await {(Read 10) -eq 1 -and (Read 9) -eq 0} 'Right snap did not auto-hide the pill.'
    Action Revert
    Assert-Snap $false $right.Left $right.Right
    Action Deploy
    Assert-Snap $true $right.Left $right.Right
    $record=Get-Content -LiteralPath (Join-Path $repo 'out/active-taskbar-test.json') -Raw | ConvertFrom-Json
    $guard=Get-Process MinimaDockTrial | Where-Object Path -eq (Join-Path $record.Directory 'MinimaDockTrial.exe')
    if(-not $guard -or (Get-FileHash -LiteralPath $guard.Path).Hash -ne $record.GuardSha256){throw 'Cannot authenticate the trial supervisor for standalone recovery.'}
    Stop-Process -Id $guard.Id -Force
    Action Revert
    Assert-Snap $false $right.Left $right.Right
    Action Deploy 10
    Assert-Snap $true $right.Left $right.Right
    Start-Sleep -Seconds 13
    Assert-Snap $false $right.Left $right.Right
    [SnapFixture]::Restore($fixture)
    Await {-not [SnapFixture]::IsWindowArranged($fixture)} 'Restoring the fixture did not exit native snap state.'
    if([SnapFixture]::Normal($fixture).ToString() -ne $normal){throw 'Unsnapping lost the original restore size.'}
    Action Deploy
    $record=Get-Content -LiteralPath (Join-Path $repo 'out/active-taskbar-test.json') -Raw | ConvertFrom-Json
    $dock=Get-Process MinimaDock | Where-Object Path -eq (Join-Path $record.Directory 'MinimaDock.exe')
    $window=[DesktopActionCheck]::Find($dock.Id)
    if(-not [DesktopActionCheck]::ActivateOwnFixture($fixture)){throw 'Cannot foreground restored fixture.'}
    Await {(Read 10) -eq 0 -and (Read 9) -eq 1000} 'Floating restored window did not keep the pill visible.'
    'Native snap passed: pre-existing left pane and new right pane reach full height without width/state/restore changes; snapped panes auto-hide; Revert, standalone recovery and timeout restore the original bottom; floating restored windows reveal the pill.'
} finally {
    try{Action Revert}finally{[DesktopActionCheck]::CloseFixture();[void][DesktopActionCheck]::SetForegroundWindow($previous);[void][DesktopActionCheck]::SetCursorPos($cursor.X,$cursor.Y)}
}
