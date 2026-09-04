@echo off
setlocal enabledelayedexpansion

echo AcuVoice SAPI5 build
echo.

set BUILD_DIR_X86=build_x86
set BUILD_DIR_X64=build_x64
set OUTPUT_DIR=output
set VERSION=1.1.1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2022 Build Tools or later.
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALLDIR=%%i"
if not defined VSINSTALLDIR (
    echo ERROR: no Visual Studio installation with the C++ toolset was found.
    exit /b 1
)
echo Visual Studio: %VSINSTALLDIR%
echo.

rem A worker left running by an earlier test keeps avcore.dll -- and output\ -- open.
taskkill /F /IM AcuVoiceServer.exe >nul 2>&1

echo === Building x64 (the product) ===
cmake -A x64 -S . -B %BUILD_DIR_X64% || exit /b 1
cmake --build %BUILD_DIR_X64% --config Release || exit /b 1
echo.

echo === Building x86 (the engine worker and the dll 32-bit hosts load) ===
cmake -A Win32 -S . -B %BUILD_DIR_X86% || exit /b 1
cmake --build %BUILD_DIR_X86% --config Release || exit /b 1
echo.

echo === Staging %OUTPUT_DIR% ===
if exist %OUTPUT_DIR%\x64 rmdir /s /q %OUTPUT_DIR%\x64
if not exist %OUTPUT_DIR% mkdir %OUTPUT_DIR%
if not exist %OUTPUT_DIR%\x86 mkdir %OUTPUT_DIR%\x86

rem 64-bit at the root: this is what the user runs and what a 64-bit host loads.
copy /Y "%BUILD_DIR_X64%\bin\Release\AcuVoiceSAPI.dll"        "%OUTPUT_DIR%\"     >nul || exit /b 1
copy /Y "%BUILD_DIR_X64%\bin\Release\AcuVoiceConfig.exe"      "%OUTPUT_DIR%\"     >nul || exit /b 1
copy /Y "%BUILD_DIR_X64%\bin\Release\AcuVoiceDiagnostics.exe" "%OUTPUT_DIR%\"     >nul || exit /b 1

rem 32-bit in x86\: only what has to be. AcuVoiceServer.exe is the one process that can
rem hold avcore.dll; the dll beside it is for hosts that are themselves 32-bit.
copy /Y "%BUILD_DIR_X86%\bin\Release\AcuVoiceServer.exe"      "%OUTPUT_DIR%\x86\" >nul || exit /b 1
copy /Y "%BUILD_DIR_X86%\bin\Release\AcuVoiceSAPI.dll"        "%OUTPUT_DIR%\x86\" >nul || exit /b 1
copy /Y "%BUILD_DIR_X86%\bin\Release\AcuVoiceDiagnostics.exe" "%OUTPUT_DIR%\x86\" >nul || exit /b 1
echo.

if not exist "engine\Lib\avcore.dll" (
    echo WARNING: engine\Lib\avcore.dll is missing, so there is nothing for the installer
    echo          to package. The AcuVoice engine is not redistributable source and is
    echo          not in this repository; see README.md.
    goto :done
)

echo === Building the installer ===
set "ISCC="
for %%p in (
    "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
    "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
    "%ProgramFiles%\Inno Setup 6\ISCC.exe"
) do (
    if not defined ISCC if exist %%p set "ISCC=%%~p"
)

if not defined ISCC (
    echo WARNING: Inno Setup 6 not found; skipping the installer.
    echo          The staged files in %OUTPUT_DIR% are complete and usable.
    goto :done
)

"%ISCC%" /Q "installer\AcuVoiceSAPI.iss" || exit /b 1
echo.

:done
echo Build finished.
if exist "%OUTPUT_DIR%\AcuVoiceSAPI5_Setup_%VERSION%.exe" echo Installer: %OUTPUT_DIR%\AcuVoiceSAPI5_Setup_%VERSION%.exe
endlocal
