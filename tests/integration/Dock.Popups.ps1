param([string]$Executable=(Join-Path $PSScriptRoot '..\..\build\portable\MinimaDock.exe'))
$ErrorActionPreference='Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'Desktop.Fixture.cs')
$process=$null;$window=[IntPtr]::Zero;$previous=[DesktopActionCheck]::GetForegroundWindow()
$cursor=New-Object DesktopActionCheck+Point;[void][DesktopActionCheck]::GetCursorPos([ref]$cursor)
function Read([int]$Offset){[DesktopActionCheck]::Send($window,0x8028+$Offset,0,0)}
function Await([scriptblock]$Check,[string]$Label){for($i=0;$i -lt 100;$i++){if(& $Check){return};Start-Sleep -Milliseconds 30};throw $Label}
function Bounds {$r=New-Object DesktopActionCheck+Rect;[void][DesktopActionCheck]::GetWindowRect($window,[ref]$r);return $r}
function ClickPanel([bool]$Launcher) {
    $r=Bounds;$scale=[DesktopActionCheck]::GetDpiForWindow($window)/96.0
    $x=(Read 46)*$scale/1000
    if(-not $Launcher){$x=((Read 47)/1000+(Read 14)/1000-166-14+63)*$scale}
    $y=(Read 7)*$scale/1000
    $point=([int][Math]::Round($y) -shl 16) -bor ([int][Math]::Round($x) -band 65535)
    [void][DesktopActionCheck]::Send($window,0x202,0,$point)
}
try {
    $fixture=[DesktopActionCheck]::Fixture()
    for($attempt=0;$attempt -lt 3 -and [DesktopActionCheck]::GetForegroundWindow() -ne $fixture;$attempt++){[void][DesktopActionCheck]::ActivateOwnFixture($fixture);Start-Sleep -Milliseconds 100}
    if([DesktopActionCheck]::GetForegroundWindow() -ne $fixture){throw 'Could not activate blank popup fixture.'}
    $process=Start-Process -FilePath ([IO.Path]::GetFullPath($Executable)) -ArgumentList '--dock' -WindowStyle Hidden -PassThru
    Await {$script:window=[DesktopActionCheck]::Find($process.Id);$window -ne [IntPtr]::Zero -and (Read 0) -eq 1} 'Dock not ready.'
    [void][DesktopActionCheck]::Send($window,0x100,9,0)
    Await {(Read 1) -eq 2 -and (Read 9) -eq 1000} 'Keyboard reveal did not settle.'
    Start-Sleep -Milliseconds 300
    $baseline=Bounds;$anchor=Read 16;$allocations=Read 17;$samples=@();$commits=0
    for($i=0;$i -lt 16;$i++) {
        foreach($launcher in @($true,$false)) {
            foreach($opening in @($true,$false)) {
                $frames=Read 18;$timer=[Diagnostics.Stopwatch]::StartNew()
                ClickPanel $launcher
                $timer.Stop();$samples+=$timer.Elapsed.TotalMilliseconds
                $panel=if($opening){if($launcher){1}else{2}}else{0}
                if((Read 19) -ne $panel){throw 'Actual launcher/volume click did not toggle the intended popup.'}
                if((Read 18) -le $frames){throw 'Popup bounds changed before a new native frame was presented.'}
                if(-not $opening){Await {(Read 49) -eq 0} 'Closing panel did not finish its exit animation.'}
                $r=Bounds;$expectedHeight=if($opening){440}else{88}
                $scale=[DesktopActionCheck]::GetDpiForWindow($window)/96.0
                if([Math]::Abs(($r.Bottom-$r.Top)/$scale-$expectedHeight) -gt 1 -or $r.Bottom -ne $baseline.Bottom -or $r.Left -ne $baseline.Left){throw 'Popup moved the taskbar anchor or changed its horizontal position.'}
                if((Read 16) -ne $anchor -or (Read 5) -ne 44000 -or (Read 9) -ne 1000){throw 'Presented taskbar shifted, grew vertically, or faded during popup toggle.'}
                if((Read 17) -ne $allocations){throw 'Repeated popup toggle rebuilt rendering/icon resources.'}
                $commits++
            }
        }
    }
    $sorted=@($samples | Sort-Object)
    'Popup stress passed: '+$commits+' actual launcher/volume click transitions, fresh committed frames, stable pill position/scale/opacity, retained rendering resources. Click-to-commit median '+[Math]::Round($sorted[$sorted.Count/2],2)+' ms; p95 '+[Math]::Round($sorted[[int][Math]::Floor($sorted.Count*.95)],2)+' ms (component check).'
    [void][DesktopActionCheck]::Send($window,0x10,0,0)
    if(-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0){throw 'Dock did not close cleanly.'}
} finally {
    [DesktopActionCheck]::CloseFixture()
    if($process){$process.Refresh();if(-not $process.HasExited){Stop-Process -Id $process.Id -Force};$process.Dispose()}
    [void][DesktopActionCheck]::SetForegroundWindow($previous)
    [void][DesktopActionCheck]::SetCursorPos($cursor.X,$cursor.Y)
}
