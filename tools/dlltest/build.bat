@echo off
rem Builds the D3D11 test programs for DS3MouseFix's hooks into tools\dlltest\bin\.
rem   dlltest.exe <dir with dinput8.dll>                 - deferred contexts, Flush, F9 capture
rem   ds3sim.exe <dir with dinput8.dll> <shader dir> [n] [nan] - a DS3-like frame with the game's own shaders (DLAA)
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist bin mkdir bin
cl /nologo /EHsc /W4 /Febin\dlltest.exe /Fobin\ dlltest.cpp || exit /b 1
cl /nologo /EHsc /W4 /Febin\ds3sim.exe /Fobin\ ds3sim.cpp || exit /b 1
