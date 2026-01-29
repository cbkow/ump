@echo off
echo Generating MSVC import library for libmpv...

REM Check if lib.exe is available
where lib.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: lib.exe not found. Please run this from a Visual Studio Developer Command Prompt.
    exit /b 1
)

REM Check if dumpbin.exe is available
where dumpbin.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: dumpbin.exe not found. Please run this from a Visual Studio Developer Command Prompt.
    exit /b 1
)

cd /d "%~dp0"

REM Extract exports from DLL
echo Extracting exports from libmpv-2.dll...
dumpbin /exports bin\libmpv-2.dll > exports.txt

REM Create DEF file
echo Creating mpv.def...
echo LIBRARY libmpv-2 > mpv.def
echo EXPORTS >> mpv.def

REM Parse exports (skip header lines, extract function names)
for /f "skip=19 tokens=4" %%i in (exports.txt) do (
    if not "%%i"=="" (
        if not "%%i"=="name" (
            echo %%i >> mpv.def
        )
    )
)

REM Generate LIB file
echo Generating mpv.lib...
lib /def:mpv.def /out:lib\mpv.lib /machine:x64

REM Cleanup
del exports.txt
del mpv.def
del mpv.exp

echo Done! Generated lib\mpv.lib
