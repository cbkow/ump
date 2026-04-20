@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: QCView MSIX Package Builder
:: ============================================================
:: Prerequisites:
::   - Windows 10 SDK (for MakeAppx.exe)
::   - Build QCView Release first: cmake --build build --config Release -j
::
:: Usage:
::   cd installer\msix
::   build_msix.cmd
:: ============================================================

set "ROOT=%~dp0..\..\"
set "BUILD_DIR=%ROOT%build\Release"
set "STAGING=%~dp0staging"
set "MSIX_DIR=%~dp0"
set "OUTPUT=%~dp0QCView-1.1.6-win64.msix"

:: Locate MakeAppx.exe and MakePri.exe from Windows SDK
set "MAKEAPPX="
set "MAKEPRI="
for /f "delims=" %%i in ('where /r "C:\Program Files (x86)\Windows Kits\10\bin" MakeAppx.exe 2^>nul') do (
    echo %%i | findstr /i "\\x64\\" >nul && set "MAKEAPPX=%%i"
)
for /f "delims=" %%i in ('where /r "C:\Program Files (x86)\Windows Kits\10\bin" MakePri.exe 2^>nul') do (
    echo %%i | findstr /i "\\x64\\" >nul && set "MAKEPRI=%%i"
)
if not defined MAKEAPPX (
    echo ERROR: MakeAppx.exe not found. Install the Windows 10 SDK.
    exit /b 1
)
if not defined MAKEPRI (
    echo ERROR: MakePri.exe not found. Install the Windows 10 SDK.
    exit /b 1
)
echo Found MakeAppx: %MAKEAPPX%
echo Found MakePri:  %MAKEPRI%

:: Verify build output exists
if not exist "%BUILD_DIR%\qcview.exe" (
    echo ERROR: qcview.exe not found in %BUILD_DIR%
    echo Build the project first: cmake --build build --config Release -j
    exit /b 1
)

:: Clean previous staging
if exist "%STAGING%" rmdir /s /q "%STAGING%"
mkdir "%STAGING%"

echo Staging files...

:: Copy executable, ffmpeg, and all DLLs
copy /y "%BUILD_DIR%\qcview.exe" "%STAGING%\" >nul
if exist "%BUILD_DIR%\ffmpeg.exe" copy /y "%BUILD_DIR%\ffmpeg.exe" "%STAGING%\" >nul
for %%f in ("%BUILD_DIR%\*.dll") do copy /y "%%f" "%STAGING%\" >nul

:: Copy assets folder
if exist "%BUILD_DIR%\assets" (
    xcopy /s /e /i /q /y "%BUILD_DIR%\assets" "%STAGING%\assets" >nul
)

:: Copy MSIX tile images
xcopy /s /e /i /q /y "%MSIX_DIR%images" "%STAGING%\images" >nul

:: Copy manifest
copy /y "%MSIX_DIR%AppxManifest.xml" "%STAGING%\" >nul

:: Copy license files if they exist
if exist "%ROOT%LICENSE.txt" copy /y "%ROOT%LICENSE.txt" "%STAGING%\" >nul
if exist "%ROOT%LICENSE" copy /y "%ROOT%LICENSE" "%STAGING%\LICENSE.txt" >nul
if exist "%ROOT%THIRD_PARTY_NOTICES.txt" copy /y "%ROOT%THIRD_PARTY_NOTICES.txt" "%STAGING%\" >nul

:: Generate resources.pri so Windows can resolve asset qualifiers
:: (targetsize, altform-unplated, scale, etc.)
echo Generating resources.pri...
"%MAKEPRI%" createconfig /cf "%STAGING%\priconfig.xml" /dq "en-us" /o
"%MAKEPRI%" new /pr "%STAGING%" /cf "%STAGING%\priconfig.xml" /mn "%STAGING%\AppxManifest.xml" /of "%STAGING%\resources.pri" /o
if %ERRORLEVEL% neq 0 (
    echo ERROR: MakePri failed.
    exit /b 1
)
:: Clean up the config file — only resources.pri is needed in the package
del /q "%STAGING%\priconfig.xml"

echo Packing MSIX...

:: Remove old package
if exist "%OUTPUT%" del /q "%OUTPUT%"

:: Pack
"%MAKEAPPX%" pack /d "%STAGING%" /p "%OUTPUT%" /o

if %ERRORLEVEL% neq 0 (
    echo ERROR: MakeAppx pack failed.
    exit /b 1
)

echo.
echo SUCCESS: %OUTPUT%
echo.

:: ── Certificate generation and signing ──
:: If winapp CLI is installed (winget install Microsoft.winappcli), you can
:: generate a dev certificate and sign in one step:
::
::   winapp cert generate --if-exists skip
::   winapp cert install .\devcert.pfx
::   signtool sign /fd SHA256 /a /f devcert.pfx "%OUTPUT%"
::
:: Or use winapp pack as a simpler alternative to this entire script:
::
::   winapp pack .\staging --cert .\devcert.pfx
::
:: For Store submission, Microsoft signs the MSIX — no local cert needed.

:: Clean up staging
rmdir /s /q "%STAGING%"

endlocal
