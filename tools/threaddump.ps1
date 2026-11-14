<#
 threaddump.ps1 - read-only snapshot of a 32-bit process: windows, modules, handles and for
 every thread its state, start address, suspend count, registers, what it waits for and a
 pseudo call stack (stack words that follow a call instruction, resolved to module!export+off).
 Used to see where a hung or zombie Game.exe is stuck. Nothing in the target is changed
 (threads are suspended only for the moment their stack is copied, then resumed).

 Usage:
   powershell -ExecutionPolicy Bypass -File tools\threaddump.ps1 [-Name Game] [-ProcId N]
              [-Out file] [-Elevate] [-Mem 0x006C1230:24,0x006C12C8:24] [-StackBytes 24576] [-Raw 64]

 A game started the normal way runs elevated (RUNASADMIN compatibility flag); a plain shell
 cannot open it. -Elevate re-runs this script through a UAC prompt in that case.
 Output goes to tools\out\threaddump_<pid>_<time>.txt unless -Out is given.
#>
param(
    [string]$Name = 'Game',
    [int]$ProcId = 0,
    [string]$Out = '',
    [switch]$Elevate,
    [string]$Mem = '',
    [int]$StackBytes = 24576,
    [int]$Raw = 64
)

$src = @'
using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

public static class ThreadDump
{
    const uint PROCESS_VM_READ = 0x0010, PROCESS_QUERY_INFORMATION = 0x0400, PROCESS_DUP_HANDLE = 0x0040;
    const uint THREAD_SUSPEND_RESUME = 0x0002, THREAD_GET_CONTEXT = 0x0008, THREAD_QUERY_INFORMATION = 0x0040;

    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenThread(uint access, bool inherit, int tid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern int Wow64SuspendThread(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern int ResumeThread(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool Wow64GetThreadContext(IntPtr h, IntPtr ctx);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, [Out] byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool IsWow64Process(IntPtr h, out bool wow);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool DuplicateHandle(IntPtr srcProc, IntPtr src, IntPtr dstProc, out IntPtr dst, uint access, bool inherit, uint options);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)] static extern int GetThreadId(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern int GetProcessIdOfThread(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern int GetProcessId(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetThreadTimes(IntPtr h, out long c, out long e, out long k, out long u);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeThread(IntPtr h, out uint code);
    [DllImport("psapi.dll", SetLastError = true)] static extern bool EnumProcessModulesEx(IntPtr h, [Out] IntPtr[] mods, int cb, out int needed, uint filter);
    [DllImport("psapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern uint GetModuleFileNameExW(IntPtr h, IntPtr mod, StringBuilder sb, int n);
    [DllImport("psapi.dll", SetLastError = true)] static extern bool GetModuleInformation(IntPtr h, IntPtr mod, out MODULEINFO mi, int cb);
    [DllImport("ntdll.dll")] static extern int NtQueryInformationThread(IntPtr h, int cls, out IntPtr info, int len, IntPtr retLen);
    [DllImport("ntdll.dll")] static extern int NtQueryObject(IntPtr h, int cls, IntPtr buf, int len, out int retLen);

    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsHungAppWindow(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int InternalGetWindowText(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern int GetWindowLongW(IntPtr h, int idx);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();

    [StructLayout(LayoutKind.Sequential)] struct MODULEINFO { public IntPtr BaseOfDll; public uint SizeOfImage; public IntPtr EntryPoint; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    class Mod
    {
        public uint Base; public uint Size; public string Path; public string Name; public bool IsExe;
        public uint[] ExpRva; public string[] ExpName; public bool ExpLoaded;
    }

    static IntPtr hProc;
    static List<Mod> mods = new List<Mod>();
    static Dictionary<uint, byte[]> pages = new Dictionary<uint, byte[]>();
    static Dictionary<uint, string> handles = new Dictionary<uint, string>();
    static StringBuilder o = new StringBuilder();

    static byte[] Read(uint addr, int size)
    {
        if (size <= 0) return null;
        byte[] buf = new byte[size];
        IntPtr got;
        if (!ReadProcessMemory(hProc, new IntPtr((long)addr), buf, new IntPtr(size), out got) || got.ToInt64() != size) return null;
        return buf;
    }

    static byte[] Page(uint addr)
    {
        uint key = addr & 0xFFFFF000u;
        byte[] p;
        if (pages.TryGetValue(key, out p)) return p;
        p = Read(key, 0x1000);
        pages[key] = p;
        return p;
    }

    static int ByteAt(uint addr) { byte[] p = Page(addr); return p == null ? -1 : p[addr & 0xFFF]; }

    static Mod FindMod(uint addr)
    {
        foreach (Mod m in mods) if (addr >= m.Base && addr - m.Base < m.Size) return m;
        return null;
    }

    static bool AfterCall(uint a)
    {
        int b1 = ByteAt(a - 1), b2 = ByteAt(a - 2), b3 = ByteAt(a - 3), b4 = ByteAt(a - 4), b5 = ByteAt(a - 5), b6 = ByteAt(a - 6), b7 = ByteAt(a - 7);
        if (b1 < 0) return false;
        if (b5 == 0xE8) return true;                                               // call rel32
        if (b2 == 0xFF && ((b1 >> 3) & 7) == 2) return true;                       // call reg / call [reg]
        if (b3 == 0xFF && ((b2 >> 3) & 7) == 2 && (b2 >> 6) != 3) return true;     // call [reg+d8] / call [sib]
        if (b4 == 0xFF && ((b3 >> 3) & 7) == 2 && (b3 & 7) == 4) return true;      // call [sib+d8]
        if (b6 == 0xFF && ((b5 >> 3) & 7) == 2) return true;                       // call [d32] / call [reg+d32]
        if (b7 == 0xFF && ((b6 >> 3) & 7) == 2 && (b6 & 7) == 4) return true;      // call [sib+d32]
        return false;
    }

    static void LoadExports(Mod m)
    {
        m.ExpLoaded = true;
        try
        {
            string path = m.Path;
            if (path == null || !File.Exists(path)) return;
            byte[] f = File.ReadAllBytes(path);
            int pe = BitConverter.ToInt32(f, 0x3C);
            if (pe <= 0 || pe + 0x100 > f.Length || f[pe] != 0x50 || f[pe + 1] != 0x45) return;
            int nsec = BitConverter.ToUInt16(f, pe + 6);
            int optSize = BitConverter.ToUInt16(f, pe + 20);
            int opt = pe + 24;
            ushort magic = BitConverter.ToUInt16(f, opt);
            int ddir = opt + (magic == 0x20B ? 112 : 96);
            uint expRva = BitConverter.ToUInt32(f, ddir);
            uint expSize = BitConverter.ToUInt32(f, ddir + 4);
            if (expRva == 0) return;
            int sec = opt + optSize;
            Func<uint, int> off = delegate(uint rva)
            {
                for (int i = 0; i < nsec; i++)
                {
                    int s = sec + i * 40;
                    uint vs = BitConverter.ToUInt32(f, s + 8);
                    uint va = BitConverter.ToUInt32(f, s + 12);
                    uint rs = BitConverter.ToUInt32(f, s + 16);
                    uint rp = BitConverter.ToUInt32(f, s + 20);
                    uint span = Math.Max(vs, rs);
                    if (rva >= va && rva - va < span)
                    {
                        uint o2 = rva - va + rp;
                        return o2 < (uint)f.Length ? (int)o2 : -1;
                    }
                }
                return -1;
            };
            int e = off(expRva);
            if (e < 0) return;
            int nFuncs = BitConverter.ToInt32(f, e + 20);
            int nNames = BitConverter.ToInt32(f, e + 24);
            int aFuncs = off(BitConverter.ToUInt32(f, e + 28));
            int aNames = off(BitConverter.ToUInt32(f, e + 32));
            int aOrds = off(BitConverter.ToUInt32(f, e + 36));
            if (aFuncs < 0 || aNames < 0 || aOrds < 0) return;
            List<KeyValuePair<uint, string>> list = new List<KeyValuePair<uint, string>>();
            for (int i = 0; i < nNames; i++)
            {
                int no = off(BitConverter.ToUInt32(f, aNames + 4 * i));
                int ord = BitConverter.ToUInt16(f, aOrds + 2 * i);
                if (no < 0 || ord >= nFuncs) continue;
                uint frva = BitConverter.ToUInt32(f, aFuncs + 4 * ord);
                if (frva >= expRva && frva - expRva < expSize) continue; // forwarder
                int end = no;
                while (end < f.Length && f[end] != 0) end++;
                list.Add(new KeyValuePair<uint, string>(frva, Encoding.ASCII.GetString(f, no, end - no)));
            }
            list.Sort(delegate(KeyValuePair<uint, string> a, KeyValuePair<uint, string> b)
            {
                int c = a.Key.CompareTo(b.Key);
                return c != 0 ? c : String.CompareOrdinal(a.Value, b.Value);
            });
            m.ExpRva = new uint[list.Count];
            m.ExpName = new string[list.Count];
            for (int i = 0; i < list.Count; i++) { m.ExpRva[i] = list[i].Key; m.ExpName[i] = list[i].Value; }
        }
        catch { }
    }

    static string Sym(uint addr)
    {
        Mod m = FindMod(addr);
        if (m == null) return null;
        uint rva = addr - m.Base;
        if (m.IsExe) return String.Format("{0}:{1:X8}", m.Name, addr);
        if (!m.ExpLoaded) LoadExports(m);
        if (m.ExpRva != null && m.ExpRva.Length > 0)
        {
            int lo = 0, hi = m.ExpRva.Length - 1, best = -1;
            while (lo <= hi)
            {
                int mid = (lo + hi) / 2;
                if (m.ExpRva[mid] <= rva) { best = mid; lo = mid + 1; } else hi = mid - 1;
            }
            // several names can share one address (Nt*/Zw*): take the first of the group
            while (best > 0 && m.ExpRva[best - 1] == m.ExpRva[best]) best--;
            if (best >= 0 && rva - m.ExpRva[best] < 0x2000)
                return String.Format("{0}!{1}+0x{2:X} [+0x{3:X}]", m.Name, m.ExpName[best], rva - m.ExpRva[best], rva);
        }
        return String.Format("{0}+0x{1:X}", m.Name, rva);
    }

    static string SymOrHex(uint addr) { string s = Sym(addr); return s != null ? s : String.Format("{0:X8}", addr); }

    static string HandleDesc(uint h)
    {
        string d;
        return handles.TryGetValue(h, out d) ? d : "?";
    }

    static void ScanHandles(int pid)
    {
        IntPtr self = GetCurrentProcess();
        IntPtr buf = Marshal.AllocHGlobal(0x1000);
        try
        {
            for (uint hv = 4; hv < 0x4000; hv += 4)
            {
                IntPtr dup;
                if (!DuplicateHandle(hProc, new IntPtr((long)hv), self, out dup, 0, false, 2)) continue;
                string type = "?";
                int ret;
                if (NtQueryObject(dup, 2, buf, 0x1000, out ret) >= 0)
                {
                    int len = (ushort)Marshal.ReadInt16(buf, 0);
                    IntPtr str = Marshal.ReadIntPtr(buf, IntPtr.Size);
                    if (str != IntPtr.Zero && len > 0) type = Marshal.PtrToStringUni(str, len / 2);
                }
                string extra = "";
                if (type == "Thread")
                {
                    int tid = GetThreadId(dup);
                    int tpid = GetProcessIdOfThread(dup);
                    uint code = 0;
                    GetExitCodeThread(dup, out code);
                    extra = String.Format(" tid={0}{1}{2}", tid, tpid == pid ? "" : " (pid " + tpid + ")",
                                          code == 259 ? "" : String.Format(" EXITED code=0x{0:X}", code));
                }
                else if (type == "Process")
                {
                    extra = " pid=" + GetProcessId(dup);
                }
                handles[hv] = type + extra;
                CloseHandle(dup);
            }
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    static void DumpWindows(int pid)
    {
        o.AppendFormat("=== windows (foreground hwnd=0x{0:X})\r\n", GetForegroundWindow().ToInt64());
        int n = 0;
        EnumWindows(delegate(IntPtr h, IntPtr l)
        {
            uint p;
            uint tid = GetWindowThreadProcessId(h, out p);
            if (p == (uint)pid)
            {
                StringBuilder c = new StringBuilder(256); GetClassNameW(h, c, 256);
                StringBuilder t = new StringBuilder(256); InternalGetWindowText(h, t, 256);
                RECT r; GetWindowRect(h, out r);
                o.AppendFormat("  hwnd=0x{0:X} tid={1} class='{2}' title='{3}' visible={4} iconic={5} hung={6} rect=({7},{8})-({9},{10}) style=0x{11:X8} ex=0x{12:X8}\r\n",
                    h.ToInt64(), tid, c, t, IsWindowVisible(h), IsIconic(h), IsHungAppWindow(h), r.L, r.T, r.R, r.B, GetWindowLongW(h, -16), GetWindowLongW(h, -20));
                n++;
            }
            return true;
        }, IntPtr.Zero);
        if (n == 0) o.Append("  (none)\r\n");
    }

    static void DumpThread(ProcessThread pt, int stackBytes, int raw)
    {
        int tid = pt.Id;
        string state = pt.ThreadState.ToString();
        string wait = "";
        try { if (pt.ThreadState == System.Diagnostics.ThreadState.Wait) wait = "/" + pt.WaitReason.ToString(); } catch { }

        IntPtr h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, false, tid);
        if (h == IntPtr.Zero)
        {
            o.AppendFormat("\r\n--- TID {0} {1}{2}: OpenThread failed, error {3}\r\n", tid, state, wait, Marshal.GetLastWin32Error());
            return;
        }
        IntPtr start = IntPtr.Zero;
        NtQueryInformationThread(h, 9, out start, IntPtr.Size, IntPtr.Zero);
        long tc, te, tk, tu;
        double kMs = -1, uMs = -1;
        string created = "?";
        if (GetThreadTimes(h, out tc, out te, out tk, out tu))
        {
            kMs = tk / 10000.0; uMs = tu / 10000.0;
            try { created = DateTime.FromFileTime(tc).ToString("HH:mm:ss.fff"); } catch { }
        }

        int prev = Wow64SuspendThread(h);
        IntPtr ctx = Marshal.AllocHGlobal(716);
        for (int i = 0; i < 716; i += 4) Marshal.WriteInt32(ctx, i, 0);
        Marshal.WriteInt32(ctx, 0, 0x00010007);
        bool ok = Wow64GetThreadContext(h, ctx);
        int ctxErr = ok ? 0 : Marshal.GetLastWin32Error();
        uint eip = (uint)Marshal.ReadInt32(ctx, 0xB8), esp = (uint)Marshal.ReadInt32(ctx, 0xC4), ebp = (uint)Marshal.ReadInt32(ctx, 0xB4);
        uint eax = (uint)Marshal.ReadInt32(ctx, 0xB0), ecx = (uint)Marshal.ReadInt32(ctx, 0xAC), edx = (uint)Marshal.ReadInt32(ctx, 0xA8);
        uint ebx = (uint)Marshal.ReadInt32(ctx, 0xA4), esi = (uint)Marshal.ReadInt32(ctx, 0xA0), edi = (uint)Marshal.ReadInt32(ctx, 0x9C);
        Marshal.FreeHGlobal(ctx);

        List<uint> words = new List<uint>();
        if (ok)
        {
            uint p = esp & ~3u;
            int remaining = stackBytes;
            while (remaining > 0)
            {
                int chunk = (int)Math.Min((uint)remaining, 0x1000 - (p & 0xFFF));
                byte[] b = Read(p, chunk);
                if (b == null) break;
                for (int i = 0; i + 4 <= b.Length; i += 4) words.Add(BitConverter.ToUInt32(b, i));
                p += (uint)chunk;
                remaining -= chunk;
            }
        }
        if (prev != -1) ResumeThread(h);
        CloseHandle(h);

        o.AppendFormat("\r\n--- TID {0}  {1}{2}  suspendCount(before)={3}  created={4}  cpu kernel={5:F0}ms user={6:F0}ms\r\n",
            tid, state, wait, prev, created, kMs, uMs);
        o.AppendFormat("    start = {0}\r\n", SymOrHex((uint)start.ToInt64()));
        if (!ok) { o.AppendFormat("    GetThreadContext failed, error {0}\r\n", ctxErr); return; }
        string es = SymOrHex(eip);
        o.AppendFormat("    EIP = {0}\r\n", es);
        o.AppendFormat("    ESP={0:X8} EBP={1:X8} EAX={2:X8} EBX={3:X8} ECX={4:X8} EDX={5:X8} ESI={6:X8} EDI={7:X8}\r\n", esp, ebp, eax, ebx, ecx, edx, esi, edi);

        // what is it waiting for (32-bit syscall stubs: [esp]=return, then the arguments)
        if (words.Count >= 6)
        {
            if (es.Contains("!NtUserMsgWaitForMultipleObjectsEx+"))
            {
                uint count = words[1];
                o.AppendFormat("    WAIT msg+objects: count={0}, timeout={1}, wakeMask=0x{2:X}, flags=0x{3:X}\r\n",
                    count, words[3] == 0xFFFFFFFFu ? "INFINITE" : words[3] + " ms", words[4], words[5]);
                if (count > 0 && count <= 64)
                {
                    byte[] hb = Read(words[2], (int)count * 4);
                    if (hb != null)
                        for (int i = 0; i < (int)count; i++)
                        {
                            uint hv = BitConverter.ToUInt32(hb, i * 4);
                            o.AppendFormat("        handle 0x{0:X} = {1}\r\n", hv, HandleDesc(hv));
                        }
                }
            }
            else if (es.Contains("!NtWaitForSingleObject+") || es.Contains("!ZwWaitForSingleObject+"))
            {
                string tmo = "INFINITE";
                if (words[3] != 0) { byte[] t = Read(words[3], 8); if (t != null) tmo = String.Format("{0:F1} ms", -BitConverter.ToInt64(t, 0) / 10000.0); }
                o.AppendFormat("    WAIT single: handle 0x{0:X} = {1}, alertable={2}, timeout={3}\r\n", words[1], HandleDesc(words[1]), words[2], tmo);
            }
            else if (es.Contains("!NtWaitForMultipleObjects+") || es.Contains("!ZwWaitForMultipleObjects+"))
            {
                uint count = words[1];
                string tmo = "INFINITE";
                if (words[5] != 0) { byte[] t = Read(words[5], 8); if (t != null) tmo = String.Format("{0:F1} ms", -BitConverter.ToInt64(t, 0) / 10000.0); }
                o.AppendFormat("    WAIT multiple: count={0}, type={1} (0=all,1=any), alertable={2}, timeout={3}\r\n", count, words[3], words[4], tmo);
                if (count > 0 && count <= 64)
                {
                    byte[] hb = Read(words[2], (int)count * 4);
                    if (hb != null)
                        for (int i = 0; i < (int)count; i++)
                        {
                            uint hv = BitConverter.ToUInt32(hb, i * 4);
                            o.AppendFormat("        handle 0x{0:X} = {1}\r\n", hv, HandleDesc(hv));
                        }
                }
            }
            else if (es.Contains("!NtDelayExecution+") || es.Contains("!ZwDelayExecution+"))
            {
                string tmo = "?";
                if (words[2] != 0) { byte[] t = Read(words[2], 8); if (t != null) tmo = String.Format("{0:F1} ms", -BitConverter.ToInt64(t, 0) / 10000.0); }
                o.AppendFormat("    SLEEP: interval={0}, alertable={1}\r\n", tmo, words[1]);
            }
        }

        o.Append("    calls (stack words that follow a call instruction; may include stale ones):\r\n");
        int shown = 0;
        for (int i = 0; i < words.Count && shown < 60; i++)
        {
            uint w = words[i];
            if (w < 0x10000) continue;
            if (FindMod(w) == null) continue;
            if (!AfterCall(w)) continue;
            o.AppendFormat("      [esp+{0:X4}] {1}\r\n", i * 4, Sym(w));
            shown++;
        }
        if (raw > 0)
        {
            o.Append("    raw stack:");
            for (int i = 0; i < words.Count && i < raw; i++)
            {
                if ((i % 8) == 0) o.AppendFormat("\r\n      +{0:X3}:", i * 4);
                o.AppendFormat(" {0:X8}", words[i]);
            }
            o.Append("\r\n");
        }
    }

    public static string Run(int pid, int stackBytes, int raw, string memSpec)
    {
        o.Length = 0; mods.Clear(); pages.Clear(); handles.Clear();
        hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, false, pid);
        if (hProc == IntPtr.Zero) return "OPENFAIL " + Marshal.GetLastWin32Error();

        Process proc = Process.GetProcessById(pid);
        bool wow = false;
        IsWow64Process(hProc, out wow);
        o.AppendFormat("=== thread dump of pid {0} ({1}) at {2:yyyy-MM-dd HH:mm:ss}, wow64={3}, threads={4}\r\n",
            pid, proc.ProcessName, DateTime.Now, wow, proc.Threads.Count);
        try { o.AppendFormat("    started {0:yyyy-MM-dd HH:mm:ss}, total cpu {1:F1} s\r\n", proc.StartTime, proc.TotalProcessorTime.TotalSeconds); } catch { }

        DumpWindows(pid);

        IntPtr[] arr = new IntPtr[1024];
        int needed;
        if (!EnumProcessModulesEx(hProc, arr, arr.Length * IntPtr.Size, out needed, wow ? 1u : 3u)) needed = 0;
        int count = Math.Min(needed / IntPtr.Size, arr.Length);
        string exeDir = null;
        o.AppendFormat("=== modules ({0})\r\n", count);
        for (int i = 0; i < count; i++)
        {
            MODULEINFO mi;
            if (!GetModuleInformation(hProc, arr[i], out mi, Marshal.SizeOf(typeof(MODULEINFO)))) continue;
            StringBuilder sb = new StringBuilder(520);
            GetModuleFileNameExW(hProc, arr[i], sb, 520);
            Mod m = new Mod();
            m.Base = (uint)mi.BaseOfDll.ToInt64();
            m.Size = mi.SizeOfImage;
            m.Path = sb.ToString();
            m.Name = System.IO.Path.GetFileName(m.Path);
            m.IsExe = (i == 0);
            if (i == 0) exeDir = System.IO.Path.GetDirectoryName(m.Path);
            else if (exeDir != null && String.Equals(System.IO.Path.GetDirectoryName(m.Path), exeDir, StringComparison.OrdinalIgnoreCase))
                m.Name = m.Name + "(game dir)";
            if (wow)
            {
                // 32-bit module paths are reported with System32; on disk that is SysWOW64
                int idx = m.Path.IndexOf("\\system32\\", StringComparison.OrdinalIgnoreCase);
                if (idx >= 0)
                {
                    string alt = m.Path.Substring(0, idx) + "\\SysWOW64\\" + m.Path.Substring(idx + 10);
                    if (File.Exists(alt)) m.Path = alt;
                }
            }
            mods.Add(m);
            o.AppendFormat("  {0:X8}-{1:X8} {2}  ({3})\r\n", m.Base, m.Base + m.Size, m.Name, m.Path);
        }

        ScanHandles(pid);
        o.AppendFormat("=== handles ({0})\r\n", handles.Count);
        List<uint> hk = new List<uint>(handles.Keys);
        hk.Sort();
        Dictionary<string, int> typeCount = new Dictionary<string, int>();
        foreach (uint k in hk)
        {
            string d = handles[k];
            string type = d.Split(' ')[0];
            int c; typeCount.TryGetValue(type, out c); typeCount[type] = c + 1;
            if (type == "Thread" || type == "Process" || type == "Mutant")
                o.AppendFormat("  0x{0:X} {1}\r\n", k, d);
        }
        o.Append("  by type:");
        foreach (KeyValuePair<string, int> kv in typeCount) o.AppendFormat(" {0}={1}", kv.Key, kv.Value);
        o.Append("\r\n");

        if (!String.IsNullOrEmpty(memSpec))
        {
            o.Append("=== memory\r\n");
            foreach (string part in memSpec.Split(','))
            {
                string[] kv = part.Trim().Split(':');
                if (kv.Length != 2) continue;
                try
                {
                    uint addr = Convert.ToUInt32(kv[0].Trim(), 16);
                    int size = Convert.ToInt32(kv[1].Trim(), 10);
                    byte[] b = Read(addr, size);
                    o.AppendFormat("  {0:X8} [{1}]:", addr, size);
                    if (b == null) { o.Append(" <unreadable>\r\n"); continue; }
                    for (int i = 0; i + 4 <= b.Length; i += 4) o.AppendFormat(" {0:X8}", BitConverter.ToUInt32(b, i));
                    for (int i = b.Length & ~3; i < b.Length; i++) o.AppendFormat(" {0:X2}", b[i]);
                    o.Append("\r\n");
                }
                catch (Exception ex) { o.AppendFormat("  bad -Mem item '{0}': {1}\r\n", part, ex.Message); }
            }
        }

        List<ProcessThread> threads = new List<ProcessThread>();
        foreach (ProcessThread pt in proc.Threads) threads.Add(pt);
        threads.Sort(delegate(ProcessThread a, ProcessThread b) { return a.Id.CompareTo(b.Id); });
        o.Append("=== threads\r\n");
        foreach (ProcessThread pt in threads)
        {
            try { DumpThread(pt, stackBytes, raw); }
            catch (Exception ex) { o.AppendFormat("\r\n--- TID {0}: {1}\r\n", pt.Id, ex.Message); }
        }
        CloseHandle(hProc);
        return o.ToString();
    }
}
'@

if (-not $ProcId) {
    $p = Get-Process -Name $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { Write-Output "No process named '$Name'."; exit 1 }
    $ProcId = $p.Id
}
if (-not $Out) {
    $outDir = Join-Path $PSScriptRoot 'out'
    if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
    $Out = Join-Path $outDir ("threaddump_{0}_{1}.txt" -f $ProcId, (Get-Date -Format 'yyyyMMdd_HHmmss'))
}

Add-Type -TypeDefinition $src -Language CSharp
$text = [ThreadDump]::Run($ProcId, $StackBytes, $Raw, $Mem)

if ($text -like 'OPENFAIL*') {
    if ($text -eq 'OPENFAIL 5' -and $Elevate) {
        $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -ProcId $ProcId -Out `"$Out`" -StackBytes $StackBytes -Raw $Raw"
        if ($Mem) { $argLine += " -Mem `"$Mem`"" }
        $ps = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
        try {
            Start-Process -FilePath $ps -ArgumentList $argLine -Verb RunAs -Wait -WindowStyle Hidden
        } catch {
            Write-Output "Elevation was not granted: $($_.Exception.Message)"
            exit 3
        }
        if (Test-Path $Out) { Write-Output "written (elevated): $Out" } else { Write-Output "Elevated run produced no output." }
        exit 0
    }
    Write-Output "Cannot open process $ProcId ($text). If it runs elevated, add -Elevate."
    exit 2
}

[IO.File]::WriteAllText($Out, $text, (New-Object Text.UTF8Encoding($false)))
Write-Output "written: $Out"
