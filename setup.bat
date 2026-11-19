@echo off
rem Opens the setup from a copy of the repository. The release package has AgeOfCalamityRecomp.exe for this instead.
cd /d "%~dp0"
if exist "%~dp0python\pythonw.exe" if exist "%~dp0python\tcl" (
    start "" "%~dp0python\pythonw.exe" "%~dp0setup\setup.py"
    exit /b 0
)
where pyw >nul 2>nul && (
    start "" pyw -3 "%~dp0setup\setup.py"
    exit /b 0
)
start "" pythonw "%~dp0setup\setup.py"
