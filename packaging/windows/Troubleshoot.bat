@echo off
title ESP32 Media Remote - troubleshooting
pushd "%~dp0"
echo.
echo  === Serial ports Windows can see ===
"python\python.exe" "host\media_remote_win.py" --list-ports
echo.
echo  If the remote is not in that list, it is either not plugged in, or the
echo  cable is a charge-only cable with no data lines.
echo.
echo  === Running with full logging (close the window to stop) ===
echo.
"python\python.exe" "host\media_remote_win.py" -v
popd
pause
