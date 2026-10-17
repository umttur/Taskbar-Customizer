@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Taskbar Customizer - Release Build
cd /d "%~dp0"

rem Discover Visual Studio without relying on PATH or a version-specific folder.
set "TC_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%TC_VSWHERE%" (
    echo ERROR: Visual Studio Installer was not found.
    goto :failed
)
set "TC_VSROOT="
for /f "usebackq delims=" %%I in (`"%TC_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TC_VSROOT=%%I"
if not defined TC_VSROOT (
    echo ERROR: Install the Visual Studio Desktop development with C++ workload.
    goto :failed
)
if not exist "%TC_VSROOT%\Common7\Tools\VsDevCmd.bat" (
    echo ERROR: Visual Studio developer environment was not found.
    goto :failed
)
call "%TC_VSROOT%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 goto :failed

set "TC_CMAKE=%TC_VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%TC_CMAKE%" (
    set "TC_CMAKE="
    for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined TC_CMAKE set "TC_CMAKE=%%I"
)
if not defined TC_CMAKE (
    echo ERROR: Install the C++ CMake tools for Windows component.
    goto :failed
)

rem Keep the CMake cache and all generated outputs inside _build.
rem No existing build folders are deleted or cleaned.
rem Exit the application and restart Explorer before replacing a loaded DLL.
echo Configuring Release x64...
"%TC_CMAKE%" -S "%~dp0." -B "%~dp0_build" -A x64
if errorlevel 1 goto :failed
echo Building EXE and DLL...
"%TC_CMAKE%" --build "%~dp0_build" --config Release
if errorlevel 1 goto :failed

echo.
echo BUILD SUCCEEDED
echo Output folder: "%~dp0_build\Release"
echo Files: TaskbarCustomizer.exe and TaskbarCustomizerXaml.dll
echo The application was not started automatically.
echo Before rebuilding, exit the app and restart Explorer to release the DLL.
pause
exit /b 0

:failed
echo.
echo BUILD FAILED. Copy the error message above.
echo If the DLL is locked: exit TaskbarCustomizer, save your work,
echo then restart Windows Explorer manually before trying again.
pause
exit /b 1
