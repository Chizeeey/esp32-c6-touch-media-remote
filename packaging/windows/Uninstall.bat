@echo off
title ESP32 Media Remote - uninstall
setlocal
set "DEST=%LOCALAPPDATA%\ESP32-Media-Remote"
set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"

echo  Stopping the program...
rem Matches only the bundled interpreter, not any other Python you may have
rem running.
powershell -NoProfile -Command "Get-Process python -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*ESP32-Media-Remote*' } | Stop-Process -Force" >nul 2>&1

echo  Removing the autostart entry...
del /q "%STARTUP%\ESP32-Media-Remote.vbs" >nul 2>&1

echo  Removing %DEST%
rmdir /s /q "%DEST%" >nul 2>&1

echo.
echo  Done. The board keeps its own settings -- accent colour and coin choice
echo  live in its flash, so they travel with the device.
echo.
pause
