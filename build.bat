@echo off
rem Builds everything: DS3Smooth (bin\) and DS3MouseFix (mousefix\bin\).
setlocal
cd /d "%~dp0"

rem Uses the C# compiler that ships with Windows 11 (.NET Framework 4.x). Nothing to install.
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" (
    echo Could not find %CSC%
    echo Enable ".NET Framework 4.8" in "Turn Windows features on or off" and try again.
    exit /b 1
)

if not exist bin mkdir bin
"%CSC%" /nologo /optimize+ /platform:x64 /target:exe /out:bin\DS3Smooth.exe src\Program.cs
if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo Done: bin\DS3Smooth.exe
echo.

call mousefix\build.bat || exit /b 1
