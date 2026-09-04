@echo off
rem Runs a command inside the 32-bit MSVC environment:  msvc32.bat cl /nologo foo.c
setlocal
set "VCVARS="
for %%p in (
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
) do if not defined VCVARS if exist %%p set "VCVARS=%%~p"
if not defined VCVARS (echo ERROR: vcvarsall.bat not found & exit /b 1)
call "%VCVARS%" x86 >nul || exit /b 1
%*
