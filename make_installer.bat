@echo off
rem ============================================================
rem  Build prom.exe, then build the installer with Inno Setup.
rem  Run from "x64 Native Tools Command Prompt for VS".
rem  Inno Setup 6 is required: https://jrsoftware.org/isdl.php
rem ============================================================
setlocal
cd /d "%~dp0"

call build.bat
if errorlevel 1 exit /b 1

set "ISCC="
for %%D in ("%ProgramFiles(x86)%\Inno Setup 6" "%ProgramFiles%\Inno Setup 6" "%LocalAppData%\Programs\Inno Setup 6" "%ProgramFiles(x86)%\Inno Setup 7" "%ProgramFiles%\Inno Setup 7" "%LocalAppData%\Programs\Inno Setup 7") do (
  if not defined ISCC if exist "%%~D\ISCC.exe" set "ISCC=%%~D\ISCC.exe"
)
if not defined ISCC (
  echo.
  echo [ERROR] Inno Setup was not found. Please install Inno Setup 6:
  echo         https://jrsoftware.org/isdl.php
  exit /b 1
)

if not exist prom.ico (
  echo [ERROR] prom.ico is required ^(used as the installer icon^).
  exit /b 1
)

"%ISCC%" /Q prom.iss
if errorlevel 1 exit /b 1
echo.
echo Installer OK: see the "installer" folder.
exit /b 0