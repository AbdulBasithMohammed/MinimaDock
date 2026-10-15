param([string]$Executable=(Join-Path $PSScriptRoot '..\..\build\portable\MinimaDock.exe'))
$ErrorActionPreference='Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'Desktop.Fixture.cs')
$process=$null;$window=[IntPtr]::Zero;$calendar=[IntPtr]::Zero
$previous=[DesktopActionCheck]::GetForegroundWindow()
$cursor=New-Object DesktopActionCheck+Point
[void][DesktopActionCheck]::GetCursorPos([ref]$cursor)
function ReadDock([int]$Offset){[DesktopActionCheck]::Send($window,0x8028+$Offset,0,0)}
function ReadCalendar([int]$Offset){[DesktopActionCheck]::Send($calendar,0x8028+$Offset,0,0)}
function Await([scriptblock]$Check,[string]$Label){for($i=0;$i -lt 150;$i++){if(& $Check){return};Start-Sleep -Milliseconds 20};throw $Label}
function Click([IntPtr]$Target,[double]$X,[double]$Y){$scale=[DesktopActionCheck]::GetDpiForWindow($Target)/96.0;$point=([int][Math]::Round($Y*$scale) -shl 16) -bor ([int][Math]::Round($X*$scale) -band 65535);[void][DesktopActionCheck]::Send($Target,0x201,1,$point);[void][DesktopActionCheck]::Send($Target,0x202,0,$point)}
function OpenCalendar {
 if((ReadDock 1) -ne 2 -or (ReadDock 9) -ne 1000){[void][DesktopActionCheck]::Send($window,0x312,3,0);Await {(ReadDock 1) -eq 2 -and (ReadDock 9) -eq 1000} 'Clock reveal did not settle.'}
 Click $window ((ReadDock 25)/1000.0) ((ReadDock 7)/1000.0)
 Await {$script:calendar=[DesktopActionCheck]::FindClass($process.Id,'MinimaDock.Calendar');$calendar -ne [IntPtr]::Zero -and (ReadCalendar 0) -eq 1} 'Calendar did not open from clock button.'
}
function Settled {Await {(ReadCalendar 4) -eq 0 -and (ReadCalendar 5) -eq 1000 -and (ReadCalendar 8) -eq 0} 'Calendar animation did not settle.'}
try {
 $fixture=[DesktopActionCheck]::Fixture()
 for($attempt=0;$attempt -lt 3 -and [DesktopActionCheck]::GetForegroundWindow() -ne $fixture;$attempt++){[void][DesktopActionCheck]::ActivateOwnFixture($fixture);Start-Sleep -Milliseconds 100}
 if([DesktopActionCheck]::GetForegroundWindow() -ne $fixture){throw 'Could not activate blank calendar fixture.'}
 $process=Start-Process -FilePath ([IO.Path]::GetFullPath($Executable)) -ArgumentList '--dock' -WindowStyle Hidden -PassThru
 Await {$script:window=[DesktopActionCheck]::Find($process.Id);$window -ne [IntPtr]::Zero -and (ReadDock 0) -eq 1} 'Dock not ready.'
 [void][DesktopActionCheck]::Send($window,0x100,9,0)
 Await {(ReadDock 1) -eq 2 -and (ReadDock 9) -eq 1000} 'Dock reveal did not settle.'
 $anchor=ReadDock 16;$allocations=ReadDock 17
 $bounds=New-Object DesktopActionCheck+Rect;[void][DesktopActionCheck]::GetWindowRect($window,[ref]$bounds)
 OpenCalendar
 if((ReadCalendar 15) -ne 2){throw 'Calendar navigation does not use the vertical axis.'}
 if((ReadDock 19) -ne 4){throw 'Calendar activation lost its flyout hold.'}
 Settled
 $popup=New-Object DesktopActionCheck+Rect;[void][DesktopActionCheck]::GetWindowRect($calendar,[ref]$popup)
 $scale=[DesktopActionCheck]::GetDpiForWindow($window)/96.0;$work=[DesktopActionCheck]::WorkArea($window)
 $clockCenter=$bounds.Left+(ReadDock 25)*$scale/1000
 $expectedLeft=[Math]::Max($work.Left,[Math]::Min($work.Right-($popup.Right-$popup.Left),$clockCenter-($popup.Right-$popup.Left)/2))
 if([Math]::Abs($popup.Left-$expectedLeft) -gt 1){throw 'Calendar is not centered above its clock button.'}
 $pillTop=$bounds.Top+((ReadDock 7)/1000-22)*$scale
 if([Math]::Abs(($popup.Bottom-10*$scale)-($pillTop-12*$scale)) -gt 1){throw 'Calendar does not use the flyout gap above the pill.'}
 $now=[DateTime]::Now.Date
 if((ReadCalendar 1) -ne $now.Year -or (ReadCalendar 2) -ne $now.Month -or (ReadCalendar 3) -ne $now.Day){throw 'Calendar does not initialize to the local date.'}
 $calendarAllocations=ReadCalendar 6
 Click $calendar 319 88
 $next=$now.AddMonths(1)
 if((ReadCalendar 1) -ne $next.Year -or (ReadCalendar 2) -ne $next.Month){throw 'Next month did not update immediately.'}
 if((ReadDock 24) -eq 0){Start-Sleep -Milliseconds 50;if((ReadCalendar 8) -eq 0 -or (ReadCalendar 4) -ne 1){throw 'Month slide did not present intermediate motion.'}}
 Settled
 Click $calendar 283 88
 Settled
 if((ReadCalendar 1) -ne $now.Year -or (ReadCalendar 2) -ne $now.Month){throw 'Previous month failed.'}
 # Two high-resolution half-wheel steps should produce one month change.
 [void][DesktopActionCheck]::Send($calendar,0x20A,(60 -shl 16),0)
 if((ReadCalendar 2) -ne $now.Month){throw 'Partial wheel delta skipped a month.'}
 [void][DesktopActionCheck]::Send($calendar,0x20A,(60 -shl 16),0)
 $prior=$now.AddMonths(-1)
 if((ReadCalendar 1) -ne $prior.Year -or (ReadCalendar 2) -ne $prior.Month){throw 'Accumulated wheel navigation failed.'}
 Click $calendar 69 416
 Settled
 # Rapid navigation must cross year boundaries correctly and retain the target.
 for($i=0;$i -lt 15;$i++){Click $calendar 319 88}
 $future=$now.AddMonths(15)
 if((ReadCalendar 1) -ne $future.Year -or (ReadCalendar 2) -ne $future.Month){throw 'Rapid month navigation lost its target.'}
 for($i=0;$i -lt 15;$i++){Click $calendar 283 88}
 Settled
 [void][DesktopActionCheck]::Send($calendar,0x100,39,0)
 $selected=$now.AddDays(1)
 if((ReadCalendar 9) -ne $selected.Year -or (ReadCalendar 10) -ne $selected.Month -or (ReadCalendar 3) -ne $selected.Day){throw 'Keyboard date selection failed.'}
 [void][DesktopActionCheck]::Send($calendar,0x100,36,0)
 Settled
 if((ReadCalendar 3) -ne $now.Day -or (ReadCalendar 6) -ne $calendarAllocations){throw 'Today failed or navigation rebuilt render resources.'}
 if((ReadDock 17) -ne $allocations -or (ReadDock 16) -ne $anchor -or (ReadDock 5) -ne 44000){throw 'Calendar changed taskbar geometry/resources.'}
 $after=New-Object DesktopActionCheck+Rect;[void][DesktopActionCheck]::GetWindowRect($window,[ref]$after)
 if($after.Left -ne $bounds.Left -or $after.Top -ne $bounds.Top -or $after.Bottom -ne $bounds.Bottom){throw 'Calendar moved or resized the dock window.'}
 [void][DesktopActionCheck]::Send($calendar,0x100,27,0)
 if((ReadDock 24) -eq 0){
  Start-Sleep -Milliseconds 40
  if((ReadCalendar 11) -ne 1 -or (ReadCalendar 5) -le 0 -or (ReadCalendar 5) -ge 1000 -or (ReadCalendar 13) -ge 1000 -or (ReadCalendar 14) -le 0){throw 'Calendar close did not present a fading, contracting exit frame.'}
 }
 Await {[DesktopActionCheck]::FindClass($process.Id,'MinimaDock.Calendar') -eq [IntPtr]::Zero -and (ReadDock 19) -eq 0} 'Escape did not close calendar and release flyout.'
 OpenCalendar;Settled
 Click $window ((ReadDock 25)/1000.0) ((ReadDock 7)/1000.0)
 Await {[DesktopActionCheck]::FindClass($process.Id,'MinimaDock.Calendar') -eq [IntPtr]::Zero -and (ReadDock 19) -eq 0} 'Clock click did not toggle calendar closed.'
 OpenCalendar;Settled
 for($attempt=0;$attempt -lt 3 -and [DesktopActionCheck]::GetForegroundWindow() -ne $fixture;$attempt++){[void][DesktopActionCheck]::ActivateOwnFixture($fixture);Start-Sleep -Milliseconds 100}
 if([DesktopActionCheck]::GetForegroundWindow() -ne $fixture){throw ('Could not activate owned outside-click fixture: '+[DesktopActionCheck]::State($fixture))}
 try {Await {[DesktopActionCheck]::FindClass($process.Id,'MinimaDock.Calendar') -eq [IntPtr]::Zero -and (ReadDock 19) -eq 0} 'Outside activation did not dismiss calendar.'}
 catch {$remaining=[DesktopActionCheck]::FindClass($process.Id,'MinimaDock.Calendar');$details='panel='+(ReadDock 19)+', calendar='+$remaining;if($remaining -ne [IntPtr]::Zero){$details+=', timer='+(ReadCalendar 4)+', alpha='+(ReadCalendar 5)+', closing='+(ReadCalendar 11)+', focused='+(ReadCalendar 12)};throw ('Outside dismissal: '+$details)}
 'Calendar passed: clock-centered placement and flyout gap, clock toggle, local date, month arrows, intermediate animation, wheel accumulation, 30 rapid month changes/year rollover, keyboard selection/Today, finite timer shutdown, retained resources, fixed taskbar bounds, Escape and outside dismissal.'
 [void][DesktopActionCheck]::Send($window,0x10,0,0)
 if(-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0){throw 'Dock did not close cleanly.'}
} finally {
 [DesktopActionCheck]::CloseFixture()
 if($process){$process.Refresh();if(-not $process.HasExited){Stop-Process -Id $process.Id -Force};$process.Dispose()}
 [void][DesktopActionCheck]::SetForegroundWindow($previous)
 [void][DesktopActionCheck]::SetCursorPos($cursor.X,$cursor.Y)
}
