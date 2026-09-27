@echo off
rem Run the media remote in a visible window. Close the window to stop it.
title ESP32 Media Remote
pushd "%~dp0"
"python\python.exe" "host\media_remote_win.py" %*
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" (
  echo.
  echo The program exited with code %RC%.
  echo Run Troubleshoot.bat to see what went wrong.
  pause
)
