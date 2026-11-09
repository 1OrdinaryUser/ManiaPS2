@echo off
rem Sonic Mania PS2 port - build. Put your own Data.rsdk into the "input" folder first, then run this file.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
    echo.
    echo The build did not finish. Read the message above, fix the problem and run build.bat again.
)
if /i not "%NOPAUSE%"=="1" pause
exit /b %RC%
