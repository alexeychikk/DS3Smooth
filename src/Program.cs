// DS3Smooth - FPS unlock, device stutter fix, sprint fix and No Logo for Dark Souls III.
//
// Extracted from SilkySouls3 (https://github.com/borgCode/SilkySouls3, MIT, (c) Shilkey).
// Only three features are kept:
//   1. FPS unlock: sets the game's built-in debug frame limiter (SprjFlipper +0x354 / +0x358).
//   2. Stutter fix: sets UserInputManager +0x24B so the game stops freezing when Windows
//      reports a USB device change ("device stutter").
//   3. No Logo: a 20-byte code patch that skips the startup logo screens.
// Borderless: removes the title bar from the game's window and stretches it over the monitor
// (a window-style change only; nothing in the game's memory).
// Plus, from DS3DebugFPS (https://github.com/0dm/DS3DebugFPS, Apache-2.0):
//   4. Sprint fix: the game's "am I stuck?" running check and its slowdown/recovery
//      rates are tuned for 60 FPS, which makes sprinting slower above ~82 FPS. Those three
//      constants are redirected to values recalculated from the real frame time.
// Settings come from DS3Smooth.ini in the game folder, the one file shared with dinput8.dll.
//
// Written in C# 5 on purpose so it compiles with the csc.exe that ships with Windows 11
// (.NET Framework 4.x). No SDK, NuGet or Visual Studio needed.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

internal static class Program
{
    // ---- Offsets (static pointers relative to DarkSoulsIII.exe), copied from SilkySouls3 ----

    private const int FlipperFps = 0x354;          // float: debug FPS cap
    private const int FlipperUseDebugFps = 0x358;  // byte:  1 = use the debug FPS cap
    private const int InputSteamInputFlag = 0x24B; // byte:  1 = stutter fix on

    // No Logo patch bytes, from SilkySouls3:
    //   xor rax,rax / mov [rdx],rax / mov [r12],rax / 10x nop
    private static readonly byte[] NoLogoPatch =
    {
        0x48, 0x31, 0xC0, 0x48, 0x89, 0x02, 0x49, 0x89, 0x04, 0x24,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };

    // Sprint fix: the running-speed "stuck" check (distance * 30.0 < 1), from DS3DebugFPS.
    //   addss / shufps / sqrtss / mulss xmm,[rip+30.0] / comiss
    private static readonly byte[] SpeedPattern =
    {
        0xF3, 0x0F, 0x58, 0x00, 0x0F, 0xC6, 0x00, 0x00, 0x0F, 0x51, 0x00,
        0xF3, 0x0F, 0x59, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x2F
    };
    private const string SpeedMask = "xxx?xx?xxx?xxxx????xx";
    private static readonly byte[] MulssRip = { 0xF3, 0x0F, 0x59, 0x05 }; // mulss xmm0,[rip+disp32]
    private const int FlipperFrameTime = 0x26C; // float: last frame time in seconds

    private sealed class GameOffsets
    {
        public readonly long SprjFlipper;
        public readonly long UserInputManager;
        public readonly long NoLogo;
        public GameOffsets(long sprjFlipper, long userInputManager, long noLogo)
        {
            SprjFlipper = sprjFlipper;
            UserInputManager = userInputManager;
            NoLogo = noLogo;
        }
    }

