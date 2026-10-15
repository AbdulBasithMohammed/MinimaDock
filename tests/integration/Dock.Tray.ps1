param([string]$Executable=(Join-Path $PSScriptRoot '..\..\build\portable\MinimaDock.exe'),[int]$ExistingPid=0,[string]$Screenshot)
$ErrorActionPreference='Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'Desktop.Fixture.cs')
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class TrayFlashCheck {
 delegate void EventCallback(IntPtr h,uint e,IntPtr w,int obj,int child,uint thread,uint time);
 static EventCallback callback=OnEvent;static IntPtr hook;public static int Flashes;
 [DllImport("user32.dll")] static extern IntPtr SetWinEventHook(uint a,uint b,IntPtr mod,EventCallback cb,uint pid,uint tid,uint flags);
 [DllImport("user32.dll")] static extern bool UnhookWinEvent(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [StructLayout(LayoutKind.Sequential)] struct Msg{public IntPtr window;public uint message;public UIntPtr w;public IntPtr l;public uint time;public int x,y;public uint extra;}
 [DllImport("user32.dll")] static extern bool PeekMessage(out Msg m,IntPtr w,uint a,uint b,uint flags);
 [DllImport("user32.dll")] static extern bool TranslateMessage(ref Msg m);
 [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref Msg m);
 static void OnEvent(IntPtr h,uint e,IntPtr w,int obj,int child,uint t,uint ms){if(obj!=0)return;var s=new StringBuilder(128);GetClassName(w,s,s.Capacity);if(s.ToString()=="TopLevelWindowForOverflowXamlIsland"||s.ToString()=="NotifyIconOverflowWindow")Flashes++;}
 public static void Start(){Flashes=0;hook=SetWinEventHook(0x8002,0x8002,IntPtr.Zero,callback,0,0,0);if(hook==IntPtr.Zero)throw new Exception("Tray show observer failed");}
 public static void Pump(){Msg m;while(PeekMessage(out m,IntPtr.Zero,0,0,1)){TranslateMessage(ref m);DispatchMessage(ref m);}}
 public static void Stop(){if(hook!=IntPtr.Zero)UnhookWinEvent(hook);hook=IntPtr.Zero;}
}
'@
$process=$null;$window=[IntPtr]::Zero;$ownsProcess=$ExistingPid -eq 0
$previous=[DesktopActionCheck]::GetForegroundWindow();$cursor=New-Object DesktopActionCheck+Point
[void][DesktopActionCheck]::GetCursorPos([ref]$cursor)
function ReadDock([int]$Offset){[DesktopActionCheck]::Send($window,0x8028+$Offset,0,0)}
function Await([scriptblock]$Check,[string]$Label){for($i=0;$i -lt 150;$i++){[TrayFlashCheck]::Pump();if(& $Check){return};Start-Sleep -Milliseconds 20};throw ($Label+'; panel='+ (ReadDock 19) +'; active='+ (ReadDock 38) +'; opacity='+ (ReadDock 21) +'; foregroundOwn='+ ([DesktopActionCheck]::GetForegroundWindow() -eq $window))}
function ClickArrow {
 [void][DesktopActionCheck]::Send($window,0x312,3,0)
 Await {(ReadDock 1) -eq 2 -and (ReadDock 9) -eq 1000} 'Dock reveal did not settle.'
 $scale=[DesktopActionCheck]::GetDpiForWindow($window)/96.0
 $x=[int][Math]::Round((ReadDock 37)*$scale/1000);$y=[int][Math]::Round((ReadDock 7)*$scale/1000)
 [DesktopActionCheck]::ClickOwnDock($window,$x,$y)
}
try {
 [TrayFlashCheck]::Start()
 if($ownsProcess){$process=Start-Process -FilePath ([IO.Path]::GetFullPath($Executable)) -ArgumentList '--dock' -WindowStyle Hidden -PassThru}else{$process=Get-Process -Id $ExistingPid;if([IO.Path]::GetFullPath($process.Path) -ne [IO.Path]::GetFullPath($Executable)){throw 'Existing PID is not requested dock executable.'}}
 Await {$script:window=[DesktopActionCheck]::Find($process.Id);$window -ne [IntPtr]::Zero -and (ReadDock 0) -eq 1} 'Dock not ready.'
 if(((ReadDock 35) -band 3) -ne 3){throw ('Custom tray not ready; HRESULT '+(ReadDock 39))}
 Await {(ReadDock 40) -gt 0} 'Custom tray contains no registered icons.'
 Start-Sleep -Milliseconds 1500
 $count=ReadDock 40
 ClickArrow
 Await {(ReadDock 19) -eq 3 -and (ReadDock 38) -eq 1 -and (ReadDock 21) -eq 1000} 'Custom tray did not settle.'
 if([IntPtr](ReadDock 36) -ne $window){throw 'Tray is not drawn by the dock.'}
 $scale=[DesktopActionCheck]::GetDpiForWindow($window)/96.0
 $left=(ReadDock 42)/1000.0;$top=(ReadDock 43)/1000.0;$width=(ReadDock 44)/1000.0;$height=(ReadDock 45)/1000.0
 $bounds=New-Object DesktopActionCheck+Rect;[void][DesktopActionCheck]::GetWindowRect($window,[ref]$bounds)
 $center=(ReadDock 37)/1000.0;$canvasWidth=($bounds.Right-$bounds.Left)/$scale
 $expectedLeft=[Math]::Max(20,[Math]::Min($canvasWidth-20-$width,$center-$width/2))
 if([Math]::Abs($left-$expectedLeft) -gt 1){throw 'Tray is not anchored over its arrow.'}
 if([Math]::Abs(($top+$height)-((ReadDock 7)/1000.0-22-12)) -gt 1){throw 'Tray does not keep flyout gap.'}
 [void][DesktopActionCheck]::Send($window,0x100,9,0)
 [void][DesktopActionCheck]::Send($window,0x100,39,0)
 if($Screenshot){Add-Type -AssemblyName System.Drawing;$image=New-Object Drawing.Bitmap ([int]($width*$scale)),([int]($height*$scale));$graphics=[Drawing.Graphics]::FromImage($image);try{$graphics.CopyFromScreen([int]($bounds.Left+$left*$scale),[int]($bounds.Top+$top*$scale),0,0,$image.Size);$image.Save([IO.Path]::GetFullPath($Screenshot),[Drawing.Imaging.ImageFormat]::Png)}finally{$graphics.Dispose();$image.Dispose()}}
 for($i=0;$i -lt 12;$i++){
  [void][DesktopActionCheck]::Send($window,0x100,27,0);Await {(ReadDock 19) -eq 0} 'Escape did not close custom tray.'
  ClickArrow;Await {(ReadDock 19) -eq 3 -and (ReadDock 21) -eq 1000} 'Custom tray did not reopen.'
  ClickArrow;Await {(ReadDock 19) -eq 0} 'Arrow did not toggle tray closed.'
  if($i -lt 11){ClickArrow;Await {(ReadDock 19) -eq 3 -and (ReadDock 21) -eq 1000} 'Tray did not reopen for next cycle.'}
 }
 [TrayFlashCheck]::Pump()
 if([TrayFlashCheck]::Flashes -ne 0){throw ('Explorer overflow flashed '+[TrayFlashCheck]::Flashes+' times.')}
 Write-Output ('Custom dock tray passed: '+$count+' real icons, own glass panel, anchoring, finite reveal, keyboard focus, Escape/toggle and zero Explorer popup shows.')
} finally {
 [TrayFlashCheck]::Stop()
 if($window -ne [IntPtr]::Zero){try{[void][DesktopActionCheck]::Send($window,0x100,27,0)}catch{} }
 if($ownsProcess -and $process -and -not $process.HasExited){[void][DesktopActionCheck]::Send($window,0x10,0,0);if(-not $process.WaitForExit(3000)){throw 'Own dock did not stop cleanly.'}}
 [void][DesktopActionCheck]::SetCursorPos($cursor.X,$cursor.Y)
 if($previous -ne [IntPtr]::Zero){[void][DesktopActionCheck]::SetForegroundWindow($previous)}
}

