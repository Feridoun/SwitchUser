@echo off
rem Builds SwitchUser.exe (x64, static CRT, no dependencies). Needs Visual Studio 2019+ with the C++ workload.
setlocal enabledelayedexpansion
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" echo Could not find vswhere.exe. Install Visual Studio 2019+ with the C++ workload. & exit /b 1
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR echo Visual Studio with C++ tools not found. & exit /b 1
call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /O2 /W4 /EHsc /MT /DUNICODE /D_UNICODE SwitchUser.cpp /link /SUBSYSTEM:WINDOWS /OUT:SwitchUser.exe || exit /b 1
del /q SwitchUser.obj 2>nul
echo Built %~dp0SwitchUser.exe
