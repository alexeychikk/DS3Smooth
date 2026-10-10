@echo off
setlocal
cd /d "%~dp0"

rem Needs Visual Studio (any edition) or Build Tools with the "Desktop development with C++" workload.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio not found. Install Visual Studio Build Tools with "Desktop development with C++".
    exit /b 1
)
set "VS="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo The C++ build tools are not installed. Add "Desktop development with C++" in the Visual Studio Installer.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1

rem Third-party files that aren't in the repository (downloaded once, checked by SHA-256).
powershell -NoProfile -ExecutionPolicy Bypass -File get_deps.ps1 || exit /b 1

if not exist bin mkdir bin
if not exist obj mkdir obj
rem dearxan (third_party\dearxan) is a Rust static library; it needs these system libraries too.
cl /nologo /O2 /MT /W4 /WX /EHsc /LD /Foobj\ src\dinput8.cpp src\dlaa.cpp src\dlaa_capture.cpp /link /DEF:src\dinput8.def /OUT:bin\dinput8.dll /IMPLIB:obj\dinput8.lib user32.lib third_party\dearxan\lib\dearxan.lib dbghelp.lib userenv.lib ntdll.lib ws2_32.lib bcrypt.lib advapi32.lib
if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

copy /y DS3MouseFix.ini bin\ >nul

rem Keyboard icons: needs Python with Pillow, and ..\tools\texconv.exe.
python ..\keyicons\make_icons.py bin\DS3KeyIcons
if errorlevel 1 (
    echo.
    echo Could not generate the keyboard icons.
    exit /b 1
)
rem DLAA: the DLSS model the NVIDIA driver downloaded (signed by NVIDIA), copied to bin\DS3DLAA\.
powershell -NoProfile -ExecutionPolicy Bypass -File copy_dlss.ps1
if errorlevel 1 echo DLAA will not work without it; everything else does.

echo.
echo Done: bin\dinput8.dll, bin\DS3MouseFix.ini, bin\DS3KeyIcons\ and bin\DS3DLAA\
echo Copy them into the game folder, next to DarkSoulsIII.exe.
