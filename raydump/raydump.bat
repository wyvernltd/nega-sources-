@echo off
REM Double-click me. Roblox must be running (menu is fine, in-game is better).
cd /d "%~dp0"
raydump.exe
echo.
echo ---------------------------------------------------------------
echo Output also saved next to this file as raydump_out.txt
echo ---------------------------------------------------------------
pause
