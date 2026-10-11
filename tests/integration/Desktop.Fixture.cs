using System;
using System.Text;
using System.Threading;
using System.Runtime.InteropServices;
public static class DesktopActionCheck {
 public delegate bool EnumCallback(IntPtr w,IntPtr p);
 delegate IntPtr WindowProc(IntPtr w,uint m,IntPtr a,IntPtr b);
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct WindowClass {public uint Style;public IntPtr Proc;public int ClassBytes,WindowBytes;public IntPtr Instance,Icon,Cursor,Background;public string Menu,Name;}
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern ushort RegisterClass(ref WindowClass cls);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr DefWindowProc(IntPtr w,uint m,IntPtr a,IntPtr b);
 static WindowProc fixtureProc=delegate(IntPtr w,uint m,IntPtr a,IntPtr b){return DefWindowProc(w,m,a,b);};
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumCallback c,IntPtr p);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr w,StringBuilder b,int n);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr w);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr w);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
 [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr w,uint attr,out uint value,uint bytes);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr w);
 [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr w,int index);
 [DllImport("user32.dll")]static extern IntPtr SetWindowLongPtr(IntPtr w,int index,IntPtr value);
 [StructLayout(LayoutKind.Sequential)]struct MonitorInfo {public uint Size;public Rect Monitor,Work;public uint Flags;}
 [DllImport("user32.dll")]static extern IntPtr MonitorFromWindow(IntPtr w,uint flags);
 [DllImport("user32.dll")]static extern bool GetMonitorInfo(IntPtr m,ref MonitorInfo info);
 public static Rect WorkArea(IntPtr w){var info=new MonitorInfo();info.Size=(uint)Marshal.SizeOf(typeof(MonitorInfo));if(!GetMonitorInfo(MonitorFromWindow(w,2),ref info))throw new Exception("No window monitor.");return info.Work;}
 public static void FullscreenOwnFixture(IntPtr w){var cls=new StringBuilder(128);GetClassName(w,cls,128);if(cls.ToString()!="MinimaDock.DesktopActionFixture")throw new Exception("Not an owned fixture.");var info=new MonitorInfo();info.Size=(uint)Marshal.SizeOf(typeof(MonitorInfo));if(!GetMonitorInfo(MonitorFromWindow(w,1),ref info))throw new Exception("No fixture monitor.");SetWindowLongPtr(w,-16,new IntPtr(unchecked((int)0x90000000)));var r=info.Monitor;SetWindowPos(w,IntPtr.Zero,r.Left,r.Top,r.Right-r.Left,r.Bottom-r.Top,0x34);}
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;}
 [StructLayout(LayoutKind.Sequential)] public struct Point {public int X,Y;}
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w,out Rect r);
 [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point p);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 public static void MoveToOwnDock(IntPtr w,int x,int y){var cls=new StringBuilder(128);GetClassName(w,cls,128);if(cls.ToString()!="MinimaDock.RevealEdge"&&cls.ToString()!="MinimaDock.DesktopDock")throw new Exception("Not an own dock test surface.");Rect r;if(!GetWindowRect(w,out r)||x<0||y<0||x>=r.Right-r.Left||y>=r.Bottom-r.Top)throw new Exception("Point outside own dock.");if(!SetCursorPos(r.Left+x,r.Top+y))throw new Exception("Could not move pointer to own dock.");}
 [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr w,IntPtr after,int x,int y,int width,int height,uint flags);
 [DllImport("user32.dll")]static extern IntPtr WindowFromPoint(Point point);
 [DllImport("user32.dll")]static extern IntPtr GetAncestor(IntPtr w,uint flags);
 [StructLayout(LayoutKind.Sequential)]struct MouseInput {public int X,Y;public uint Data,Flags,Time;public UIntPtr Extra;}
 [StructLayout(LayoutKind.Explicit,Size=40)]struct Input {[FieldOffset(0)]public uint Type;[FieldOffset(8)]public MouseInput Mouse;}
 [DllImport("user32.dll")]static extern uint SendInput(uint count,Input[] input,int bytes);
 public static bool ActivateOwnFixture(IntPtr w){var cls=new StringBuilder(128);GetClassName(w,cls,128);if(cls.ToString()!="MinimaDock.DesktopActionFixture")throw new Exception("Not an owned blank fixture.");if(SetForegroundWindow(w)&&GetForegroundWindow()==w)return true;SetWindowPos(w,new IntPtr(-1),0,0,0,0,19);Rect r;GetWindowRect(w,out r);var p=new Point();p.X=r.Left+40;p.Y=r.Top+80;SetCursorPos(p.X,p.Y);if(GetAncestor(WindowFromPoint(p),2)!=w)return false;var input=new Input[2];input[0].Mouse.Flags=2;input[1].Mouse.Flags=4;uint sent=SendInput(2,input,40);if(sent==1)SendInput(1,new Input[]{input[1]},40);SetWindowPos(w,new IntPtr(-2),0,0,0,0,19);return sent==2&&GetForegroundWindow()==w;}
 public static string State(IntPtr w){uint cloak;DwmGetWindowAttribute(w,14,out cloak,4);var cls=new StringBuilder(128);GetClassName(GetForegroundWindow(),cls,128);return "iconic="+IsIconic(w)+", visible="+IsWindowVisible(w)+", cloaked="+cloak+", style="+GetWindowLongPtr(w,-16).ToInt64().ToString("X")+", foreground="+cls;}
 public static bool Hidden(IntPtr w){uint cloak;DwmGetWindowAttribute(w,14,out cloak,4);return IsIconic(w)||!IsWindowVisible(w)||cloak!=0;}
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateWindowEx(uint ex,string cls,string title,uint style,int x,int y,int width,int height,IntPtr parent,IntPtr menu,IntPtr instance,IntPtr data);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr w,int state);
 [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr w);
 [StructLayout(LayoutKind.Sequential)] struct Message {public IntPtr Window;public uint Id;public UIntPtr W;public IntPtr L;public uint Time;public int X,Y;public uint Private;}
 [DllImport("user32.dll")] static extern int GetMessage(out Message m,IntPtr w,uint min,uint max);
 [DllImport("user32.dll")] static extern bool TranslateMessage(ref Message m);
 [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref Message m);
 [DllImport("user32.dll")] static extern bool PostThreadMessage(uint t,uint m,UIntPtr a,IntPtr b);
 [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
 static Thread fixtureThread;static uint fixtureThreadId;
 [DllImport("user32.dll",SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,IntPtr b,uint flags,uint timeout,out UIntPtr result);
 [DllImport("user32.dll")] static extern bool PostMessage(IntPtr w,uint m,IntPtr a,IntPtr b);
 public static IntPtr Find(uint pid){return FindClass(pid,"MinimaDock.DesktopDock");}
 public static IntPtr FindClass(uint pid,string cls){IntPtr result=IntPtr.Zero;EnumCallback callback=delegate(IntPtr w,IntPtr p){uint owner;GetWindowThreadProcessId(w,out owner);var name=new StringBuilder(128);GetClassName(w,name,128);if(owner==pid&&name.ToString()==cls){result=w;return false;}return true;};EnumWindows(callback,IntPtr.Zero);return result;}
 public static IntPtr Fixture(){IntPtr w=IntPtr.Zero;var ready=new ManualResetEvent(false);fixtureThread=new Thread(delegate(){fixtureThreadId=GetCurrentThreadId();var cls=new WindowClass();cls.Name="MinimaDock.DesktopActionFixture";cls.Proc=Marshal.GetFunctionPointerForDelegate(fixtureProc);RegisterClass(ref cls);w=CreateWindowEx(0x40000,cls.Name,"MinimaDock Show desktop test",0xCF0000,80,80,300,160,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero);if(w!=IntPtr.Zero)ShowWindow(w,4);ready.Set();if(w==IntPtr.Zero)return;Message m;while(GetMessage(out m,IntPtr.Zero,0,0)>0){TranslateMessage(ref m);DispatchMessage(ref m);}DestroyWindow(w);});fixtureThread.IsBackground=true;fixtureThread.SetApartmentState(ApartmentState.STA);fixtureThread.Start();if(!ready.WaitOne(5000)||w==IntPtr.Zero)throw new Exception("Could not create own fixture.");return w;}
 public static void CloseFixture(){if(fixtureThread!=null&&fixtureThread.IsAlive){PostThreadMessage(fixtureThreadId,0x12,UIntPtr.Zero,IntPtr.Zero);fixtureThread.Join(5000);}}
 public static long Send(IntPtr w,uint m,long a,long b){UIntPtr r;if(SendMessageTimeout(w,m,new IntPtr(a),new IntPtr(b),3,3000,out r)==IntPtr.Zero)throw new Exception("Own dock UI did not respond.");return unchecked((long)r.ToUInt64());}
 public static void ClickDesktop(IntPtr w){double scale=GetDpiForWindow(w)/96.0;int x=(int)Math.Round(Send(w,0x802e,0,0)*scale/1000.0);int y=(int)Math.Round(Send(w,0x802f,0,0)*scale/1000.0);if(!PostMessage(w,0x202,IntPtr.Zero,new IntPtr(((long)y<<16)|(uint)x)))throw new Exception("Could not post own button click.");long result=unchecked((int)0x8000000A);for(int i=0;i<60;i++){Thread.Sleep(50);result=Send(w,0x8030,0,0);if(result!=unchecked((int)0x8000000A))break;}if(result<0)throw new Exception("Desktop action HRESULT 0x"+unchecked((uint)result).ToString("X8")+" at own client point "+x+","+y);}
}
