param([string]$Executable=(Join-Path $PSScriptRoot '..\..\build\portable\MinimaDock.exe'))
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class DockMotionCheck {
 public delegate bool EnumCallback(IntPtr w,IntPtr p);
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;}
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumCallback c,IntPtr p);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr w,StringBuilder b,int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w,out Rect r);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr w);
 [DllImport("user32.dll",SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,IntPtr b,uint flags,uint timeout,out UIntPtr result);
 public static IntPtr Find(uint pid){IntPtr result=IntPtr.Zero;EnumCallback callback=delegate(IntPtr w,IntPtr p){uint owner;GetWindowThreadProcessId(w,out owner);var name=new StringBuilder(128);GetClassName(w,name,128);if(owner==pid&&name.ToString()=="MinimaDock.DesktopDock"){result=w;return false;}return true;};EnumWindows(callback,IntPtr.Zero);return result;}
 public static long Send(IntPtr w,uint m,long a){UIntPtr r;if(SendMessageTimeout(w,m,new IntPtr(a),IntPtr.Zero,3,2000,out r)==IntPtr.Zero)throw new Exception("Own dock UI did not respond.");return unchecked((long)r.ToUInt64());}
 public static Rect Bounds(IntPtr w){Rect r;if(!GetWindowRect(w,out r))throw new Exception("No own dock bounds.");return r;}
}
'@
$process=Start-Process -FilePath ([IO.Path]::GetFullPath($Executable)) -ArgumentList '--dock' -WindowStyle Hidden -PassThru
$window=[IntPtr]::Zero
try {
    for($i=0;$i -lt 150;$i++){
        $process.Refresh();if($process.HasExited){throw 'Dock exited at startup.'}
        $window=[DockMotionCheck]::Find($process.Id)
        if($window -ne [IntPtr]::Zero -and [DockMotionCheck]::Send($window,0x8028,0) -eq 1){break}
        Start-Sleep -Milliseconds 100
    }
    if($window -eq [IntPtr]::Zero){throw 'Own desktop dock not found.'}
    $before=[DockMotionCheck]::Bounds($window)
    $scale=[DockMotionCheck]::GetDpiForWindow($window)/96.0
    if([Math]::Abs(($before.Bottom-$before.Top)/$scale-88) -gt 1){throw 'Idle render layer is larger than 88 DIP.'}
    $count=[DockMotionCheck]::Send($window,0x802a,0)
    if([DockMotionCheck]::Send($window,0x802d,0) -ne 44000){throw 'Collapsed pill height is not 44 DIP.'}
    $compactWidth=[DockMotionCheck]::Send($window,0x8036,0)
    $compactLeft=[DockMotionCheck]::Send($window,0x8057,0)
    if([Math]::Abs($compactLeft+$compactWidth/2-($before.Right-$before.Left)/$scale*500) -gt 1000){throw 'Compact app group is not centered.'}
    $positions=@();for($i=0;$i -lt $count;$i++){$positions+=,[DockMotionCheck]::Send($window,0x802b,$i)}
    $seenUnpinned=$false
    $launcher=[DockMotionCheck]::Send($window,0x8056,0)
    for($i=0;$i -lt $count;$i++){
        $flags=[DockMotionCheck]::Send($window,0x802c,$i)
        if(($flags -band 2) -eq 0){$seenUnpinned=$true}
        elseif($seenUnpinned){throw 'Pinned app appeared after an unpinned running app.'}
        if($i -gt 0 -and $positions[$i]-$positions[$i-1] -ne 40000){throw 'Apps are not in one ordered row.'}
        if($positions[$i]+($before.Right-$before.Left)/$scale*500 -le $launcher){throw 'App appeared before the launcher.'}
    }
    [void][DockMotionCheck]::Send($window,0x100,9) # Tab on this test's own HWND.
    if([DockMotionCheck]::Send($window,0x8029,0) -ne 1){throw 'Expansion did not start on input.'}
    if([DockMotionCheck]::Send($window,0x802d,0) -ne 44000){throw 'Pill grows vertically during expansion.'}
    Start-Sleep -Milliseconds 130
    if([DockMotionCheck]::Send($window,0x8029,0) -ne 2){throw 'Expansion did not complete promptly.'}
    if([DockMotionCheck]::Send($window,0x802d,0) -ne 44000){throw 'Expanded pill height changed.'}
    if([DockMotionCheck]::Send($window,0x8056,0) -ne $launcher -or ([DockMotionCheck]::Bounds($window)).Left -ne $before.Left){throw 'Hover moved the launcher screen position.'}
    $expandedWidth=[DockMotionCheck]::Send($window,0x8036,0)
    $expandedLeft=[DockMotionCheck]::Send($window,0x8057,0)
    if($expandedLeft -ge $compactLeft -or $expandedLeft+$expandedWidth -le $compactLeft+$compactWidth){throw 'Hover did not expand both wings.'}
    if([Math]::Abs(($compactLeft-$expandedLeft)-($expandedWidth-$compactWidth)/2) -gt 1000){throw 'Hover wings are not symmetric.'}
    for($i=0;$i -lt $count;$i++){
        $expanded=[DockMotionCheck]::Send($window,0x802b,$i)
        if($positions[$i] -ne $expanded){throw 'An app moved while revealing utility controls.'}
    }
    for($left=0;$left -lt 6;$left++){[void][DockMotionCheck]::Send($window,0x100,37)} # Pass media, Show desktop and calendar to select quick settings.
    [void][DockMotionCheck]::Send($window,0x100,13)
    $flyout=[DockMotionCheck]::Bounds($window)
    if([Math]::Abs(($flyout.Bottom-$flyout.Top)/$scale-440) -gt 1 -or $flyout.Bottom -ne $before.Bottom){throw 'Flyout resize moved the bottom anchor.'}
    [void][DockMotionCheck]::Send($window,0x100,27)
    $after=[DockMotionCheck]::Bounds($window)
    if($after.Bottom -ne $before.Bottom -or $after.Top -ne $before.Top){throw 'Closing flyout did not restore narrow layer.'}
    [void][DockMotionCheck]::Send($window,0x100,27)
    if([DockMotionCheck]::Send($window,0x8029,0) -ne 3){throw 'Collapse did not start immediately.'}
    Start-Sleep -Milliseconds 130
    if([DockMotionCheck]::Send($window,0x8029,0) -ne 0){throw 'Collapse did not finish promptly.'}
    for($i=0;$i -lt $count;$i++){if([DockMotionCheck]::Send($window,0x802b,$i) -ne $positions[$i]){throw 'An app moved while hiding utility controls.'}}
    [void][DockMotionCheck]::Send($window,0x10,0)
    if(-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0){throw 'Own dock did not close cleanly.'}
    'Desktop component passed: left launcher, pinned-first app order, immediate transitions, fixed app coordinates across hover, narrow render layer, fixed bottom anchor, quick settings open/close, clean shutdown. Explorer taskbar untouched.'
} finally {
    $process.Refresh();if(-not $process.HasExited){Stop-Process -Id $process.Id -Force}
    $process.Dispose()
}
