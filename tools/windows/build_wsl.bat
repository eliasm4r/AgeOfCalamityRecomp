@echo off
setlocal
rem Age of Calamity Recomp build through WSL, on any machine with the repository cloned.
rem 1) sets up WSL (clang, lld, ninja, Python venv) - may ask for the Ubuntu sudo password the first time
rem 2) extracts the game code from "Game Files\" if it is not extracted yet
rem 3) generates the C and compiles it in ~/AgeOfCalamityRecomp-build (on the Linux disk)
rem The full log goes to build\build-wsl.log.
cd /d "%~dp0..\.."

where wsl >nul 2>nul
if errorlevel 1 (
    echo ERROR: WSL is not installed.
    echo In PowerShell as administrator: wsl --install -d Ubuntu-24.04
    goto :end
)
wsl -e true >nul 2>nul
if errorlevel 1 (
    echo ERROR: WSL could not open the default Linux distro.
    echo List the distros with:          wsl -l -v
    echo Install a new Ubuntu with:      wsl --install -d Ubuntu-24.04
    echo Or pick another as the default: wsl --set-default NAME
    goto :end
)
if not exist "build\v130\exefs\main" if not exist "Game Files\prod.keys" (
    echo ERROR: the game files are missing.
    echo Put the base, update 1.3.0 and DLC NSPs, and your prod.keys, in "%CD%\Game Files\".
    goto :end
)

wsl -e bash -lc "cd \"$(wslpath '%CD%')\" && bash tools/setup_wsl.sh && . ~/.venvs/AgeOfCalamityRecomp/bin/activate && mkdir -p build && python3 -u tools/build.py --work ~/AgeOfCalamityRecomp-build 2>&1 | tee build/build-wsl.log; exit ${PIPESTATUS[0]}"
if errorlevel 1 (
    echo.
    echo The build failed. See build\build-wsl.log
) else (
    echo.
    echo Build finished. Log in build\build-wsl.log
)

:end
echo.
pause
