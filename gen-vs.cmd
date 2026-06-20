@echo off
rem Generates (or refreshes) the Visual Studio solution at build\vs\Dungeon.slnx.

setlocal
pushd "%~dp0"

set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
set CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
rem Put the VS Installer (vswhere.exe) on PATH: VsDevCmd.bat (called by vcvars64)
rem runs a bare "vswhere.exe" relying on the current dir, which fails when
rem NoDefaultCurrentDirectoryInExePath=1 is set in the environment. PATH lookup
rem works regardless and silences the "'vswhere.exe' is not recognized" warning.
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul

"%CMAKE%" --preset vs
if errorlevel 1 exit /b 1

echo.
echo Solution ready: %~dp0build\vs\Dungeon.slnx
exit /b 0
