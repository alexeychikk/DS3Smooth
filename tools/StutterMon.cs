// StutterMon - samples what the system and DarkSoulsIII.exe are doing, to line up with PresentMon frame
// times when hunting micro stutters.
//
//   StutterMon.exe <seconds> <out prefix>
//
// Writes <prefix>_100ms.csv (game CPU/IO/page faults, system CPU, worst-core DPC and interrupt time
// every 100 ms) and <prefix>_procs.csv (the five busiest other processes every second).
// C# 5 so it builds with the csc.exe in Windows (see build line in CLAUDE.md).

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

internal static class StutterMon
{
    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS { public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes; }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_MEMORY_COUNTERS
    {
        public uint cb, PageFaultCount;
        public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
            QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
    }

    // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION; the two "reserved" fields are DPC and interrupt time.
    [StructLayout(LayoutKind.Sequential)]
    private struct CPU_PERF { public long Idle, Kernel, User, Dpc, Interrupt; public uint InterruptCount; }

    [DllImport("kernel32.dll")] private static extern bool GetProcessIoCounters(IntPtr process, out IO_COUNTERS counters);
    [DllImport("psapi.dll")] private static extern bool GetProcessMemoryInfo(IntPtr process, out PROCESS_MEMORY_COUNTERS counters, uint size);
    [DllImport("kernel32.dll")] private static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
    [DllImport("ntdll.dll")] private static extern int NtQuerySystemInformation(int cls, [Out] CPU_PERF[] info, int size, out int returned);

    private static int Main(string[] args)
    {
        int seconds = args.Length > 0 ? int.Parse(args[0]) : 300;
        string prefix = args.Length > 1 ? args[1] : "stutter";
        Process[] found = Process.GetProcessesByName("DarkSoulsIII");
        if (found.Length == 0) { Console.WriteLine("DarkSoulsIII is not running."); return 1; }
        Process game = found[0];
        int cores = Environment.ProcessorCount;
        int perfSize = Marshal.SizeOf(typeof(CPU_PERF));

        using (StreamWriter fast = new StreamWriter(prefix + "_100ms.csv"))
        using (StreamWriter procs = new StreamWriter(prefix + "_procs.csv"))
        {
            fast.WriteLine("time,game_cpu_pct,game_read_mb,game_write_mb,game_page_faults,game_ws_mb,sys_cpu_pct,max_core_dpc_pct,max_core_int_pct,total_dpc_pct,total_int_pct");
            procs.WriteLine("time,name,pid,cpu_pct_of_one_core");

            Stopwatch clock = Stopwatch.StartNew();
            TimeSpan lastGameCpu = game.TotalProcessorTime;
            IO_COUNTERS lastIo; GetProcessIoCounters(game.Handle, out lastIo);
            PROCESS_MEMORY_COUNTERS lastMem; GetProcessMemoryInfo(game.Handle, out lastMem, (uint)Marshal.SizeOf(typeof(PROCESS_MEMORY_COUNTERS)));
            long lastIdle, lastKernel, lastUser; GetSystemTimes(out lastIdle, out lastKernel, out lastUser);
            CPU_PERF[] lastPerf = new CPU_PERF[cores]; int ret;
            NtQuerySystemInformation(8, lastPerf, perfSize * cores, out ret);
            double lastT = 0;
            Dictionary<int, TimeSpan> lastProc = SnapshotProcesses();
            double lastProcT = 0;

            while (clock.Elapsed.TotalSeconds < seconds && !game.HasExited)
            {
                Thread.Sleep(100);
                double t = clock.Elapsed.TotalSeconds, dt = t - lastT;
                string stamp = DateTime.Now.ToString("HH:mm:ss.fff");

                game.Refresh();
                TimeSpan gameCpu = game.TotalProcessorTime;
                IO_COUNTERS io; GetProcessIoCounters(game.Handle, out io);
                PROCESS_MEMORY_COUNTERS mem; GetProcessMemoryInfo(game.Handle, out mem, (uint)Marshal.SizeOf(typeof(PROCESS_MEMORY_COUNTERS)));
                long idle, kernel, user; GetSystemTimes(out idle, out kernel, out user);
                CPU_PERF[] perf = new CPU_PERF[cores];
                NtQuerySystemInformation(8, perf, perfSize * cores, out ret);

                double maxDpc = 0, maxInt = 0, sumDpc = 0, sumInt = 0;
                for (int c = 0; c < cores; c++)
                {
                    double wall = dt * 1e7; // 100 ns units
                    double dpc = (perf[c].Dpc - lastPerf[c].Dpc) / wall * 100, irq = (perf[c].Interrupt - lastPerf[c].Interrupt) / wall * 100;
                    maxDpc = Math.Max(maxDpc, dpc); maxInt = Math.Max(maxInt, irq); sumDpc += dpc; sumInt += irq;
                }
                long sysTotal = (kernel - lastKernel) + (user - lastUser);
                double sysCpu = sysTotal > 0 ? 100.0 * (sysTotal - (idle - lastIdle)) / sysTotal : 0;

                fast.WriteLine(string.Join(",", new string[] {
                    stamp,
                    F((gameCpu - lastGameCpu).TotalSeconds / dt * 100 / cores),
                    F((io.ReadBytes - lastIo.ReadBytes) / 1048576.0), F((io.WriteBytes - lastIo.WriteBytes) / 1048576.0),
                    (mem.PageFaultCount - lastMem.PageFaultCount).ToString(), F(mem.WorkingSetSize.ToUInt64() / 1048576.0),
                    F(sysCpu), F(maxDpc), F(maxInt), F(sumDpc / cores), F(sumInt / cores) }));

                lastGameCpu = gameCpu; lastIo = io; lastMem = mem; lastIdle = idle; lastKernel = kernel; lastUser = user; lastPerf = perf; lastT = t;

                if (t - lastProcT >= 1.0)
                {
                    Dictionary<int, TimeSpan> now = SnapshotProcesses();
                    double pdt = t - lastProcT;
                    var top = now.Where(p => p.Key != game.Id && lastProc.ContainsKey(p.Key))
                                 .Select(p => new { Pid = p.Key, Pct = (p.Value - lastProc[p.Key]).TotalSeconds / pdt * 100 })
                                 .OrderByDescending(p => p.Pct).Take(5);
                    foreach (var p in top)
                    {
                        string name;
                        try { name = Process.GetProcessById(p.Pid).ProcessName; } catch (Exception) { name = "?"; }
                        procs.WriteLine(stamp + "," + name + "," + p.Pid + "," + F(p.Pct));
                    }
                    lastProc = now; lastProcT = t;
                }
            }
        }
        Console.WriteLine("Done.");
        return 0;
    }

    private static Dictionary<int, TimeSpan> SnapshotProcesses()
    {
        Dictionary<int, TimeSpan> result = new Dictionary<int, TimeSpan>();
        foreach (Process p in Process.GetProcesses())
        {
            try { result[p.Id] = p.TotalProcessorTime; } catch (Exception) { }
            p.Dispose();
        }
        return result;
    }

    private static string F(double v) { return v.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture); }
}
