param([string]$Executable=(Join-Path $PSScriptRoot '..\..\out\MinimaDock.exe'))
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MinimaPreviewSmoke {
    public delegate bool WindowCallback(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(WindowCallback c,IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll",SetLastError=true)] public static extern IntPtr SendMessageTimeout(IntPtr h,uint m,IntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
    public static IntPtr Find(uint pid) { IntPtr result=IntPtr.Zero; WindowCallback c=delegate(IntPtr h,IntPtr p){uint owner;GetWindowThreadProcessId(h,out owner);if(owner==pid){result=h;return false;}return true;};EnumWindows(c,IntPtr.Zero);return result; }
}
'@
$process=Start-Process -FilePath ([IO.Path]::GetFullPath($Executable)) -ArgumentList '--preview' -WindowStyle Hidden -PassThru
try {
    $window=[IntPtr]::Zero
    for($i=0;$i -lt 50;$i++) { $process.Refresh(); if($process.HasExited){throw 'Preview exited during startup.'}; $window=[MinimaPreviewSmoke]::Find($process.Id); if($window -ne [IntPtr]::Zero){break}; Start-Sleep -Milliseconds 100 }
    if($window -eq [IntPtr]::Zero){throw 'No preview window found.'}
    [UIntPtr]$reply=[UIntPtr]::Zero
    if([MinimaPreviewSmoke]::SendMessageTimeout($window,0,[IntPtr]::Zero,[IntPtr]::Zero,3,2000,[ref]$reply) -eq [IntPtr]::Zero){throw 'Preview UI thread did not respond.'}
    # Only the HWND belonging to this test's spawned process receives these messages.
    foreach($key in @(9,39,27,70,70,82,82)) { [void][MinimaPreviewSmoke]::PostMessage($window,0x100,[IntPtr]$key,[IntPtr]::Zero) }
    Start-Sleep -Milliseconds 500
    if([MinimaPreviewSmoke]::SendMessageTimeout($window,0,[IntPtr]::Zero,[IntPtr]::Zero,3,2000,[ref]$reply) -eq [IntPtr]::Zero){throw 'Preview stopped responding after keyboard events.'}
    [void][MinimaPreviewSmoke]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    if(-not $process.WaitForExit(10000)){throw 'Preview did not shut down cleanly.'}
    if($process.ExitCode -ne 0){throw ('Preview exit code '+$process.ExitCode)}
    Write-Output 'Preview startup, UI liveness, keyboard events and clean shutdown passed. No desktop configuration changed.'
} finally {
    $process.Refresh()
    if(-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    $process.Dispose()
}
