@echo off
rem Starts DS3Smooth (minimized), then Dark Souls III through Steam.
rem DS3Smooth has to be running before the game for No Logo to work; it waits for the game by itself.
setlocal

set "MOD=%~dp0bin\DS3Smooth.exe"
if not exist "%MOD%" (
    echo %MOD% not found. Run build.bat first.
    pause
    exit /b 1
)

tasklist /fi "imagename eq DS3Smooth.exe" | find /i "DS3Smooth.exe" >nul
if errorlevel 1 start "DS3Smooth" /min /d "%~dp0bin" "%MOD%"

rem 374320 = Dark Souls III. Going through Steam avoids the game restarting itself via Steam.
start "" "steam://rungameid/374320"
