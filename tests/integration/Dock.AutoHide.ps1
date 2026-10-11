param([string]$Executable=(Join-Path $PSScriptRoot '..\..\build\portable\MinimaDock.exe'))
$ErrorActionPreference='Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'Desktop.Fixture.cs')
$fixture=[IntPtr]::Zero;$process=$null;$window=[IntPtr]::Zero
$previous=[DesktopActionCheck]::GetForegroundWindow();$cursor=New-Object DesktopActionCheck+Point
[void][DesktopActionCheck]::GetCursorPos([ref]$cursor)
function Read([int]$Offset){[DesktopActionCheck]::Send($window,0x8028+$Offset,0,0)}
function Await([scriptblock]$Check,[string]$Label){for($i=0;$i -lt 100;$i++){if(& $Check){return};Start-Sleep -Milliseconds 30};throw $Label}
function EdgeBounds {
    $bounds=New-Object DesktopActionCheck+Rect;[void][DesktopActionCheck]::GetWindowRect($edge,[ref]$bounds)
    $scale=[DesktopActionCheck]::GetDpiForWindow($window)/96.0
    if([Math]::Abs(($bounds.Right-$bounds.Left)-(Read 14)*$scale/1000) -gt 1){throw 'Reveal area does not match the current pill width.'}
    $dockBounds=New-Object DesktopActionCheck+Rect;[void][DesktopActionCheck]::GetWindowRect($window,[ref]$dockBounds)
    if([Math]::Abs(($bounds.Left+$bounds.Right)-($dockBounds.Left+$dockBounds.Right)) -gt 1 -or $bounds.Bottom -ne $dockBounds.Bottom){throw 'Reveal area is not aligned with the pill and screen bottom.'}
    return $bounds
}
try {
    $fixture=[DesktopActionCheck]::Fixture()
    [void][DesktopActionCheck]::ShowWindow($fixture,3)
    [void][DesktopActionCheck]::ActivateOwnFixture($fixture)
    if([DesktopActionCheck]::GetForegroundWindow() -ne $fixture){throw 'Own maximized test fixture could not become foreground.'}
    $process=Start-Process -FilePath ([IO.Path]::GetFullPath($Executable)) -ArgumentList '--dock' -WindowStyle Hidden -PassThru
    Await {$script:window=[DesktopActionCheck]::Find($process.Id);$window -ne [IntPtr]::Zero -and (Read 0) -eq 1} 'Dock not ready.'
    Await {(Read 10) -eq 1 -and (Read 9) -eq 0} 'Maximized application did not fully hide the pill.'
    if((Read 11) -ne 0 -or ([DesktopActionCheck]::GetWindowLongPtr($window,-20).ToInt64() -band 32) -eq 0){throw 'Hidden pill animates or intercepts application input.'}
    $edge=[DesktopActionCheck]::FindClass($process.Id,'MinimaDock.RevealEdge')
    if($edge -eq [IntPtr]::Zero -or -not [DesktopActionCheck]::IsWindowVisible($edge)){throw 'Bottom reveal edge is missing.'}
    $r=EdgeBounds
    if($r.Bottom-$r.Top -ne [Math]::Max(2,[Math]::Round(3*[DesktopActionCheck]::GetDpiForWindow($window)/96.0))){throw 'Hidden indicator is not three DIP tall.'}
    if((Read 15) -ne 180){throw 'Hidden indicator is not visible.'}
    # Both ends must reveal; a center-only sensor would fail these checks.
    [DesktopActionCheck]::MoveToOwnDock($edge,8,1)
    Await {(Read 12) -eq 1} 'Edge did not request immediate reveal.'
    Await {(Read 9) -eq 1000 -and (Read 11) -eq 0} 'Reveal did not settle.'
    $null=EdgeBounds
    if((Read 15) -ne 0){throw 'Indicator did not fade away after reveal.'}
    if([DesktopActionCheck]::GetForegroundWindow() -ne $fixture){throw 'Pointer reveal stole app focus.'}
    [DesktopActionCheck]::MoveToOwnDock($window,[int](([DesktopActionCheck]::GetDpiForWindow($window)/96.0)*36),1)
    Await {(Read 9) -eq 0 -and (Read 11) -eq 0} 'Leaving the pill did not fully hide and settle.'
    $r=EdgeBounds
    [DesktopActionCheck]::MoveToOwnDock($edge,$r.Right-$r.Left-8,1)
    Await {(Read 12) -eq 1} 'Right end did not request immediate reveal.'
    Await {(Read 9) -eq 1000 -and (Read 11) -eq 0} 'Right-end reveal did not settle.'
    [DesktopActionCheck]::MoveToOwnDock($window,[int](([DesktopActionCheck]::GetDpiForWindow($window)/96.0)*36),1)
    Await {(Read 9) -eq 0 -and (Read 11) -eq 0} 'Leaving right-end reveal did not hide.'
    [void][DesktopActionCheck]::Send($window,0x100,9,0) # Own keyboard hold.
    Await {(Read 9) -eq 1000} 'Keyboard did not reveal.'
    [void][DesktopActionCheck]::Send($window,0x100,37,0);[void][DesktopActionCheck]::Send($window,0x100,37,0)
    [void][DesktopActionCheck]::Send($window,0x100,13,0) # Quick settings flyout.
    [void][DesktopActionCheck]::Send($window,0x281,0,0) # Mouse leaves own dock.
    Start-Sleep -Milliseconds 350
    if((Read 9) -ne 1000){throw 'Open flyout did not hold visibility.'}
    [void][DesktopActionCheck]::Send($window,0x100,27,0);[void][DesktopActionCheck]::Send($window,0x100,27,0)
    Await {(Read 9) -eq 0} 'Closing flyout did not restore auto-hide.'
    [void][DesktopActionCheck]::ShowWindow($fixture,1)
    Await {(Read 10) -eq 0 -and (Read 9) -eq 1000 -and (Read 11) -eq 0} 'Restoring the app did not reveal the dock.'
    [void][DesktopActionCheck]::ShowWindow($fixture,3)
    Await {(Read 10) -eq 1 -and (Read 9) -eq 0} 'Maximizing again did not hide the dock.'
    if((Read 13) -ne 1){throw 'Reveal shortcut registration failed.'}
    [void][DesktopActionCheck]::Send($window,0x312,3,0)
    Await {(Read 9) -eq 1000} 'Reveal shortcut handler did not reveal.'
    [void][DesktopActionCheck]::Send($window,0x100,27,0)
    Await {(Read 9) -eq 0} 'Escape did not release shortcut visibility hold.'
    if([DesktopActionCheck]::GetForegroundWindow() -ne $fixture){throw 'Escape did not return focus to the previous application.'}
    [DesktopActionCheck]::FullscreenOwnFixture($fixture)
    Await {(Read 1) -eq 4 -and (Read 9) -eq 0} 'Real fullscreen fixture did not suppress the dock.'
    if([DesktopActionCheck]::IsWindowVisible($edge)){throw 'Fullscreen still exposes reveal edge.'}
    [void][DesktopActionCheck]::Send($window,0x312,3,0)
    if((Read 12) -ne 0 -or (Read 11) -ne 0){throw 'Fullscreen shortcut bypassed suppression.'}
    [void][DesktopActionCheck]::Send($window,0x10,0,0)
    if(-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0){throw 'Dock did not close cleanly.'}
    'Auto-hide passed: pill-width indicator, real reveal at both ends, indicator fade, maximize/restore, input pass-through, focus preservation/return, finite slide/fade, flyout holds, keyboard reveal, fullscreen suppression, cleanup.'
} finally {
    if($process){$process.Refresh();if(-not $process.HasExited){Stop-Process -Id $process.Id -Force};$process.Dispose()}
    [DesktopActionCheck]::CloseFixture()
    [void][DesktopActionCheck]::SetForegroundWindow($previous)
    [void][DesktopActionCheck]::SetCursorPos($cursor.X,$cursor.Y)
}
