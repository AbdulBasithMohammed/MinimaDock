param([string]$Executable=(Join-Path $PSScriptRoot '..\..\build\portable\MinimaDock.exe'))
$ErrorActionPreference='Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'Desktop.Fixture.cs')
$process=$null;$window=[IntPtr]::Zero;$previous=[DesktopActionCheck]::GetForegroundWindow()
$cursor=New-Object DesktopActionCheck+Point;[void][DesktopActionCheck]::GetCursorPos([ref]$cursor)
function Read([int]$Offset){[DesktopActionCheck]::Send($window,0x8028+$Offset,0,0)}
function Await([scriptblock]$Check,[string]$Label){for($i=0;$i -lt 100;$i++){if(& $Check){return};Start-Sleep -Milliseconds 20};throw $Label}
try {
 $fixture=[DesktopActionCheck]::Fixture()
 for($attempt=0;$attempt -lt 3 -and [DesktopActionCheck]::GetForegroundWindow() -ne $fixture;$attempt++){[void][DesktopActionCheck]::ActivateOwnFixture($fixture);Start-Sleep -Milliseconds 100}
 if([DesktopActionCheck]::GetForegroundWindow() -ne $fixture){throw 'Could not activate blank animation fixture.'}
 $process=Start-Process -FilePath ([IO.Path]::GetFullPath($Executable)) -ArgumentList '--dock' -WindowStyle Hidden -PassThru
 Await {$script:window=[DesktopActionCheck]::Find($process.Id);$window -ne [IntPtr]::Zero -and (Read 0) -eq 1} 'Dock not ready.'
 [void][DesktopActionCheck]::Send($window,0x100,9,0)
 Await {(Read 1) -eq 2 -and (Read 9) -eq 1000} 'Keyboard reveal did not settle.'
 $r=New-Object DesktopActionCheck+Rect;[void][DesktopActionCheck]::GetWindowRect($window,[ref]$r)
 $scale=[DesktopActionCheck]::GetDpiForWindow($window)/96.0
 $x=[int][Math]::Round((Read 46)*$scale/1000);$y=[int][Math]::Round((Read 7)*$scale/1000)
 $point=($y -shl 16) -bor ($x -band 65535)
 $allocations=Read 17;$anchor=Read 16
 [void][DesktopActionCheck]::Send($window,0x201,1,$point)
 if((Read 23) -ne 1){throw 'Launcher press feedback not armed.'}
 [void][DesktopActionCheck]::Send($window,0x202,0,$point)
 if((Read 23) -ne 0 -or (Read 19) -ne 1){throw 'Release did not open launcher immediately.'}
 if((Read 24) -eq 0) {
  if((Read 20) -lt 1 -or (Read 21) -ge 1000 -or (Read 22) -le 0 -or (Read 11) -ne 1){throw 'Finite click/panel animations did not start.'}
  Start-Sleep -Milliseconds 75
  $opacity=Read 21
  if($opacity -le 0 -or $opacity -ge 1000){throw 'Panel did not present an intermediate fade frame.'}
 }
 Await {(Read 20) -eq 0 -and (Read 21) -eq 1000 -and (Read 22) -eq 0 -and (Read 11) -eq 0} 'Feedback did not settle and stop its frame timer.'
 if((Read 17) -ne $allocations -or (Read 16) -ne $anchor -or (Read 5) -ne 44000){throw 'Feedback changed render allocation, anchor or pill height.'}
 # Logical input closes immediately while the rendered flyout animates out.
 [void][DesktopActionCheck]::Send($window,0x100,27,0)
 if((Read 19) -ne 0){throw 'Closing did not stop panel input immediately.'}
 if((Read 24) -eq 0){
  Start-Sleep -Milliseconds 45
  if((Read 49) -ne 1 -or (Read 21) -le 0 -or (Read 21) -ge 1000 -or (Read 50) -ge 1000){throw 'Closing flyout did not fade and contract before removal.'}
 }
 Await {(Read 49) -eq 0 -and (Read 11) -eq 0} 'Close did not finish and release its animation.'
 # Restart/reverse multiple reveal animations without leaving stale input.
 for($i=0;$i -lt 12;$i++) {
  [void][DesktopActionCheck]::Send($window,0x100,13,0)
  if((Read 19) -ne 1){throw 'Rapid keyboard activation failed.'}
  [void][DesktopActionCheck]::Send($window,0x100,27,0)
  if((Read 19) -ne 0){throw 'Closing a reveal left stale panel input.'}
 }
 Await {(Read 20) -eq 0 -and (Read 11) -eq 0} 'Rapid activation left idle animations running.'
 if((Read 17) -ne $allocations -or (Read 16) -ne $anchor){throw 'Rapid activation rebuilt or moved the pill.'}
 'Feedback passed: real launcher press/release, immediate action, intermediate open and close fade/slide/scale, settled timer, 12 rapid keyboard open/reverse cycles, fixed scale/anchor and retained resources.'
 [void][DesktopActionCheck]::Send($window,0x10,0,0)
 if(-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0){throw 'Dock did not close cleanly.'}
} finally {
 [DesktopActionCheck]::CloseFixture()
 if($process){$process.Refresh();if(-not $process.HasExited){Stop-Process -Id $process.Id -Force};$process.Dispose()}
 [void][DesktopActionCheck]::SetForegroundWindow($previous)
 [void][DesktopActionCheck]::SetCursorPos($cursor.X,$cursor.Y)
}
