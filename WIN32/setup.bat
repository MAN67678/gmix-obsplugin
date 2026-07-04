@echo off
setlocal enabledelayedexpansion

:: ─────────────────────────────────────────────────────────────────────────────
:: GMix (Windows) OBS plugin installer.
::
:: Removes any previously installed GMix OBS plugin (the old, unrelated
:: "gmix-obs-source.dll" prototype AND any earlier "obs-gmix-source.dll" from
:: this port) and installs a fresh copy of this build's obs-gmix-source.dll +
:: locale data. Requires Administrator rights (Program Files is not
:: user-writable) -- this script self-elevates via UAC if not already
:: running elevated.
::
:: Run this from the WIN32 directory (paths below are relative to it). Build
:: the plugin first: cmake --build build --target obs-gmix-source
:: ─────────────────────────────────────────────────────────────────────────────

:: Edit this if your OBS Studio is installed somewhere else.
set "OBS_DIR=C:\Program Files\obs-studio"

set "PLUGIN_SRC=%~dp0build\obs_plugin\bin\64bit\obs-gmix-source.dll"
set "LOCALE_SRC=%~dp0data\locale\en-US.ini"
set "PLUGIN_DST_DIR=%OBS_DIR%\obs-plugins\64bit"
set "DATA_DST_DIR=%OBS_DIR%\data\obs-plugins\obs-gmix-source"

:: ── Self-elevate if not already running as Administrator ───────────────────
net session >nul 2>&1
if not "%errorlevel%"=="0" (
    echo Requesting administrator privileges...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs"
    exit /b
)

echo.
echo === GMix OBS plugin installer ===
echo OBS directory: %OBS_DIR%
echo.

if not exist "%PLUGIN_SRC%" (
    echo ERROR: %PLUGIN_SRC% not found.
    echo Build it first: cmake --build build --target obs-gmix-source
    pause
    exit /b 1
)
if not exist "%OBS_DIR%\bin\64bit\obs64.exe" (
    echo ERROR: obs64.exe not found under %OBS_DIR% -- edit OBS_DIR at the top of this script.
    pause
    exit /b 1
)

echo Closing OBS if it's running...
taskkill /IM obs64.exe /F >nul 2>&1

echo Removing old GMix plugin(s)...
if exist "%PLUGIN_DST_DIR%\gmix-obs-source.dll" (
    del /f /q "%PLUGIN_DST_DIR%\gmix-obs-source.dll"
    echo   removed gmix-obs-source.dll (old prototype)
)
if exist "%OBS_DIR%\data\obs-plugins\gmix-obs-source" (
    rmdir /s /q "%OBS_DIR%\data\obs-plugins\gmix-obs-source"
    echo   removed old prototype's data folder
)
if exist "%PLUGIN_DST_DIR%\obs-gmix-source.dll" (
    del /f /q "%PLUGIN_DST_DIR%\obs-gmix-source.dll"
    echo   removed previous obs-gmix-source.dll
)
if exist "%DATA_DST_DIR%" (
    rmdir /s /q "%DATA_DST_DIR%"
    echo   removed previous obs-gmix-source data folder
)

echo Installing new plugin...
copy /y "%PLUGIN_SRC%" "%PLUGIN_DST_DIR%\" >nul
if not "%errorlevel%"=="0" (
    echo ERROR: failed to copy the plugin DLL.
    pause
    exit /b 1
)
mkdir "%DATA_DST_DIR%\locale" >nul 2>&1
copy /y "%LOCALE_SRC%" "%DATA_DST_DIR%\locale\" >nul

echo.
echo Done. Installed:
echo   %PLUGIN_DST_DIR%\obs-gmix-source.dll
echo   %DATA_DST_DIR%\locale\en-US.ini
echo.
echo Launching OBS...
start "" "%OBS_DIR%\bin\64bit\obs64.exe"

pause
