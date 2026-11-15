using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Threading;

// Synthetic pen/touch/mouse/keyboard input for UI tests (C# 5 for Windows PowerShell's Add-Type).
public static class Inj
{
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Explicit, Size = 96)]
    struct POINTER_INFO
    {
        [FieldOffset(0)] public int pointerType;
        [FieldOffset(4)] public uint pointerId;
        [FieldOffset(8)] public uint frameId;
        [FieldOffset(12)] public uint pointerFlags;
        [FieldOffset(16)] public IntPtr sourceDevice;
        [FieldOffset(24)] public IntPtr hwndTarget;
        [FieldOffset(32)] public POINT ptPixelLocation;
        [FieldOffset(40)] public POINT ptHimetricLocation;
        [FieldOffset(48)] public POINT ptPixelLocationRaw;
        [FieldOffset(56)] public POINT ptHimetricLocationRaw;
        [FieldOffset(64)] public uint dwTime;
        [FieldOffset(68)] public uint historyCount;
        [FieldOffset(72)] public int inputData;
        [FieldOffset(76)] public uint dwKeyStates;
        [FieldOffset(80)] public ulong performanceCount;
        [FieldOffset(88)] public int buttonChangeType;
    }

    [StructLayout(LayoutKind.Explicit, Size = 152)]
    struct PEN_TYPE_INFO
    {
        [FieldOffset(0)] public int type;
        [FieldOffset(8)] public POINTER_INFO info;
        [FieldOffset(104)] public uint penFlags;
        [FieldOffset(108)] public uint penMask;
        [FieldOffset(112)] public uint pressure;
        [FieldOffset(116)] public uint rotation;
        [FieldOffset(120)] public int tiltX;
        [FieldOffset(124)] public int tiltY;
    }

    [StructLayout(LayoutKind.Explicit, Size = 152)]
    struct TOUCH_TYPE_INFO
    {
        [FieldOffset(0)] public int type;
        [FieldOffset(8)] public POINTER_INFO info;
        [FieldOffset(104)] public uint touchFlags;
        [FieldOffset(108)] public uint touchMask;
        [FieldOffset(112)] public RECT rcContact;
        [FieldOffset(128)] public RECT rcContactRaw;
        [FieldOffset(144)] public uint orientation;
        [FieldOffset(148)] public uint pressure;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct INPUT { public int type; public INPUTUNION u; }
    [StructLayout(LayoutKind.Explicit)]
    struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)]
    struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; public uint pad1, pad2; }

    [DllImport("user32", SetLastError = true)] static extern IntPtr CreateSyntheticPointerDevice(int type, uint maxCount, int mode);
    [DllImport("user32", SetLastError = true)] static extern bool InjectSyntheticPointerInput(IntPtr dev, PEN_TYPE_INFO[] info, uint count);
    [DllImport("user32", SetLastError = true, EntryPoint = "InjectSyntheticPointerInput")] static extern bool InjectTouch(IntPtr dev, TOUCH_TYPE_INFO[] info, uint count);
    [DllImport("user32")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32")] static extern bool SetProcessDPIAware();
    [DllImport("user32")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32")] static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32")] static extern int GetSystemMetrics(int i);
    [DllImport("user32")] static extern bool GetClientRect(IntPtr h, out RECT r);
    public static double W, H;

    const uint INRANGE = 0x2, INCONTACT = 0x4, FIRSTBUTTON = 0x10, PRIMARY = 0x2000, CONFIDENCE = 0x4000,
        DOWN = 0x10000, UPDATE = 0x20000, UP = 0x40000;

    static IntPtr pen, touch;
    static IntPtr window;
    static double scale = 1;
    static POINT origin;

    public static string Init(IntPtr hwnd)
    {
        SetProcessDPIAware();
        window = hwnd;
        scale = GetDpiForWindow(hwnd) / 96.0;
        origin = new POINT();
        ClientToScreen(hwnd, ref origin);
        RECT cr; GetClientRect(hwnd, out cr);
        W = cr.Right / scale; H = cr.Bottom / scale;
        if (pen == IntPtr.Zero) pen = CreateSyntheticPointerDevice(3, 1, 1);
        if (touch == IntPtr.Zero) touch = CreateSyntheticPointerDevice(2, 2, 1);
        return "scale=" + scale + " origin=" + origin.X + "," + origin.Y + " pen=" + pen + " touch=" + touch +
            " client=" + W + "x" + H + " screen=" + GetSystemMetrics(0) + "x" + GetSystemMetrics(1);
    }

    // Client DIPs -> screen pixels.
    static POINT P(double x, double y)
    {
        var p = new POINT();
        p.X = origin.X + (int)Math.Round(x * scale);
        p.Y = origin.Y + (int)Math.Round(y * scale);
        return p;
    }

    static void Pen(POINT p, uint flags, uint penFlags, uint pressure)
    {
        var info = new PEN_TYPE_INFO();
        info.type = 3;
        info.info.pointerType = 3;
        info.info.pointerFlags = flags;
        info.info.ptPixelLocation = p;
        info.penFlags = penFlags;
        info.penMask = 1;
        info.pressure = pressure;
        if (!InjectSyntheticPointerInput(pen, new[] { info }, 1))
            throw new Exception("pen injection failed: " + Marshal.GetLastWin32Error());
        Thread.Sleep(8);
    }

    /// <summary>Pen stroke through client DIP points (x0,y0,x1,y1,...); eraser = eraser end of the pen.</summary>
    public static void PenStroke(double[] pts, bool eraser, uint pressure)
    {
        uint pf = eraser ? 0x6u : 0u; // INVERTED | ERASER
        var first = P(pts[0], pts[1]);
        Pen(first, INRANGE | UPDATE | PRIMARY, eraser ? 0x2u : 0u, 0);
        Pen(first, INRANGE | INCONTACT | DOWN | FIRSTBUTTON | PRIMARY, pf, pressure);
        for (int i = 2; i + 1 < pts.Length; i += 2)
        {
            double x0 = pts[i - 2], y0 = pts[i - 1], x1 = pts[i], y1 = pts[i + 1];
            for (int s = 1; s <= 8; s++)
            {
                Pen(P(x0 + (x1 - x0) * s / 8, y0 + (y1 - y0) * s / 8), INRANGE | INCONTACT | UPDATE | FIRSTBUTTON | PRIMARY, pf, pressure);
            }
        }
        var last = P(pts[pts.Length - 2], pts[pts.Length - 1]);
        Pen(last, INRANGE | UP | PRIMARY, eraser ? 0x2u : 0u, 0);
        Pen(last, UPDATE | PRIMARY, 0, 0);
    }

    static TOUCH_TYPE_INFO Contact(uint id, POINT p, uint flags)
    {
        var t = new TOUCH_TYPE_INFO();
        t.type = 2;
        t.info.pointerType = 2;
        t.info.pointerId = id;
        t.info.pointerFlags = flags;
        t.info.ptPixelLocation = p;
        t.touchMask = 0x7;
        t.rcContact.Left = p.X - 4; t.rcContact.Right = p.X + 4; t.rcContact.Top = p.Y - 4; t.rcContact.Bottom = p.Y + 4;
        t.orientation = 90;
        t.pressure = 32000;
        return t;
    }

    static void Touch(TOUCH_TYPE_INFO[] t)
    {
        if (!InjectTouch(touch, t, (uint)t.Length))
            throw new Exception("touch injection failed: " + Marshal.GetLastWin32Error());
        Thread.Sleep(10);
    }

    /// <summary>One-finger drag in client DIPs.</summary>
    public static void TouchDrag(double x0, double y0, double x1, double y1, int steps)
    {
        const uint F = PRIMARY | CONFIDENCE;
        Touch(new[] { Contact(0, P(x0, y0), F | INRANGE | INCONTACT | DOWN) });
        for (int s = 1; s <= steps; s++)
            Touch(new[] { Contact(0, P(x0 + (x1 - x0) * s / steps, y0 + (y1 - y0) * s / steps), F | INRANGE | INCONTACT | UPDATE) });
        Touch(new[] { Contact(0, P(x1, y1), F | UP) });
    }

    /// <summary>Two fingers around (cx, cy) moving from distance d0 to d1 (client DIPs).</summary>
    public static void Pinch(double cx, double cy, double d0, double d1, int steps)
    {
        for (int s = 0; s <= steps; s++)
        {
            double d = d0 + (d1 - d0) * s / steps;
            uint f = s == 0 ? INRANGE | INCONTACT | DOWN : INRANGE | INCONTACT | UPDATE;
            Touch(new[] { Contact(0, P(cx - d / 2, cy), f | PRIMARY | CONFIDENCE), Contact(1, P(cx + d / 2, cy), f | CONFIDENCE) });
        }
        double e = d1;
        Touch(new[] { Contact(0, P(cx - e / 2, cy), UP | PRIMARY | CONFIDENCE), Contact(1, P(cx + e / 2, cy), UP | CONFIDENCE) });
    }

    static void Mouse(uint flags, POINT p)
    {
        var i = new INPUT();
        i.type = 0;
        i.u.mi.dx = (int)(p.X * 65535.0 / (GetSystemMetrics(0) - 1));
        i.u.mi.dy = (int)(p.Y * 65535.0 / (GetSystemMetrics(1) - 1));
        i.u.mi.dwFlags = flags | 0x8000 | 0x1; // ABSOLUTE | MOVE
        SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
        Thread.Sleep(30);
    }

    public static void Click(double x, double y)
    {
        var p = P(x, y);
        Mouse(0, p);
        Mouse(0x2, p);
        Mouse(0x4, p);
    }

    static void KeyEvent(ushort vk, bool up)
    {
        var i = new INPUT();
        i.type = 1;
        i.u.ki.wVk = vk;
        i.u.ki.dwFlags = up ? 2u : 0u;
        SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
        Thread.Sleep(30);
    }

    public static void Ctrl(ushort vk)
    {
        KeyEvent(0x11, false); KeyEvent(vk, false); KeyEvent(vk, true); KeyEvent(0x11, true);
    }

    public static void Shot(string file)
    {
        int w = GetSystemMetrics(0), h = GetSystemMetrics(1);
        using (var bmp = new Bitmap(w, h))
        {
            using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(0, 0, 0, 0, new Size(w, h));
            bmp.Save(file, ImageFormat.Png);
        }
    }
}
