# Explicit live taskbar trial validation. Explorer stays running; only MinimaDock
# processes, captured taskbar visibility and journaled work areas are changed.
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Threading;
using System.Runtime.InteropServices;
public static class MinimaLiveCheck {
    [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom; public override string ToString(){return Left+","+Top+","+Right+","+Bottom;}}
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] public struct MonitorInfo {public uint Size;public Rect Bounds,Work;public uint Flags;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string Device;}
    delegate bool MonitorCallback(IntPtr m,IntPtr dc,IntPtr r,IntPtr p);
    [DllImport("user32.dll")]static extern bool EnumDisplayMonitors(IntPtr dc,IntPtr clip,MonitorCallback callback,IntPtr p);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern bool GetMonitorInfo(IntPtr m,ref MonitorInfo info);
    [DllImport("user32.dll")]static extern bool SystemParametersInfo(uint action,uint param,out Rect rect,uint flags);
    [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")]public static extern bool IsZoomed(IntPtr w);
    [StructLayout(LayoutKind.Sequential)]struct Placement {public uint Length,Flags,Show;public int MinX,MinY,MaxX,MaxY;public Rect Normal;}
    [DllImport("user32.dll")]static extern bool GetWindowPlacement(IntPtr w,ref Placement p);
    public static Rect NormalBounds(IntPtr w){var p=new Placement();p.Length=(uint)Marshal.SizeOf(typeof(Placement));if(!GetWindowPlacement(w,ref p))throw new Exception("Cannot read fixture restore placement.");return p.Normal;}
    [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr w,out Rect r);
    [DllImport("dwmapi.dll")]static extern int DwmGetWindowAttribute(IntPtr w,uint attr,out Rect r,uint bytes);
    public static Rect Bounds(IntPtr w){Rect r;if(!GetWindowRect(w,out r))throw new Exception("Cannot read taskbar bounds.");return r;}
    public static Rect VisibleBounds(IntPtr w){Rect r;if(DwmGetWindowAttribute(w,9,out r,16)!=0)throw new Exception("Cannot read fixture frame.");return r;}
    public static string Diagnostic(IntPtr fixture){Rect r;SystemParametersInfo(0x30,0,out r,0);return "SPI="+r+"; fixture="+VisibleBounds(fixture);}
    public static MonitorInfo[] Areas(){Pump();var list=new List<MonitorInfo>();MonitorCallback cb=delegate(IntPtr m,IntPtr dc,IntPtr r,IntPtr p){var info=new MonitorInfo();info.Size=(uint)Marshal.SizeOf(typeof(MonitorInfo));if(!GetMonitorInfo(m,ref info))throw new Exception("Cannot read work area.");list.Add(info);return true;};if(!EnumDisplayMonitors(IntPtr.Zero,IntPtr.Zero,cb,IntPtr.Zero))throw new Exception("Cannot enumerate work areas.");return list.ToArray();}
    delegate IntPtr WindowProc(IntPtr w,uint m,IntPtr a,IntPtr b);
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]struct WindowClass {public uint Style;public IntPtr Proc;public int ClassBytes,WindowBytes;public IntPtr Instance,Icon,Cursor,Background;public string Menu,Name;}
    [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern ushort RegisterClass(ref WindowClass c);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern IntPtr DefWindowProc(IntPtr w,uint m,IntPtr a,IntPtr b);
    static WindowProc fixtureProc=delegate(IntPtr w,uint m,IntPtr a,IntPtr b){return DefWindowProc(w,m,a,b);};
    [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern IntPtr CreateWindowEx(uint ex,string cls,string title,uint style,int x,int y,int width,int height,IntPtr parent,IntPtr menu,IntPtr instance,IntPtr data);
    [DllImport("user32.dll")]static extern bool ShowWindow(IntPtr w,int state);
    [DllImport("user32.dll")]static extern bool DestroyWindow(IntPtr w);
    [StructLayout(LayoutKind.Sequential)]struct Message {public IntPtr Window;public uint Id;public UIntPtr W;public IntPtr L;public uint Time;public int X,Y;public uint Private;}
    [DllImport("user32.dll")]static extern int GetMessage(out Message m,IntPtr w,uint min,uint max);
    [DllImport("user32.dll")]static extern bool PeekMessage(out Message m,IntPtr w,uint min,uint max,uint remove);
    static void Pump(){Message m;while(PeekMessage(out m,IntPtr.Zero,0,0,1)){TranslateMessage(ref m);DispatchMessage(ref m);}}
    [DllImport("user32.dll")]static extern bool TranslateMessage(ref Message m);
    [DllImport("user32.dll")]static extern IntPtr DispatchMessage(ref Message m);
    [DllImport("user32.dll")]static extern bool PostThreadMessage(uint t,uint m,UIntPtr a,IntPtr b);
    [DllImport("kernel32.dll")]static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")]static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]static extern bool SetForegroundWindow(IntPtr w);
    static Thread fixtureThread;static uint fixtureThreadId;static IntPtr previousForeground,fixtureWindow;
    public static IntPtr Fixture(){previousForeground=GetForegroundWindow();var ready=new ManualResetEvent(false);fixtureThread=new Thread(delegate(){SetThreadDpiAwarenessContext(new IntPtr(-4));fixtureThreadId=GetCurrentThreadId();var cls=new WindowClass();cls.Name="MinimaDock.MaximizeFixture";cls.Proc=Marshal.GetFunctionPointerForDelegate(fixtureProc);RegisterClass(ref cls);fixtureWindow=CreateWindowEx(0x40000,cls.Name,"MinimaDock maximized-window test",0xCF0000,80,80,300,160,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero);if(fixtureWindow!=IntPtr.Zero){ShowWindow(fixtureWindow,4);ShowWindow(fixtureWindow,3);}ready.Set();Message m;while(GetMessage(out m,IntPtr.Zero,0,0)>0){TranslateMessage(ref m);DispatchMessage(ref m);}if(GetForegroundWindow()==fixtureWindow)SetForegroundWindow(previousForeground);DestroyWindow(fixtureWindow);});fixtureThread.IsBackground=true;fixtureThread.SetApartmentState(ApartmentState.STA);fixtureThread.Start();if(!ready.WaitOne(5000)||fixtureWindow==IntPtr.Zero)throw new Exception("Cannot create maximized fixture.");return fixtureWindow;}
    public static void CloseFixture(){if(fixtureThread!=null&&fixtureThread.IsAlive){PostThreadMessage(fixtureThreadId,0x12,UIntPtr.Zero,IntPtr.Zero);fixtureThread.Join(5000);}}
    public delegate bool EnumCallback(IntPtr window,IntPtr context);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumCallback callback,IntPtr context);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window,StringBuilder value,int count);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("kernel32.dll")]static extern IntPtr OpenThread(uint access,bool inherit,uint tid);
    [DllImport("kernel32.dll")]static extern uint SuspendThread(IntPtr thread);
    [DllImport("kernel32.dll")]static extern bool CloseHandle(IntPtr handle);
    public static void SuspendDock(IntPtr window){uint pid;uint tid=GetWindowThreadProcessId(window,out pid);IntPtr thread=OpenThread(2,false,tid);if(thread==IntPtr.Zero)throw new Exception("Cannot open own dock UI thread.");try{if(SuspendThread(thread)==uint.MaxValue)throw new Exception("Cannot suspend own dock fixture.");}finally{CloseHandle(thread);}}
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string name,string title);
    public static IntPtr Dock(){return FindWindow("MinimaDock.DesktopDock",null);}
    public static IntPtr Control(){return FindWindow("MinimaDock.Trial.Control",null);}
    public static IntPtr[] Bars(){var bars=new List<IntPtr>();EnumCallback callback=delegate(IntPtr w,IntPtr p){var n=new StringBuilder(128);GetClassName(w,n,128);if(n.ToString()=="Shell_TrayWnd"||n.ToString()=="Shell_SecondaryTrayWnd")bars.Add(w);return true;};EnumWindows(callback,IntPtr.Zero);return bars.ToArray();}
}
'@
[void][MinimaLiveCheck]::SetThreadDpiAwarenessContext([IntPtr](-4))
$baselineHash=(Get-FileHash -LiteralPath (Join-Path $repo 'recovery-bundle\baseline.json')).Hash
$initial=@([MinimaLiveCheck]::Bars() | ForEach-Object {
    [uint32]$owner=0;[void][MinimaLiveCheck]::GetWindowThreadProcessId($_,[ref]$owner)
    [pscustomobject]@{Window=$_;Visible=[MinimaLiveCheck]::IsWindowVisible($_);Pid=$owner;Bounds=[MinimaLiveCheck]::Bounds($_)}
})
if($initial.Count -eq 0){throw 'No Windows taskbar available for live validation.'}
$areas=@([MinimaLiveCheck]::Areas() | ForEach-Object {
    $desired=$_.Work
    foreach($bar in $initial){$r=$bar.Bounds;if($r.Bottom -ge $_.Bounds.Bottom -and $r.Top -le $_.Work.Bottom -and $r.Top -gt $_.Bounds.Top -and $r.Left -le $_.Work.Left -and $r.Right -ge $_.Work.Right){$desired.Bottom=$_.Bounds.Bottom}}
    [pscustomobject]@{Device=$_.Device;Bounds=$_.Bounds;Original=$_.Work;Applied=$desired}
})
$fixture=[IntPtr]::Zero;$fixtureNormal=$null
function Assert-Areas([bool]$Active) {
    $current=[MinimaLiveCheck]::Areas()
    foreach($area in $areas) {
        $found=@($current | Where-Object Device -eq $area.Device)
        if($found.Count -ne 1 -or $found[0].Bounds.ToString() -ne $area.Bounds.ToString()){throw 'Display topology changed during live validation.'}
        $expected=if($Active){$area.Applied}else{$area.Original}
        if($found[0].Work.ToString() -ne $expected.ToString()){
            $probe=Join-Path $repo 'build\portable\work_area_probe.exe'
            $fresh=if(Test-Path -LiteralPath $probe){& $probe}else{'probe not built'}
            throw ('Work area disagrees on '+$area.Device+': '+$found[0].Work+' expected '+$expected+'; '+[MinimaLiveCheck]::Diagnostic($fixture)+'; fresh probe='+$fresh)
        }
    }
    if($fixture -ne [IntPtr]::Zero) {
        if(-not [MinimaLiveCheck]::IsZoomed($fixture)){throw 'Fixture lost its maximized state.'}
        if([MinimaLiveCheck]::NormalBounds($fixture).ToString() -ne $fixtureNormal.ToString()){throw 'Work-area resizing changed the fixture restore size/position.'}
        $frame=[MinimaLiveCheck]::VisibleBounds($fixture)
        $primary=$areas | Where-Object {$_.Bounds.Left -eq 0 -and $_.Bounds.Top -eq 0} | Select-Object -First 1
        $expected=if($Active){$primary.Applied}else{$primary.Original}
        if([Math]::Abs($frame.Bottom-$expected.Bottom) -gt 1){throw ('Already-maximized fixture bottom='+$frame.Bottom+' expected '+$expected.Bottom)}
    }
}
function Assert-Original {
    foreach($bar in $initial){if([MinimaLiveCheck]::IsWindowVisible($bar.Window) -ne $bar.Visible){throw 'Original taskbar visibility differs after revert.'};if(-not(Get-Process -Id $bar.Pid -ErrorAction SilentlyContinue)){throw 'Original Explorer process exited.'}}
    if((Get-FileHash -LiteralPath (Join-Path $repo 'recovery-bundle\baseline.json')).Hash -cne $baselineHash){throw 'Original shell baseline changed.'}
    if([MinimaLiveCheck]::Dock() -ne [IntPtr]::Zero){throw 'Dock remains after revert.'}
    Assert-Areas $false
}
function Assert-Hidden {
    foreach($bar in $initial){if([MinimaLiveCheck]::IsWindowVisible($bar.Window)){throw 'Windows taskbar did not hide during active trial.'}}
    if([MinimaLiveCheck]::Dock() -eq [IntPtr]::Zero){throw 'Desktop dock is missing.'}
    Assert-Areas $true
}
function Run-Action([string]$Action,[int]$Seconds=10) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Taskbar-Test.ps1') -Action $Action -Seconds $Seconds
    if($LASTEXITCODE -ne 0){throw ('Live validation action failed: '+$Action)}
}
try {
    $fixture=[MinimaLiveCheck]::Fixture()
    $fixtureNormal=[MinimaLiveCheck]::NormalBounds($fixture)
    Assert-Areas $false
    Run-Action Deploy
    Start-Sleep -Milliseconds 2500;Assert-Hidden
    Start-Sleep -Seconds 13;Assert-Original
    'LIVE PASS: full maximize area, already-maximized fixture grows, deadline restores original work area/visibility/Explorer/baseline.'
    Run-Action Deploy
    Run-Action Keep
    Start-Sleep -Seconds 11;Assert-Hidden
    Run-Action Revert
    Start-Sleep -Milliseconds 500;Assert-Original
    'LIVE PASS: Keep cancels deadline, Revert restores work area and shrinks maximized fixture.'
    Run-Action Deploy
    Start-Sleep -Milliseconds 500
    Assert-Hidden
    $record=Get-Content -LiteralPath (Join-Path $repo 'out\active-taskbar-test.json') -Raw | ConvertFrom-Json
    $control=[MinimaLiveCheck]::Control()
    [uint32]$guardPid=0;[void][MinimaLiveCheck]::GetWindowThreadProcessId($control,[ref]$guardPid)
    $guard=Get-Process -Id $guardPid
    $expected=[IO.Path]::Combine($record.Directory,'MinimaDockTrial.exe')
    if($guard.Path -cne $expected -or (Get-FileHash -LiteralPath $expected).Hash -cne $record.GuardSha256){throw 'Cannot authenticate supervisor for the crash recovery check.'}
    # Terminate only this authenticated development supervisor to test journal recovery.
    Stop-Process -Id $guardPid -Force
    Run-Action Revert
    Start-Sleep -Milliseconds 500;Assert-Original
    'LIVE PASS: standalone journal recovery after supervisor termination.'
    foreach($scenario in @('crash','hang')) {
        Run-Action Deploy
        Start-Sleep -Milliseconds 2500;Assert-Hidden
        $dock=[MinimaLiveCheck]::Dock();[uint32]$dockPid=0;[void][MinimaLiveCheck]::GetWindowThreadProcessId($dock,[ref]$dockPid)
        $record=Get-Content -LiteralPath (Join-Path $repo 'out\active-taskbar-test.json') -Raw | ConvertFrom-Json
        $dockProcess=Get-Process -Id $dockPid
        if($dockProcess.Path -cne (Join-Path $record.Directory 'MinimaDock.exe')){throw 'Cannot authenticate own dock fixture.'}
        if($scenario -eq 'crash'){Stop-Process -Id $dockPid -Force}else{[MinimaLiveCheck]::SuspendDock($dock)}
        Start-Sleep -Seconds 7
        Assert-Original
        ('LIVE PASS: dock '+$scenario+' restores taskbar and work area.')
    }
    $areas | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $repo 'captures\maximize-026-areas.json') -Encoding UTF8
} finally {try{Run-Action Revert}finally{[MinimaLiveCheck]::CloseFixture()}}
