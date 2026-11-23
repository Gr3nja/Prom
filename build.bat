@echo off
rem ============================================================
rem  Prom build script (MSVC, 64-bit)
rem  Run this from "x64 Native Tools Command Prompt for VS".
rem  Needs in third_party\webview2 :
rem    WebView2.h / WebView2Loader.dll / WebView2Loader.dll.lib
rem    (WebView2Loader_dll.lib is also accepted)
rem  Optional: prom.ico + prom.rc next to this file = app icon.
rem ============================================================
setlocal
cd /d "%~dp0"
set "WV2=third_party\webview2"
set "WV2LIB="
if exist "%WV2%\WebView2Loader.dll.lib" set "WV2LIB=%WV2%\WebView2Loader.dll.lib"
if not defined WV2LIB if exist "%WV2%\WebView2Loader_dll.lib" set "WV2LIB=%WV2%\WebView2Loader_dll.lib"

if not exist "%WV2%\WebView2.h" (
  echo [ERROR] %WV2%\WebView2.h not found.
  goto :list
)
if not exist "%WV2%\WebView2Loader.dll" (
  echo [ERROR] %WV2%\WebView2Loader.dll not found.
  goto :list
)
if not defined WV2LIB (
  echo [ERROR] WebView2Loader.dll.lib not found in %WV2%
  goto :list
)

set "RES="
if exist prom.ico (
  rc /nologo prom.rc
  if errorlevel 1 exit /b 1
  set "RES=prom.res"
) else (
  echo [WARN] prom.ico not found - building without an app icon.
)

cl /nologo /utf-8 /W3 /I"%WV2%" prom.c %RES% /link /SUBSYSTEM:WINDOWS /OUT:prom.exe "%WV2LIB%"
if errorlevel 1 exit /b 1

copy /y "%WV2%\WebView2Loader.dll" . >nul
echo.
echo Build OK: prom.exe
echo Keep the "ui" folder and WebView2Loader.dll next to prom.exe.
exit /b 0

:list
echo.
echo Current folder: %CD%
echo Files in %WV2% :
dir /b "%WV2%" 2>nul
exit /b 1