# DS3Smooth

Personal mods for Dark Souls III (PC, Steam). Two independent parts:

- **DS3Smooth** (`src/Program.cs`): an external C# console exe that edits game memory: FPS
  unlock, USB device-stutter fix, sprint fix, No Logo, borderless window.
- **DS3MouseFix** (`mousefix/src/dinput8.cpp`): a `dinput8.dll` proxy that runs inside the game:
  menu mouse-stutter fix, keyboard button icons (icons drawn by `keyicons/make_icons.py`), Arxan
  disabler, and DLAA (DLSS anti-aliasing, `mousefix/src/dlaa.cpp`; see the DLAA section).

Git repository, public on GitHub: https://github.com/alexeychikk/DS3Smooth (branch `main`). Commit
or push only when the user asks. No binaries in git (`.gitignore`): `mousefix\get_deps.ps1` (run by
the build) downloads dearxan.lib, texconv.exe and the NVIDIA DLSS SDK headers (whose license forbids
redistributing them as source) and checks each file's pinned SHA-256. `README.md` is the short,
non-technical user guide; keep it in sync when features or settings change. The user tests in game. For the D3D11 hooks there are test programs in
`tools\dlltest\` (build with its `build.bat`): `dlltest.exe` (deferred contexts, Flush, capture) and
`ds3sim.exe` (a DS3-like frame with the game's own shader bytecode; runs the whole DLAA path through the
real DLSS, with `dlaa_dump = 1` in the test folder's ini). Run them before asking the user to test.

**The user drives the game.** Don't launch DS3 and steer it with simulated input; ask the user to
start it, do the in-game step (e.g. F9/F10) and quit, and batch what each run collects.

## Deploying: always build and install

Whenever you change something for the user, **build it and copy every file it needs to where it is
used**, so the user can start the game and try it straight away. Don't stop at "copy X to Y".

| Part | Build | Install to |
| --- | --- | --- |
| Both | `build.bat` (root) builds DS3Smooth, then calls `mousefix\build.bat` | as below |
| DS3Smooth | `build.bat` → `bin\DS3Smooth.exe` | Stays in `bin\`; the user starts it with `Play DS3.bat` (project root), which starts it minimized and then launches the game through Steam (`steam://rungameid/374320`) |
| DS3MouseFix | `mousefix\build.bat` → `mousefix\bin\dinput8.dll` + `mousefix\bin\DS3KeyIcons\*.dds` | `F:\SteamLibrary\steamapps\common\DARK SOULS III\Game\` (next to `DarkSoulsIII.exe`); the icons go in `Game\DS3KeyIcons\`, replacing the old ones |

- **Never overwrite the user's ini files.** `bin\DS3Smooth.ini` and the game folder's
  `DS3MouseFix.ini` hold their own settings (e.g. `fps = 120`, not the default 144). When you add a
  setting, add the new key and comment to the installed ini and leave the existing values alone.
  Also update the template ini in the repo (`DS3Smooth.ini`, `mousefix\DS3MouseFix.ini`) and, for
  DS3Smooth, the defaults written by `LoadConfig()` in `Program.cs`.
- The DLL can't be replaced while the game is running (the file is locked). Check for a
  `DarkSoulsIII` process first; if it's running, tell the user to close the game, not kill it.
- In your summary, say what was installed where.

## Environment

- Windows 11; PowerShell is the main shell, Git Bash also works.
- Game: `F:\SteamLibrary\steamapps\common\DARK SOULS III\Game\DarkSoulsIII.exe`, version 1.15.2.0.
- C# compiler: `%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe` (.NET Framework 4.x).
  **`Program.cs` must stay C# 5**: no `$""`, `?.`, `=>` members, `out var`, tuples and so on.
- C++: Visual Studio 2022 Community at `C:\Program Files\Microsoft Visual Studio\2022\Community`
  (MSVC 14.38). `mousefix\build.bat` finds it with vswhere and compiles with `/W4 /WX`, so any
  warning breaks the build.
- `dumpbin`: `...\2022\Community\VC\Tools\MSVC\14.38.33130\bin\Hostx64\x64\dumpbin.exe`. From Git
  Bash, pass options as `-nologo -imports` (Git Bash turns `/nologo` into a path).
- Running `.bat` files from the scratchpad: use `cmd /c "cd /d <dir> && call <dir>\x.bat"`; a bare
  `cmd /c x.bat` there failed with "not recognized".
- Python 3.10 with Pillow and numpy. `tools\texconv.exe` is Microsoft's DirectXTex texconv (signed,
  from the GitHub releases); it decodes the game's BC textures (`-tu` for typeless formats) and
  encodes ours. BC5 normal maps can't be written as PNG; that's harmless.
- Steam is at `C:\Games`. The user's Steam screenshots (F12) of DS3 are in
  `C:\Games\userdata\<user id>\760\remote\374320\screenshots\` (one user id folder there). You can also screenshot the game
  yourself with PowerShell `CopyFromScreen` (3840x2160 monitor) to check visual changes.

## DS3Smooth (`src/Program.cs`)

Waits for `DarkSoulsIII.exe`, reads its FileVersion, picks static offsets from the `Versions`
table (1.3.2 to 1.15.2; the keys are version prefixes like `"1.15.2."`), then opens the process and
loops: sprint values every 16 ms, everything else every 0.5 s, writing only when the game has reset
a value. It exits when the game exits. Settings come from `DS3Smooth.ini` next to the exe (keys
`fps`, `stutter_fix`, `sprint_fix`, `no_logo`, `borderless`); it's created with defaults if missing.

- **FPS unlock:** the debug frame limiter: `[SprjFlipper] + 0x354` float cap, `+0x358` byte = 1.
  Same method as DS3DebugFPS. `fps` is clamped to 30–1000; 0 = off.
- **Stutter fix:** `[UserInputManager] + 0x24B` = 1 makes the game skip the slow device re-scan on
  USB device changes (same flag FromStutterFix uses).
- **No Logo:** 20-byte code patch at `module + NoLogo` offset, applied once, 2 s after game start
  (SilkySouls3 timing). Only works if DS3Smooth is running before the logos play.
- **Sprint fix** (ported from DS3DebugFPS, rewritten in C#): pattern-scans for the running "am I
  stuck?" check (`mulss xmm,[rip+30.0]`), allocates a page within 2 GB of the module, and redirects
  three rip-relative constants (stuck scale at `hit+15`, slowdown at `hit+0x2D`, recovery at
  `hit+0x58`) to it. Values are recomputed from the real frame time (`[SprjFlipper] + 0x26C`). At
  60 FPS they equal the originals (30, 0.8, 1.2). Only active when `fps` is set. If DS3Smooth is
  closed mid-game the values freeze, which is fine at a steady frame rate.
- **Borderless:** strips caption/frame styles from the window titled `DARK SOULS…` and sizes it to
  its monitor in physical pixels (the process is per-monitor DPI aware). Only touches windows that
  have a frame or that we made borderless, so exclusive Full Screen is left alone. Needs the game
  in Windowed mode. No game memory involved.

Offsets and the No Logo patch come from SilkySouls3; the sprint fix comes from DS3DebugFPS.

## DS3MouseFix (`mousefix/src/dinput8.cpp`)

Proxy `dinput8.dll`: forwards `DirectInput8Create` (the game's only dinput8 import) to the System32
DLL. Settings come from `DS3MouseFix.ini`, section `[DS3MouseFix]`. A setup thread subclasses the
game window, re-checking every 2 s in case the game recreates it. `DS3MouseFix.log` is written only
with `log = 1` (default 0, added 2026-10-10) or any developer setting (`dump_textures`, `stall_trace`,
`dlaa_capture`, `dlaa_dump`); with those off no debugging code runs per frame (the stall tracer's
Present timestamp and DLAA's brightness check are gated too). Ask the user for `log = 1` when a run's
log is needed.

- **Mouse throttle** (`interval_ms`, default 8, 0–50, 0 = off). Measured cause: in menus the game's
  wndproc spends ~1.25 ms each on `WM_SETCURSOR` and `WM_MOUSEMOVE` (~0.02 ms in gameplay), so with
  a 1000–8000 Hz mouse the main thread barely renders. Moves and set-cursors are passed at most once
  per interval; the newest position is held and delivered by a short timer, or right before any
  click or wheel message. The camera is unaffected because the game reads it through DirectInput.
- **Keyboard icons** (`keyboard_icons`, default 1) and **texture dump** (`dump_textures`, default
  0): see below. They log to `DS3MouseFix.log` next to the DLL.
- **Disable Arxan** (`disable_arxan`, default 1): calls `dearxan_neuter_arxan` from `DllMain`, before
  the game's entry point. dearxan v0.5.6 (tremwil, MIT/Apache-2.0) is vendored prebuilt in
  `mousefix/third_party/dearxan` (header, `dearxan.lib`, LICENSE, SOURCE.txt). Its result is logged
  ("Arxan: disabled."). Reviewed 2026-10-09 before use:
  - **Source (v0.5.6):** the analysis code has no OS calls and no `unsafe`. The disabler only
    patches the game in-process: SteamStub header decryption (XTEA, like Steamless), a code
    buffer, in-process thread suspension, and a `Local\..._<pid>` file mapping for coordination.
    The only file access reads the first 4 KB of the game exe. It makes all game sections RWX.
  - **Linked DLL:** imports nothing beyond kernel32/user32/ntdll/synch, with no networking,
    registry or other-process APIs. Its only strings are Rust's standard io error names.
  - It's not built from source here, because Rust isn't installed.
- **Camera auto-follow off** (`no_camera_auto_follow`, default 1, added 2026-10-10): NOPs the 8-byte
  `movdqa [rsi+140h], xmm4` that stores the camera's automatic turn toward the running direction (from
  The Grand Archives DS3 CE table, "Disable Auto Follow Cam" / `camHRotateCorrection`, read as text from
  GitHub). In 1.15.2 it's at `+0x516FB4` (the table's 1.15.0 address is stale; identified by its 0x7A8
  distance to the next `movdqa [rsi+140h], xmm5`, as in the table's older listing). Found by a unique
  wildcard pattern in DllMain (the exe's `.text` isn't encrypted on disk); logs "Camera auto-follow:".
  Confirmed in game 2026-10-10 (log: "disabled (DarkSoulsIII.exe+0x516fb4)"; the user liked it).
- **Stall trace** (`stall_trace`, default 0): hooks `IDXGISwapChain::Present` (vtable 8, via the
  `CreateDXGIFactory` import and factory vtable 10). When no frame is presented for 40 ms, it
  samples the render thread's stack every 15 ms (suspend, `RtlVirtualUnwind`) and logs it with
  module!export+offset. It also logs `WM_DEVICECHANGE`.
- Can't be combined with any other `dinput8.dll` mod.

## Micro stutters (investigated 2026-10-09)

- **Symptom:** a ~125–145 ms freeze about once a minute in normal play. Everything else is clean:
  PresentMon shows a steady 8.33 ms at the 120 cap, Hardware Independent Flip, and G-Sync working
  (displayed time = frame time).
- **Ruled out by measurement:** CPU load from other processes, disk I/O, page faults, DPC/ISR
  spikes, and `WM_DEVICECHANGE` (none logged).
- **Cause:** the stall tracer shows the render thread *executing* code in the exe's second `.text`
  section (RVA 0x4DBD000–0x56C0BFF, no unwind data), which is Arxan's. Our patches stay intact,
  so Arxan isn't reverting them.
- **Fixed with dearxan (above), confirmed 2026-10-09:** in 16 minutes of play there were no
  120–145 ms stalls, against about one a minute before, and the user confirmed the stutters were
  gone. What remains is a ~50 ms hitch every few minutes in normal game code (call chain
  `DS3+0x1ab2xxx` < `0x1ad13ca` < `0x2b7b8e` … < main loop `0x23c1da5`). It was present before
  the fix too and is probably map streaming; left alone.
- **Crash on exit (pre-existing, not ours):** the game crashes with `0xc0000374` heap corruption
  in `ntdll+0x117eb5` every time it's closed. It was logged 16 times since 2026-10-08, including
  before the DLL existed. Harmless because it happens on quit. Not investigated yet.
- **DS3Smooth fix:** `Process.HasExited` throws `Win32Exception` if the game dies before
  DS3Smooth gets a handle. It now goes through `HasExited(game)`, which treats that as exited.
- **Tools:**
  - `tools\PresentMon.exe` (Intel, signed, v2.6.0): run it without admin (the user is in
    Performance Log Users) with `--process_name DarkSoulsIII.exe --v2_metrics --date_time`.
    Its times are UTC, 3 h ahead of local time.
  - `tools\StutterMon.exe` (`tools\StutterMon.cs`, built with csc): samples game CPU/IO/page
    faults and per-core DPC/interrupt time every 100 ms, plus the top processes every second.

## DLAA (`mousefix/src/dlaa.cpp`, `dlaa_capture.cpp`, `dlaa_internal.h`), started 2026-10-10

DLSS at native resolution instead of the game's FXAA, written from scratch (the Nexus mod 2382 has no
source and ships its own binaries). Settings: `dlaa` (1), `dlaa_dump` (0), `dlaa_jitter_flip` (0),
`dlaa_capture` (0). Needs **Motion Blur ON** in the game (for object motion; the blur is removed).

- **No NVIDIA binaries shipped, no SDK library:** only the SDK headers (`mousefix/third_party/ngx`,
  NVIDIA/DLSS commit 3749594). `_nvngx.dll` is loaded from the driver store via the registry (never the
  game folder) and its `NVSDK_NGX_D3D11_*` exports are called directly (signatures as in OptiScaler's
  `NVNGX_Proxy.h`; `Init_ProjectID` takes the SDK version *before* the feature info). `build.bat` runs
  `copy_dlss.ps1`: the newest NVIDIA-signed DLSS model the driver downloaded to
  `C:\ProgramData\NVIDIA\NGX\models\dlss\versions\*\files\*.bin` → `Game\DS3DLAA\nvngx_dlss.dll`.
  Smoke test passed in game: DLAA feature at 3840x2160, preset K (driver DRS override).
- **DS3 has no TAA:** AA is FXAA (`CBFxaaParam`; one shader also has an unrelated `CBDlaaParam`).
  Shaders keep their reflection (cbuffer/variable names), which is how the passes were identified.
- **Render order** (from captures, motion blur on): shadows (D32S8 4096²) and G-buffer (6 targets: f10,
  f28, f29, f28, f26, f10; depth D32S8 `R32G8X24_TYPELESS` at screen size, cleared depth+stencil once
  per frame first) on worker threads → lighting (`cbLightAcc*`, read-only depth) → HDR scene (f10) →
  forward/transparent, half-res transparents (read-only half depth) → **velocity pass**: target
  half-res RGBA8 cleared to (127/255, 127/255, 1, 0); moving objects only; VS writes current and
  previous clip position (`cbMatrixWVP` b0: `VC_MatrixWVP`, `VC_PrevMatrixWVP`; bones `cb5`/prev `cb6`,
  cloth `cb12`); PS (`940604d2b2bcb300` and alpha-tested variants) outputs
  `saturate((cur - prev) * cbModelParam[0].xy * 0.5 + 0.5)` (b7) → main thread, **YEBIS** (Silicon
  Studio post effects): `393a947c35535471` copies the HDR scene to its target, `7004286725cbc5c4`,
  first reader `454e47f5721b6ac0` (also the glare downsample), motion blur `ef5c2e8f8b91a269`,
  `fb4f78e37bba3aa5`, `d55e5f7c1e2c4722` (velocity: depth + `am44_TransformMatrix` + object
  velocity), `e484bfebc00c5cf7` (combine, twice), glare pyramid, tonemap `c2ef3668568c5617`, grading
  LUT → FXAA + glow (worker list) → HUD (to the LDR target, with the scene depth bound) →
  brightness/gamma → back buffer.
- **Camera:** `cbSceneParam` VS b8 (960 bytes): `FC_MatrixInvViewProj` registers 8-11,
  `VC_MatrixViewProj` 24-27, rows (`dp4 o0.x, pos, cb8[24]`). Standard depth (1 = far), infinite far
  plane, near 0.08. **Camera-relative:** the view-projection has no translation but the inverse gives
  absolute world positions (`inv * vp` = translation by the camera position, e.g. (66, -155, -591)), so
  the motion vector shader uses `prevVP * translate(-prevCamPos) * inv`, built in double on the CPU
  (`BuildReprojection`). Mixing them (the first in-game build) gave camera motion vectors of 0 or
  thousands of pixels. The HDR scene is `R16G16B16A16_TYPELESS`.
- **D3D11 hooking facts (measured with test programs):** every context has its own heap copy of the
  vtable (patch each; deferred contexts via `CreateDeferredContext`, entry 27); d3d11 rewrites the
  immediate context's draw/dispatch/copy/clear entries on `Flush` and again on the first draw after it
  (re-hook after every call on the immediate context); command list addresses are reused (look up
  newest first). DS3 uses 17 worker contexts and the immediate one. The GPU clips to the viewport
  rounded down to whole pixels: any negative TopLeft (even -0.05) leaves the target's last column/row
  unrendered, so with negative jitter the scene's right/bottom edge is missing (a flickering line at
  the bottom, reported 2026-10-10); `FillMissingEdges` copies the neighbour in before DLSS.
- **How it works:** viewport jitter (Halton 2,3, 8 phases) on scene geometry only; the game's velocity
  draws redrawn into a full-res R32G32_FLOAT target (scale 1/16 in b7, read-only scene depth,
  LESS_EQUAL, small negative depth bias); a compute pass makes motion vectors (camera from depth +
  matrices, objects where drawn); DLSS (IsHDR | AutoExposure) runs in a swapped
  `ID3DDeviceContextState` right before YEBIS's first read of the scene; YEBIS motion blur and FXAA
  draws are skipped. Per-frame data travels with command lists (`ListInfo`).
- **Verified offline** with `ds3sim.exe` (camera-relative like the game) + `tools\check_dump.py`
  (motion vectors exact, DLSS runs). **In game 2026-10-10:** runs at 120 fps, DLSS every frame, the user
  saw no ghosting and liked the look. After the camera fix, verified from dumps: warping with the motion
  vectors matches the next frame ~3x better than not warping, and the jitter sign is right (the color
  input is offset from DLSS's output in the jitter's direction). The user confirmed it looks good.
- **Black flashes (reported 2026-10-10, not seen since):** a few black frames, twice, while talking to
  an NPC, and once after a warp; DLSS ran every frame then. DLSS's colour input is now a cleaned copy
  of the scene (NaN/inf replaced), but `ds3sim ... nan` showed DLSS tolerates a NaN and an inf pixel
  anyway, so that's probably not the cause. With `dlaa_dump = 1`, each frame the brightness of input vs
  output is compared on a 1/16 grid (read back without waiting); a dark or invalid output logs "OUTPUT
  BROKEN" and the first one per session saves a dump (`DS3DLAA\dump\`, ~500 MB). Warps show ~250
  frames without DLSS (loading fade, "scene copy seen, never read"), which is harmless. Next run
  (22:35, NPCs + warp): no flashes seen, nothing broken logged, no invalid input pixels; single frames
  without DLSS ("scene copy not seen") every ~20 s, unexplained, with nothing visible.
- **Release state:** all debugging is behind ini flags that are off: `dlaa_dump` (also gates F10, NGX's
  own ~800-line log, the GPU depth readback, periodic log lines, the output brightness check and the
  frames-without-DLSS lines), `dlaa_capture` (F9). Plain logging is `log` (off). The log is
  deleted at startup once it passes 1 MB. The game's `DS3DLAA\` holds only `nvngx_dlss.dll`; the
  shader set for `ds3sim` is regenerated with `dlaa_capture = 1` (saved to `DS3DLAA\shaders\`).
- **Tools:** `dlaa_capture = 1` saves shaders to `DS3DLAA\shaders\` and F9 (or `capture.trigger`)
  writes 4 frames of draw calls to `DS3DLAA\capture\`; `tools\disasm_shaders.py` disassembles shaders;
  `tools\check_dump.py` checks a `dlaa_dump`.

## Keyboard button icons

**DS3 on PC has no keyboard prompts.** It always draws Xbox icons, whatever input device is used.
(Verified 2026-10-09: no controller connected, Xbox icons anyway. A hook that hid XInput
controllers was tried first and removed because it can't work.) So the icon textures are replaced:

- **How:** `HookImport()` redirects the game's `d3d11.dll!D3D11CreateDevice` import (its only d3d11
  import). After the device is created, entry 5 of its vtable (`CreateTexture2D`, shared by all
  devices) is hooked. Every texture created with initial data is hashed: FNV-1a 64 over width,
  height and format plus the top mip's tight rows, so the row pitch doesn't matter. If
  `Game\DS3KeyIcons\<hash>.dds` exists (DX10-header DDS with the same size, format and mip count),
  the texture is created from it instead. With `dump_textures = 1`, every such texture is saved to
  `Game\texture_dump\<hash>_<w>x<h>_f<dxgi format>.dds`; that's how the icons were found. A dump of
  menu, inventory and one area is about 2,200 files and 820 MB.
- **The icons:** each button is its own **32x32 BC7_UNORM texture, 1 mip**. The full Xbox set
  (A/B/X/Y, LB/RB/LT/RT, L3/R3 click, View, Menu, both sticks in two styles, D-pad variants) is
  listed with hashes in `keyicons/make_icons.py`. The dump also has a PlayStation set (shown with a
  DualShock) and four-face-button diagrams; those aren't replaced.
- **The keys** come from the user's key config (Settings → Key bindings, screenshots from
  2026-10-09). Where a button differs between menus and gameplay, the menu key is used, because
  prompts are mostly shown in menus: B = Q (cancel), LB/RB = Shift+←/→, LT/RT = Shift+↑/↓,
  L3 = X (sort), R3 = C (Function 3). If the user rebinds keys, update `ICONS` and rebuild. Side
  effect: the "Gamepad" column of the key config screen shows the keyboard icons too.
- **Style:** dark beveled key caps with a gold rim and gold labels, drawn at 8x and scaled down
  to match the game's icons. Arrow keys are drawn as a cross with the relevant directions lit.
- Verified in game: the A icon test showed E (log: `replaced c3a90c813af7cd90`). The full
  23-icon set was installed on 2026-10-09 but not yet seen in game.
- Other facts: the game statically imports only `XINPUT1_3.dll` ordinals 2 and 3; it's SteamStub
  packed (`.bind` section) with its import table in `.idata` (Read Write).

## Conventions

- Comments explain *why* (measurements, game behavior), in plain sentences. Keep the header comment
  at the top of each source file up to date when features change.
- Console messages in DS3Smooth are short and user-facing; repeated "reset by the game" messages
  are capped at 5.
- Changes are memory-only, never written to game files on disk; the icons are swapped as the
  game loads them. Don't add anything with network access.

## Licenses and credits

`LICENSE` (MIT) covers this project and the SilkySouls3-derived parts
(https://github.com/borgCode/SilkySouls3). `LICENSE-DS3DebugFPS` (Apache-2.0) covers the ported
sprint fix (https://github.com/0dm/DS3DebugFPS). Keep both files and the attribution comments in
`Program.cs`.

User-facing notes (compatibility, Windowed mode, RTSS, Defender) are in `README.md`.
