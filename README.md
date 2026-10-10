# DS3Smooth

Small fixes that make Dark Souls III on PC smoother and sharper. For the Steam version 1.15.2.

![Keyboard button icons](keyicons/preview.png)

## What it does

- **Higher frame rate:** removes the 60 FPS cap (144 by default, you pick the number). Sprinting
  keeps working normally at high FPS.
- **No more stutters:** removes the short freeze that happens about once a minute, the freeze when
  a USB device is plugged in, and the laggy menus with fast gaming mice.
- **Sharper image:** replaces the game's blurry anti-aliasing with NVIDIA DLAA, for clean edges
  without shimmering (needs an NVIDIA RTX graphics card).
- **Keyboard button icons:** the game only shows Xbox buttons on PC; this shows your keys instead.
- **Steady camera:** the camera no longer turns by itself while you run; only your mouse moves it.
- **Quicker start and a borderless window:** skips the logo screens and fills the screen without
  window borders.

Everything happens while the game runs. Your game files and saves are never changed.

## Install

There is no ready-made download; build it first (see [Building](#building), it's one command).

1. Copy everything inside `mousefix\bin\` into the game folder, next to `DarkSoulsIII.exe`
   (usually `C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS III\Game`).
2. In the game's settings:
   - **Screen mode: Windowed.** Needed for the higher frame rate; the window is made borderless.
   - **Motion Blur: ON.** Needed for DLAA, which removes the blur itself.
3. Start the game with **`Play DS3.bat`**.

To uninstall, delete `dinput8.dll`, `DS3MouseFix.ini` and the `DS3KeyIcons` and `DS3DLAA` folders
from the game folder.

## Settings

Two settings files; each option is explained inside, and `0` turns it off.

- `bin\DS3Smooth.ini`: frame rate cap, sprint fix, logo skip, borderless window, USB freeze fix.
- `DS3MouseFix.ini` in the game folder: DLAA, keyboard icons, steady camera, menu mouse fix,
  stutter fix.

The keyboard icons match one particular key layout (set in `keyicons\make_icons.py`).

## Good to know

- Don't use it together with DS3DebugFPS or FromStutterFix; they do the same jobs.
- Still stuck at 60 FPS? The game has to be in Windowed mode.
- RTSS works alongside; set `fps` at or above the RTSS limit.
- At 4K the game window covers the small DS3Smooth window, so it may look like it closed.
- Windows Defender may warn about `DS3Smooth.exe`, because it changes another program's memory.
- It doesn't change gameplay, but any mod carries some risk online. Use at your own risk.

## Building

You need, once:

- [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
  with **Desktop development with C++**
- [Python 3](https://www.python.org/downloads/) with Pillow: `pip install pillow`
- An NVIDIA driver, for DLAA (the build copies the DLSS file the driver already downloaded)

Then run, in the project folder:

```
build.bat
```

The first build downloads a few third-party files from their official GitHub pages (NVIDIA's DLSS
headers, dearxan, Microsoft's texconv) and checks each against a fixed checksum. The results go to
`bin\` and `mousefix\bin\`.

## Credits and licenses

- Frame rate unlock, logo skip and USB freeze fix are based on
  [SilkySouls3](https://github.com/borgCode/SilkySouls3) (MIT, see `LICENSE`).
- The sprint fix is ported from [DS3DebugFPS](https://github.com/0dm/DS3DebugFPS) (Apache-2.0,
  see `LICENSE-DS3DebugFPS`).
- The once-a-minute stutter fix uses [dearxan](https://github.com/tremwil/dearxan) by tremwil
  (MIT/Apache-2.0).
- The steady camera fix follows "Disable Auto Follow Cam" from
  [The Grand Archives](https://github.com/The-Grand-Archives/Dark-Souls-III-CT-TGA) table.
- DLAA uses NVIDIA DLSS through the [DLSS SDK](https://github.com/NVIDIA/DLSS) headers and the DLSS
  files installed with your NVIDIA driver. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.

This project is MIT licensed (`LICENSE`). Not affiliated with FromSoftware, Bandai Namco or NVIDIA.
