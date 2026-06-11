@echo off
rem Generates (or refreshes) the Visual Studio solution at build\vs\Dungeon.slnx.

setlocal
pushd "%~dp0"

set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
set CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul

"%CMAKE%" --preset vs
if errorlevel 1 exit /b 1

echo.
echo Solution ready: %~dp0build\vs\Dungeon.slnx
exit /b 0
