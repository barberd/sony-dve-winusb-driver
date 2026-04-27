@echo off
REM Install IcdUsb WinUSB replacement DLLs for Sony Digital Voice Editor 3.
REM Run as Administrator.
REM
REM This replaces the original Sony USB DLLs in SysWOW64 with our WinUSB
REM replacements, and disables the Roxio PxHlpa64.sys driver that causes BSODs
REM on modern Windows.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Run this script as Administrator.
    echo Right-click and select "Run as administrator".
    pause
    exit /b 1
)

set SRC=%~dp0
set DST=%SystemRoot%\SysWOW64
set DRV=%SystemRoot%\System32\drivers\PxHlpa64.sys

echo Installing IcdUsb WinUSB replacement DLLs...

for %%F in (ICDUSB.DLL ICDUSB2.DLL ICDUSB3.DLL) do (
    if not exist "%SRC%%%F" (
        echo ERROR: %SRC%%%F not found. Build the DLLs first.
        pause
        exit /b 1
    )
    copy /y "%SRC%%%F" "%DST%\%%F" >nul
    if errorlevel 1 (
        echo ERROR: Failed to copy %%F
        pause
        exit /b 1
    )
    echo   %%F -^> %DST%\%%F
)

if exist "%DRV%" (
    ren "%DRV%" PxHlpa64.sys.disabled
    if errorlevel 1 (
        echo WARNING: Could not rename PxHlpa64.sys. It may be in use.
    ) else (
        echo   Renamed PxHlpa64.sys -^> PxHlpa64.sys.disabled
    )
) else (
    echo   PxHlpa64.sys already disabled or not present.
)

echo.
echo Done. Now install WinUSB driver via Zadig if not already done.
pause
