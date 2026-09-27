@echo off
title ESP32 Media Remote - install
setlocal
set "DEST=%LOCALAPPDATA%\ESP32-Media-Remote"
set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"

echo.
echo  Installing the ESP32 Media Remote
echo  ---------------------------------
echo.
echo  Copying to %DEST%
echo  (it runs from local disk rather than from the folder you unpacked it in,
echo   so it starts faster and keeps working if that folder moves or a network
echo   share goes offline)
echo.

robocopy "%~dp0." "%DEST%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto copyfailed

echo  Registering it to start when you log in...
> "%STARTUP%\ESP32-Media-Remote.vbs" echo CreateObject("WScript.Shell").Run """%DEST%\Start.bat""", 0, False

echo  Starting...
start "" wscript.exe "%STARTUP%\ESP32-Media-Remote.vbs"

echo.
echo  Done. The remote now starts by itself when you log in, and runs in the
echo  background without a window.
echo.
echo  Undo with Uninstall.bat
echo.
pause
exit /b 0

:copyfailed
echo.
echo  The copy failed -- could not write to %DEST%
echo.
pause
exit /b 1
