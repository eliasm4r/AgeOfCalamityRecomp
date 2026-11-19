@echo off
setlocal
rem Opens the launcher (launcher\launcher.cpp): pick the folder with your copy of the game and forge the port for the
rem Deck and/or Windows. In the release package AoC-Launcher.exe comes ready next to this file; in a copy of the
rem repository the launcher is compiled the first time in MSYS2 UCRT64 (tools\build_launcher.py, CONTRIBUTING.md).
cd /d "%~dp0"
if exist "%~dp0AoC-Launcher.exe" (
    start "" "%~dp0AoC-Launcher.exe"
    exit /b 0
)
set "EXE=%~dp0build\launcher\aoc-launcher.exe"
if exist "%EXE%" goto :open

if not exist "C:\msys64\usr\bin\bash.exe" (
    echo ERROR: MSYS2 not found in C:\msys64. See CONTRIBUTING.md, "Native Windows".
    pause
    exit /b 1
)
echo Compiling the launcher (first time only)...
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
C:\msys64\usr\bin\bash.exe -lc "python3 tools/build_launcher.py"
if not exist "%EXE%" (
    echo ERROR: the launcher did not compile. See the messages above.
    pause
    exit /b 1
)

:open
start "" "%EXE%"
