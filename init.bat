@echo off
REM Run this from the game folder (where the game exe and proxy dinput8.dll live)
setlocal
set SYS=%WINDIR%\System32
if exist "%WINDIR%\SysWOW64\dinput8.dll" set SYS=%WINDIR%\SysWOW64

if not exist "%SYS%\dinput8.dll" (
    echo Could not find the original dinput8.dll in %SYS%
    echo Install manually: copy dinput8.dll from System32 and rename it
    echo to dinput8_orig.dll next to this file.
    pause
    exit /b 1
)

copy /y "%SYS%\dinput8.dll" "%~dp0dinput8_orig.dll"
echo Done: dinput8_orig.dll copied.
echo Make sure dinput8.dll (the proxy) and dinput8.ini are in this same folder.
pause
