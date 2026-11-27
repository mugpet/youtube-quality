@echo off
setlocal EnableExtensions DisableDelayedExpansion
title YouTube Quality setup
set "YTQ_EXTENSION_DIR=%~dp0extension"
if not exist "%YTQ_EXTENSION_DIR%\manifest.json" goto missingExtension

set "YTQ_CHROME="
for %%H in (HKCU HKLM) do (
    for /f "tokens=2,*" %%A in ('reg.exe query "%%H\Software\Microsoft\Windows\CurrentVersion\App Paths\chrome.exe" /ve 2^>nul') do (
        if /i "%%A"=="REG_SZ" if not defined YTQ_CHROME if exist "%%~B" set "YTQ_CHROME=%%~B"
    )
)
for %%P in ("%ProgramFiles%\Google\Chrome\Application\chrome.exe" "%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe" "%LocalAppData%\Google\Chrome\Application\chrome.exe") do (
    if not defined YTQ_CHROME if exist "%%~P" set "YTQ_CHROME=%%~P"
)
if not defined YTQ_CHROME goto missingChrome
start "" "%YTQ_CHROME%" "chrome://extensions/"
if errorlevel 1 goto missingChrome

powershell.exe -NoProfile -NonInteractive -Command "Set-Clipboard -Value $env:YTQ_EXTENSION_DIR -ErrorAction Stop" >nul 2>nul
if errorlevel 1 goto manualPath
echo The extension folder path is on your clipboard.
goto instructions

:manualPath
echo Copy the folder path shown below.

:instructions
echo Folder: "%YTQ_EXTENSION_DIR%"
echo.
echo 1. In Chrome, turn on Developer mode.
echo 2. Click Load unpacked.
echo 3. Paste the path in the Folder field and choose Select Folder.
echo 4. Refresh any open YouTube tabs.
echo.
pause
exit /b 0

:missingExtension
echo The extension folder is missing. Keep Setup.cmd next to the extension
echo folder and extract both before running setup.
goto failed

:missingChrome
echo Open Google Chrome and enter chrome://extensions/ in the address bar.
echo Turn on Developer mode, click Load unpacked, and select this folder:
echo "%YTQ_EXTENSION_DIR%"
echo Then refresh your YouTube tabs.
:failed
echo.
pause
exit /b 1
