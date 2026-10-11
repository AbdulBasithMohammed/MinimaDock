using System;
using System.Text;
using System.Threading;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class SnapFixture {
 [StructLayout(LayoutKind.Sequential)]public struct MonitorInfo {public uint Size;public Rect Monitor,Work;public uint Flags;}
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;public override string ToString(){return Left+","+Top+","+Right+","+Bottom;}}
 [StructLayout(LayoutKind.Sequential)] struct Placement {public uint Length,Flags,Show;public int MinX,MinY,MaxX,MaxY;public Rect Normal;}
 [StructLayout(LayoutKind.Sequential)] struct Keyboard {public ushort Key,Scan;public uint Flags,Time;public UIntPtr Extra;}
 [StructLayout(LayoutKind.Explicit,Size=40)] struct Input {[FieldOffset(0)]public uint Type;[FieldOffset(8)]public Keyboard Keyboard;}
 [DllImport("user32.dll")] static extern uint SendInput(uint count,Input[] input,int bytes);
 [DllImport("user32.dll")] public static extern bool IsWindowArranged(IntPtr w);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr w,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassName(IntPtr w,StringBuilder cls,int count);
 [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr w,out Rect r);
 [DllImport("user32.dll")]static extern bool GetWindowPlacement(IntPtr w,ref Placement p);
 [DllImport("user32.dll")]static extern bool SetWindowPos(IntPtr w,IntPtr after,int x,int y,int width,int height,uint flags);
 [DllImport("user32.dll")]static extern IntPtr MonitorFromWindow(IntPtr w,uint flags);
 [DllImport("user32.dll")]static extern bool GetMonitorInfo(IntPtr monitor,ref MonitorInfo info);
 [DllImport("user32.dll")]static extern bool SetWindowPlacement(IntPtr w,ref Placement p);
 [DllImport("dwmapi.dll")]static extern int DwmGetWindowAttribute(IntPtr w,uint attr,out Rect r,uint bytes);
 [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 static void Own(IntPtr w){uint pid;GetWindowThreadProcessId(w,out pid);var cls=new StringBuilder(128);GetClassName(w,cls,128);if(pid!=(uint)Process.GetCurrentProcess().Id||cls.ToString()!="MinimaDock.DesktopActionFixture")throw new Exception("Not our blank snap fixture.");}
 public static Rect Frame(IntPtr w){Rect r;if(DwmGetWindowAttribute(w,9,out r,16)!=0)throw new Exception("Cannot read frame.");return r;}
 public static Rect Normal(IntPtr w){var p=new Placement();p.Length=(uint)Marshal.SizeOf(typeof(Placement));if(!GetWindowPlacement(w,ref p))throw new Exception("Cannot read restore placement.");return p.Normal;}
 public static MonitorInfo Area(IntPtr w){var info=new MonitorInfo();info.Size=(uint)Marshal.SizeOf(typeof(MonitorInfo));if(!GetMonitorInfo(MonitorFromWindow(w,1),ref info))throw new Exception("Cannot read fixture work area.");return info;}
 public static void Restore(IntPtr w){Own(w);var p=new Placement();p.Length=(uint)Marshal.SizeOf(typeof(Placement));if(!GetWindowPlacement(w,ref p))throw new Exception("Cannot read fixture placement.");p.Show=1;if(!SetWindowPlacement(w,ref p))throw new Exception("Cannot restore fixture.");}
 public static void Snap(IntPtr w,bool right){Own(w);DesktopActionCheck.ShowWindow(w,3);if(!DesktopActionCheck.ActivateOwnFixture(w)||DesktopActionCheck.GetForegroundWindow()!=w)throw new Exception("Cannot activate own fixture for native snap.");var keys=new Input[4];for(int i=0;i<4;i++)keys[i].Type=1;keys[0].Keyboard.Key=0x5B;keys[1].Keyboard.Key=(ushort)(right?0x27:0x25);keys[1].Keyboard.Flags=1;keys[2]=keys[1];keys[2].Keyboard.Flags=3;keys[3]=keys[0];keys[3].Keyboard.Flags=2;uint sent=SendInput(4,keys,40);if(sent!=4){SendInput(2,new Input[]{keys[2],keys[3]},40);throw new Exception("Native snap input incomplete.");}for(int i=0;i<40&&!IsWindowArranged(w);i++)Thread.Sleep(25);if(!IsWindowArranged(w))throw new Exception("Windows did not mark our fixture as snapped.");Thread.Sleep(200);}
 public static void ResizeBottom(IntPtr w,int bottom){Own(w);Rect outer;GetWindowRect(w,out outer);var frame=Frame(w);if(!SetWindowPos(w,IntPtr.Zero,outer.Left,outer.Top,outer.Right-outer.Left,bottom+outer.Bottom-frame.Bottom-outer.Top,0x214))throw new Exception("Own fixture resize failed.");}
}
