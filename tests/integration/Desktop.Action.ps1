param([string]$Executable=(Join-Path $PSScriptRoot '..\..\build\portable\MinimaDock.exe'),[switch]$Framed)
$ErrorActionPreference='Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'Desktop.Fixture.cs')
$fixture=[IntPtr]::Zero;$window=[IntPtr]::Zero;$process=$null;$desktopToggled=$false;$previousForeground=[DesktopActionCheck]::GetForegroundWindow()
try {
    $fixture=[DesktopActionCheck]::Fixture()
    [void][DesktopActionCheck]::ShowWindow($fixture,3)
    if(-not $Framed -and -not [DesktopActionCheck]::ActivateOwnFixture($fixture)){throw 'Could not activate the owned blank fixture for the desktop button test.'}
    if(-not [DesktopActionCheck]::IsWindowVisible($fixture) -or [DesktopActionCheck]::IsIconic($fixture)){throw 'Own test window is not initially visible.'}
    'Fixture before: '+[DesktopActionCheck]::State($fixture)
    $previewArgument=if($Framed){'--preview'}else{'--dock'}
    $previewClass=if($Framed){'MinimaDock.Preview'}else{'MinimaDock.DesktopDock'}
    $process=Start-Process -FilePath ([IO.Path]::GetFullPath($Executable)) -ArgumentList $previewArgument -WindowStyle Hidden -PassThru
    $ready=$false;$window=[IntPtr]::Zero
    for($i=0;$i -lt 150;$i++) {
        $process.Refresh();if($process.HasExited){throw 'Dock exited at startup.'}
        $window=[DesktopActionCheck]::FindClass($process.Id,$previewClass)
        if($window -ne [IntPtr]::Zero -and ($Framed -or [DesktopActionCheck]::Send($window,0x8028,0,0) -eq 1)){$ready=$true;break}
        Start-Sleep -Milliseconds 100
    }
    if(-not $ready){throw 'Own dock did not become ready.'}
    if([DesktopActionCheck]::Send($window,0x8029,0,0) -eq 4){throw 'A fullscreen foreground app suppresses the dock. Run the button integration check outside fullscreen; native desktop fixture tests are independent.'}
    [void][DesktopActionCheck]::Send($window,0x100,9,0) # Reveal for keyboard access above the maximized fixture.
    Start-Sleep -Milliseconds 250 # Let hover animation and inventory reconciliation settle before sampling the moving sliver.
    [DesktopActionCheck]::ClickDesktop($window)
    $desktopToggled=$true
    'Native desktop action completed through the sliver hit target.'
    'Fixture after first click: '+[DesktopActionCheck]::State($fixture)
    $shown=$false
    for($i=0;$i -lt 40;$i++){if([DesktopActionCheck]::Hidden($fixture)){$shown=$true;break};Start-Sleep -Milliseconds 50}
    if(-not $shown){throw ('Show desktop did not hide the visible test window: '+[DesktopActionCheck]::State($fixture))}
    $desktopFocused=$false
    for($i=0;$i -lt 40;$i++){if([DesktopActionCheck]::State($fixture) -match 'foreground=(Progman|WorkerW)$'){$desktopFocused=$true;break};Start-Sleep -Milliseconds 50}
    if(-not $desktopFocused){throw ('Show desktop stopped on another application: '+[DesktopActionCheck]::State($fixture))}
    Start-Sleep -Milliseconds 250
    [DesktopActionCheck]::ClickDesktop($window)
    $desktopToggled=$false
    $restored=$false
    for($i=0;$i -lt 40;$i++){if(-not [DesktopActionCheck]::Hidden($fixture)){$restored=$true;break};Start-Sleep -Milliseconds 50}
    if(-not $restored){throw 'Second desktop-button click did not restore the test window.'}
    [void][DesktopActionCheck]::Send($window,0x10,0,0)
    if(-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0){throw 'Own dock did not close cleanly.'}
    'Desktop button passed: real sliver hit target, visible fixture hidden on first click and restored on second click, clean shutdown.'
} finally {
    # Always pair a successful first toggle, including assertion failures.
    try {
        if($desktopToggled) {
            Start-Sleep -Milliseconds 250
            [DesktopActionCheck]::ClickDesktop($window)
        }
    } finally {
        if($null -ne $process){$process.Refresh();if(-not $process.HasExited){try{[void][DesktopActionCheck]::Send($window,0x10,0,0)}catch{};if(-not $process.WaitForExit(5000)){Stop-Process -Id $process.Id -Force}};$process.Dispose()}
        [DesktopActionCheck]::CloseFixture()
        [void][DesktopActionCheck]::SetForegroundWindow($previousForeground)
    }
}