    // Key = start of the exe's FileVersion (e.g. "1.15.2.")
    private static readonly Dictionary<string, GameOffsets> Versions = new Dictionary<string, GameOffsets>
    {
        { "1.3.2.",  new GameOffsets(0x47E3940, 0x48A9968, 0xBBAFDF) },
        { "1.4.1.",  new GameOffsets(0x47E4C60, 0x48AAC70, 0xBBB0CF) },
        { "1.4.2.",  new GameOffsets(0x47E4C60, 0x48AAC70, 0xBBB0CF) },
        { "1.4.3.",  new GameOffsets(0x47E4C60, 0x48AAC70, 0xBBB0CF) },
        { "1.5.0.",  new GameOffsets(0x47E8D70, 0x48AED80, 0xBBBF2F) },
        { "1.5.1.",  new GameOffsets(0x47E7D70, 0x48ADD80, 0xBBBD5F) },
        { "1.6.0.",  new GameOffsets(0x47E8DD0, 0x48AEDF0, 0xBBC32F) },
        { "1.7.0.",  new GameOffsets(0x47ED650, 0x48B3670, 0xBBEA5F) },
        { "1.8.0.",  new GameOffsets(0x484BFD0, 0x49127F0, 0xBD6ACF) },
        { "1.9.0.",  new GameOffsets(0x484C110, 0x4912930, 0xBD708F) },
        { "1.10.0.", new GameOffsets(0x484C110, 0x4912930, 0xBD70FF) },
        { "1.11.0.", new GameOffsets(0x487F930, 0x4945EC8, 0xBE6F8F) },
        { "1.12.0.", new GameOffsets(0x4882AC0, 0x4949058, 0xBE7D9F) },
        { "1.13.0.", new GameOffsets(0x48862E0, 0x494C878, 0xBE993F) },
        { "1.14.0.", new GameOffsets(0x4888440, 0x494E9D8, 0xBE9C0F) },
        { "1.15.0.", new GameOffsets(0x4888440, 0x494E9D8, 0xBE9D0F) },
        { "1.15.1.", new GameOffsets(0x489DD20, 0x49644C8, 0xBF42BF) },
        { "1.15.2.", new GameOffsets(0x489DD10, 0x49644B8, 0xBF43EF) },
    };

    // ---- Win32 ----

    private const int PROCESS_VM_READ = 0x0010;
    private const int PROCESS_VM_WRITE = 0x0020;
    private const int PROCESS_VM_OPERATION = 0x0008;
    private const int PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(int access, bool inheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr written);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualProtectEx(IntPtr process, IntPtr address, IntPtr size, int newProtect, out int oldProtect);

    [DllImport("kernel32.dll")]
    private static extern bool FlushInstructionCache(IntPtr process, IntPtr address, IntPtr size);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll")]
    private static extern bool GetExitCodeProcess(IntPtr process, out int exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, IntPtr size, int allocationType, int protect);

    private const int PAGE_EXECUTE_READWRITE = 0x40;
    private const int PAGE_READWRITE = 0x04;
    private const int MEM_COMMIT = 0x1000;
    private const int MEM_RESERVE = 0x2000;

