@echo off
rem Build helper: sets up the VS developer environment, configures, and builds.
rem Usage: build.cmd [debug|release]   (default: debug)

setlocal
pushd "%~dp0"
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=debug

set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
set CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

"%CMAKE%" --preset %CONFIG%
if errorlevel 1 exit /b 1

"%CMAKE%" --build --preset %CONFIG%
exit /b %errorlevel%