    // Borderless window
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public int dwFlags; }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern int GetWindowThreadProcessId(IntPtr hwnd, out int processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hwnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int maxCount);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetWindowLongPtrW(IntPtr hwnd, int index);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowLongPtrW(IntPtr hwnd, int index, IntPtr value);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter, int x, int y, int cx, int cy, int flags);

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, int flags);

    [DllImport("user32.dll")]
    private static extern bool GetMonitorInfoW(IntPtr monitor, ref MONITORINFO info);

    [DllImport("user32.dll")]
    private static extern bool SetProcessDpiAwarenessContext(IntPtr context);

    [DllImport("user32.dll")]
    private static extern bool SetProcessDPIAware();

    private const int GWL_STYLE = -16;
    private const int GWL_EXSTYLE = -20;
    private const long WS_POPUP = 0x80000000L;
    private const long WS_CAPTION = 0x00C00000L;     // title bar + thin border
    private const long WS_THICKFRAME = 0x00040000L;
    private const long WS_SYSMENU = 0x00080000L;
    private const long WS_MINIMIZEBOX = 0x00020000L;
    private const long WS_MAXIMIZEBOX = 0x00010000L;
    private const long WS_EX_DLGMODALFRAME = 0x0001L;
    private const long WS_EX_WINDOWEDGE = 0x0100L;
    private const long WS_EX_CLIENTEDGE = 0x0200L;
    private const long WS_EX_STATICEDGE = 0x20000L;
    private const int SWP_NOZORDER = 0x0004;
    private const int SWP_NOACTIVATE = 0x0010;
    private const int SWP_FRAMECHANGED = 0x0020;
    private const int SWP_NOOWNERZORDER = 0x0200;
    private const int MONITOR_DEFAULTTONEAREST = 2;
    private static readonly IntPtr DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = new IntPtr(-4);

    // ---- Settings ----

    private static float _fps = 144f;      // 0 = leave the frame rate alone
    private static bool _stutterFix = true;
    private static bool _noLogo = true;
    private static bool _sprintFix = true;
    private static bool _borderless = true;
    private const int CheckIntervalMs = 500;   // FPS cap / stutter fix / borderless re-check
    private const int SprintUpdateMs = 16;     // sprint values follow the frame time

    private static long _speedFactors; // address of our 3 floats inside the game process

    private static int Main()
    {
        // Work in real pixels, so the borderless window exactly covers the monitor at any display scaling.
        try { if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware(); }
        catch (EntryPointNotFoundException) { SetProcessDPIAware(); }

        Console.Title = "DS3Smooth";
        Console.WriteLine("DS3Smooth - FPS unlock, stutter fix, sprint fix, No Logo and borderless window for Dark Souls III");
        Console.WriteLine("Based on SilkySouls3 (MIT) and DS3DebugFPS (Apache-2.0). Use offline.");
        Console.WriteLine();

        Process game = WaitForGame();
        try
        {
            return Run(game);
        }
        finally
        {
            game.Dispose();
        }
    }

    private static Process WaitForGame()
    {
        bool announced = false;
        while (true)
        {
            Process[] found = Process.GetProcessesByName("DarkSoulsIII");
            for (int i = 1; i < found.Length; i++) found[i].Dispose();
            if (found.Length > 0) return found[0];

            if (!announced)
            {
                Console.WriteLine("Waiting for Dark Souls III to start...");
                Console.WriteLine("(For No Logo to work, keep this open and start the game now.)");
                announced = true;
            }
            Thread.Sleep(250);
        }
    }

    private static int Run(Process game)
    {
        // The module list is not readable for the first moments after launch, so retry.
        string version = null;
        string gameDir = null;
        long moduleBase = 0;
        long moduleSize = 0;
        for (int attempt = 0; attempt < 60 && !HasExited(game); attempt++)
        {
            try
            {
                ProcessModule main = game.MainModule;
                version = main.FileVersionInfo.FileVersion;
                gameDir = Path.GetDirectoryName(main.FileName);
                moduleBase = main.BaseAddress.ToInt64();
                moduleSize = main.ModuleMemorySize;
                if (version != null && gameDir != null && moduleBase != 0 && moduleSize != 0) break;
            }
            catch (Exception)
            {
                // not ready yet
            }
            Thread.Sleep(250);
        }

        if (HasExited(game)) { Console.WriteLine("Game closed."); return 0; }
        if (version == null || gameDir == null || moduleBase == 0)
        {
            Console.WriteLine("Could not read the game's version. Try running DS3Smooth as administrator.");
            return Pause(1);
        }

        // The settings live in the game folder, in the same file as dinput8.dll's, so they are read
        // only now that the game (and so its folder) is known. No Logo is applied 2 s after start,
        // long after this.
        LoadConfig(gameDir);
        Console.WriteLine("FPS cap:     " + (_fps > 0 ? _fps.ToString("0.##", CultureInfo.InvariantCulture) : "off (game default)"));
        Console.WriteLine("Stutter fix: " + (_stutterFix ? "on" : "off"));
        Console.WriteLine("Sprint fix:  " + (!_sprintFix ? "off" : _fps > 0 ? "on" : "off (only needed with an FPS cap)"));
        Console.WriteLine("No Logo:     " + (_noLogo ? "on" : "off"));
        Console.WriteLine("Borderless:  " + (_borderless ? "on (set the game to Windowed mode)" : "off"));
        Console.WriteLine();
        if (_fps <= 0) _sprintFix = false;

        if (_fps <= 0 && !_stutterFix && !_noLogo && !_borderless)
        {
            Console.WriteLine("All DS3Smooth features are off in DS3Smooth.ini, nothing to do.");
            return Pause(0);
        }

        GameOffsets offsets = FindOffsets(version);
        if (offsets == null)
        {
            Console.WriteLine("Unsupported game version: " + version);
            Console.WriteLine("Supported: 1.3.2 to 1.15.2. Nothing was changed.");
            return Pause(1);
        }
        Console.WriteLine("Found Dark Souls III " + version);

        IntPtr handle = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_LIMITED_INFORMATION, false, game.Id);
        if (handle == IntPtr.Zero)
        {
            Console.WriteLine("Could not open the game process (error " + Marshal.GetLastWin32Error() + ").");
            Console.WriteLine("If the game runs as administrator, run DS3Smooth as administrator too.");
            return Pause(1);
        }

        try
        {
            // Both code patches need the game's code to be fully unpacked first.
            TimeSpan ageAtAttach = WaitUntilGameAge(game, TimeSpan.FromSeconds(2));
            if (HasExited(game)) { Console.WriteLine("Game closed."); return 0; }

            if (_noLogo) ApplyNoLogo(handle, moduleBase + offsets.NoLogo, ageAtAttach);
            if (_sprintFix) _sprintFix = ApplySprintFix(handle, moduleBase, moduleSize);

            bool fpsApplied = false, fixApplied = false;
            int fpsResets = 0, fixResets = 0;
            Stopwatch sinceCheck = Stopwatch.StartNew();
            bool firstCheck = true;

            while (!HasExited(game))
            {
                if (_sprintFix) UpdateSpeedFactors(handle, moduleBase + offsets.SprjFlipper);

                if (!firstCheck && sinceCheck.ElapsedMilliseconds < CheckIntervalMs)
                {
                    Thread.Sleep(SprintUpdateMs);
                    continue;
                }
                firstCheck = false;
                sinceCheck.Restart();

                // The game can rebuild these objects (e.g. on loading screens),
                // so re-check every half second and only write when a value differs.
                if (_fps > 0)
                {
                    long flipper = ReadInt64(handle, moduleBase + offsets.SprjFlipper);
                    if (flipper != 0)
                    {
                        bool changed = false;
                        float current = ReadFloat(handle, flipper + FlipperFps);
                        if (Math.Abs(current - _fps) > 0.01f)
                            changed |= WriteBytes(handle, flipper + FlipperFps, BitConverter.GetBytes(_fps));
                        if (ReadByte(handle, flipper + FlipperUseDebugFps) != 1)
                            changed |= WriteBytes(handle, flipper + FlipperUseDebugFps, new byte[] { 1 });

                        if (changed)
                        {
                            if (!fpsApplied) Console.WriteLine("FPS cap applied.");
                            else if (++fpsResets <= 5) Console.WriteLine("FPS cap was reset by the game, re-applied.");
                        }
                        fpsApplied = true;
                    }
                }

                if (_stutterFix)
                {
                    long input = ReadInt64(handle, moduleBase + offsets.UserInputManager);
                    if (input != 0)
                    {
                        if (ReadByte(handle, input + InputSteamInputFlag) != 1 &&
                            WriteBytes(handle, input + InputSteamInputFlag, new byte[] { 1 }))
                        {
                            if (!fixApplied) Console.WriteLine("Stutter fix applied.");
                            else if (++fixResets <= 5) Console.WriteLine("Stutter fix was reset by the game, re-applied.");
                        }
                        fixApplied = true;
                    }
                }

                if (_borderless) UpdateBorderless(game.Id);

                Thread.Sleep(_sprintFix ? SprintUpdateMs : CheckIntervalMs);
            }
            int exitCode;
            if (GetExitCodeProcess(handle, out exitCode) && exitCode < 0)
            {
                // NTSTATUS error codes (0xC...) mean the game crashed rather than being quit.
                Console.WriteLine("The game crashed (error 0x" + exitCode.ToString("X8") + ").");
                return Pause(1);
            }
            Console.WriteLine("Game closed. Exiting.");
            return 0;
        }
        finally
        {
            CloseHandle(handle);
        }
    }

    // Process.HasExited throws if the game died before we got a handle to it (e.g. a crash during
    // startup); treat that as exited instead of crashing DS3Smooth too.
    private static bool HasExited(Process game)
    {
        try { return game.HasExited; }
        catch (System.ComponentModel.Win32Exception) { return true; }
        catch (InvalidOperationException) { return true; }
    }

    // Waits until the game has been running for at least minAge. Returns its age on arrival.
    private static TimeSpan WaitUntilGameAge(Process game, TimeSpan minAge)
    {
        TimeSpan age;
        try { age = DateTime.Now - game.StartTime; }
        catch (Exception) { age = TimeSpan.Zero; }
        if (age < minAge) Thread.Sleep(minAge - age);
        return age;
    }

    private static void ApplyNoLogo(IntPtr handle, long address, TimeSpan age)
    {
        byte[] current = Read(handle, address, NoLogoPatch.Length);
        if (current == null)
        {
            Console.WriteLine("No Logo: could not read the game code, skipped.");
            return;
        }
        bool alreadyPatched = true;
        for (int i = 0; i < NoLogoPatch.Length; i++)
            if (current[i] != NoLogoPatch[i]) { alreadyPatched = false; break; }
        if (alreadyPatched)
        {
            Console.WriteLine("No Logo already applied.");
            return;
        }

        bool ok = WriteCode(handle, address, NoLogoPatch);
        if (!ok) Console.WriteLine("No Logo: patch failed (error " + Marshal.GetLastWin32Error() + ").");
        else if (age > TimeSpan.FromSeconds(15)) Console.WriteLine("No Logo applied, but the game was already running, so the logos had likely played already.");
        else Console.WriteLine("No Logo applied.");
    }

    // ---- Borderless window ----

    private static IntPtr _borderlessWindow = IntPtr.Zero; // the game window we made borderless
    private static int _borderlessResets;
    private static bool _fullscreenNoted;

    // Turns the game's normal window into a borderless one covering its monitor. Only touches a
    // window that has a title bar (Windowed mode) or one we already made borderless, so exclusive
    // Full Screen mode is left alone. Re-applied if the game puts the frame back or moves the window.
    private static void UpdateBorderless(int processId)
    {
        IntPtr hwnd = FindGameWindow(processId);
        if (hwnd == IntPtr.Zero || IsIconic(hwnd)) return;

        long style = GetWindowLongPtrW(hwnd, GWL_STYLE).ToInt64();
        RECT win;
        if (!GetWindowRect(hwnd, out win)) return;
        MONITORINFO info = new MONITORINFO();
        info.cbSize = Marshal.SizeOf(typeof(MONITORINFO));
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), ref info)) return;
        RECT mon = info.rcMonitor;

        bool hasFrame = (style & (WS_CAPTION | WS_THICKFRAME)) != 0;
        bool coversMonitor = win.Left == mon.Left && win.Top == mon.Top && win.Right == mon.Right && win.Bottom == mon.Bottom;
        if (!hasFrame && (hwnd != _borderlessWindow || coversMonitor))
        {
            if (hwnd != _borderlessWindow && !_fullscreenNoted)
            {
                Console.WriteLine("Borderless: the game is in Full Screen mode. Set Screen Mode to Windowed in its graphics settings.");
                _fullscreenNoted = true;
            }
            return;
        }

        if (hasFrame)
        {
            long newStyle = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
            SetWindowLongPtrW(hwnd, GWL_STYLE, new IntPtr(newStyle));
            long exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE).ToInt64();
            exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new IntPtr(exStyle));
        }

        int width = mon.Right - mon.Left, height = mon.Bottom - mon.Top;
        if (!SetWindowPos(hwnd, IntPtr.Zero, mon.Left, mon.Top, width, height,
                          SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE))
        {
            Console.WriteLine("Borderless: could not resize the game window (error " + Marshal.GetLastWin32Error() + ").");
            return;
        }

        if (hwnd != _borderlessWindow)
            Console.WriteLine("Borderless window applied (" + width + "x" + height + ").");
        else if (++_borderlessResets <= 5)
            Console.WriteLine("Borderless window was reset by the game, re-applied.");
        _borderlessWindow = hwnd;
    }

    // The game's main window: visible, top-level, owned by the game, titled "DARK SOULS III".
    private static IntPtr FindGameWindow(int processId)
    {
        IntPtr found = IntPtr.Zero;
        StringBuilder title = new StringBuilder(64);
        EnumWindows((hwnd, lParam) =>
        {
            int pid;
            GetWindowThreadProcessId(hwnd, out pid);
            if (pid != processId || !IsWindowVisible(hwnd)) return true;
            title.Length = 0;
            GetWindowTextW(hwnd, title, title.Capacity);
            if (!title.ToString().StartsWith("DARK SOULS", StringComparison.OrdinalIgnoreCase)) return true;
            found = hwnd;
            return false;
        }, IntPtr.Zero);
        return found;
    }

    // ---- Sprint fix (ported from DS3DebugFPS) ----

    private static bool ApplySprintFix(IntPtr handle, long moduleBase, long moduleSize)
    {
        long hit = 0;
        for (int attempt = 0; attempt < 5 && hit == 0; attempt++)
        {
            if (attempt > 0) Thread.Sleep(1000);
            hit = Scan(handle, moduleBase, moduleBase + moduleSize, SpeedPattern, SpeedMask);
        }
        if (hit == 0)
        {
            Console.WriteLine("Sprint fix: running-speed check not found, sprinting left unchanged.");
            return false;
        }

        // Our three floats must be within 2 GB of the game code (rip-relative addressing).
        long page = 0;
        for (long hint = (moduleBase + moduleSize + 0xFFFF) & ~0xFFFFL; page == 0 && hint < moduleBase + 0x70000000L; hint += 0x10000)
            page = VirtualAllocEx(handle, new IntPtr(hint), new IntPtr(0x1000), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE).ToInt64();
        if (page == 0)
        {
            Console.WriteLine("Sprint fix: could not allocate memory near the game code, skipped.");
            return false;
        }
        _speedFactors = page;

        // Fill in valid values before pointing the game at them.
        if (!WriteBytes(handle, page, ComputeSpeedFactors(1f / _fps)))
        {
            Console.WriteLine("Sprint fix: could not write the speed values, skipped.");
            return false;
        }

        if (!Redirect(handle, hit + 15, page))
        {
            Console.WriteLine("Sprint fix: patch failed (error " + Marshal.GetLastWin32Error() + ").");
            return false;
        }

        if (BytesEqual(Read(handle, hit + 0x29, 4), MulssRip) && BytesEqual(Read(handle, hit + 0x54, 4), MulssRip))
        {
            Redirect(handle, hit + 0x2D, page + 4);
            Redirect(handle, hit + 0x58, page + 8);
            Console.WriteLine("Sprint fix applied.");
        }
        else
        {
            Console.WriteLine("Sprint fix applied (stuck check only; slowdown/recovery rates not found).");
        }
        return true;
    }

    private static void UpdateSpeedFactors(IntPtr handle, long flipperSlot)
    {
        float frameTime = 1f / _fps;
        long flipper = ReadInt64(handle, flipperSlot);
        if (flipper != 0)
        {
            float measured = ReadFloat(handle, flipper + FlipperFrameTime);
            if (measured >= 1f / 480f && measured <= 0.05f) frameTime = measured;
        }
        WriteBytes(handle, _speedFactors, ComputeSpeedFactors(frameTime));
    }

    // Same math as DS3DebugFPS: at 60 FPS these equal the game's own constants (30, 0.8, recovery).
    private static byte[] ComputeSpeedFactors(float frameTime)
    {
        if (frameTime < 1f / 480f) frameTime = 1f / 480f;
        if (frameTime > 0.05f) frameTime = 0.05f;
        float slack = _fps >= 90f ? 1.2f : 1.0f;
        float recovery = _fps >= 90f ? 1.5f : 1.2f;

        float stuckScale = 0.5f / frameTime * slack;
        float slowdown = (float)Math.Exp(Math.Log(0.8) * frameTime * 60.0);
        float recover = (float)Math.Exp(Math.Log(recovery) * frameTime * 60.0);

        byte[] result = new byte[12];
        Buffer.BlockCopy(BitConverter.GetBytes(stuckScale), 0, result, 0, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(slowdown), 0, result, 4, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(recover), 0, result, 8, 4);
        return result;
    }

    // Rewrites the rip-relative disp32 at dispAddress so the instruction reads from target.
    private static bool Redirect(IntPtr handle, long dispAddress, long target)
    {
        long rel = target - (dispAddress + 4);
        if (rel < int.MinValue || rel > int.MaxValue) return false;
        return WriteCode(handle, dispAddress, BitConverter.GetBytes((int)rel));
    }

    // Pattern scan of the game module, read in 64 KB chunks that overlap by the pattern length.
    private static long Scan(IntPtr handle, long start, long end, byte[] pattern, string mask)
    {
        const int ChunkSize = 0x10000;
        int len = mask.Length;
        for (long addr = start; addr + len <= end; addr += ChunkSize - len + 1)
        {
            int size = (int)Math.Min(ChunkSize, end - addr);
            byte[] buf = Read(handle, addr, size);
            if (buf == null) continue;
            for (int i = 0; i + len <= size; i++)
            {
                if (buf[i] != pattern[0]) continue;
                int j = 1;
                while (j < len && (mask[j] != 'x' || buf[i + j] == pattern[j])) j++;
                if (j == len) return addr + i;
            }
        }
        return 0;
    }

    private static bool BytesEqual(byte[] a, byte[] b)
    {
        if (a == null || b == null || a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
        return true;
    }

    private static GameOffsets FindOffsets(string version)
    {
        foreach (KeyValuePair<string, GameOffsets> entry in Versions)
            if (version.StartsWith(entry.Key, StringComparison.Ordinal))
                return entry.Value;
        return null;
    }

    // ---- Memory helpers ----

    private static byte[] Read(IntPtr handle, long address, int size)
    {
        byte[] buffer = new byte[size];
        IntPtr read;
        if (!ReadProcessMemory(handle, new IntPtr(address), buffer, new IntPtr(size), out read) || read.ToInt64() != size)
            return null;
        return buffer;
    }

    private static long ReadInt64(IntPtr handle, long address)
    {
        byte[] b = Read(handle, address, 8);
        return b == null ? 0 : BitConverter.ToInt64(b, 0);
    }

    private static float ReadFloat(IntPtr handle, long address)
    {
        byte[] b = Read(handle, address, 4);
        return b == null ? float.NaN : BitConverter.ToSingle(b, 0);
    }

    private static int ReadByte(IntPtr handle, long address)
    {
        byte[] b = Read(handle, address, 1);
        return b == null ? -1 : b[0];
    }

    // Writes into game code: unprotect, write, restore protection, flush the CPU cache.
    private static bool WriteCode(IntPtr handle, long address, byte[] data)
    {
        IntPtr addr = new IntPtr(address);
        IntPtr size = new IntPtr(data.Length);
        int oldProtect;
        bool unprotected = VirtualProtectEx(handle, addr, size, PAGE_EXECUTE_READWRITE, out oldProtect);
        bool ok = WriteBytes(handle, address, data);
        if (unprotected)
        {
            int ignored;
            VirtualProtectEx(handle, addr, size, oldProtect, out ignored);
        }
        FlushInstructionCache(handle, addr, size);
        return ok;
    }

    private static bool WriteBytes(IntPtr handle, long address, byte[] data)
    {
        IntPtr written;
        return WriteProcessMemory(handle, new IntPtr(address), data, new IntPtr(data.Length), out written)
               && written.ToInt64() == data.Length;
    }

    // ---- Config (DS3Smooth.ini in the game folder, shared with dinput8.dll) ----

    // The file is installed with dinput8.dll (from mousefix\bin\); without it the defaults above apply.
    // dinput8.dll's keys and the [DS3Smooth] section line are ignored here.
    private static void LoadConfig(string gameDir)
    {
        string path = Path.Combine(gameDir, "DS3Smooth.ini");
        if (!File.Exists(path))
        {
            Console.WriteLine("No DS3Smooth.ini in the game folder, using the default settings.");
            return;
        }

        foreach (string raw in File.ReadAllLines(path))
        {
            string line = raw;
            int comment = line.IndexOf(';');
            if (comment >= 0) line = line.Substring(0, comment);
            int eq = line.IndexOf('=');
            if (eq < 0) continue;

            string key = line.Substring(0, eq).Trim().ToLowerInvariant();
            string value = line.Substring(eq + 1).Trim();

            if (key == "fps")
            {
                float fps;
                if (float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out fps))
                {
                    if (fps <= 0) _fps = 0;
                    else _fps = Math.Max(30f, Math.Min(1000f, fps));
                }
            }
            else if (key == "stutter_fix")
            {
                _stutterFix = IsOn(value);
            }
            else if (key == "no_logo")
            {
                _noLogo = IsOn(value);
            }
            else if (key == "sprint_fix")
            {
                _sprintFix = IsOn(value);
            }
            else if (key == "borderless")
            {
                _borderless = IsOn(value);
            }
        }
    }

    private static bool IsOn(string value)
    {
        return value == "1" || value.Equals("true", StringComparison.OrdinalIgnoreCase);
    }

    private static int Pause(int code)
    {
        Console.WriteLine();
        Console.WriteLine("Press any key to close.");
        try { Console.ReadKey(true); } catch (InvalidOperationException) { }
        return code;
    }
}
