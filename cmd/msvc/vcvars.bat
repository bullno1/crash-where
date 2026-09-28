@echo off
rem Enters the Visual Studio 2026 x64 build environment so that cmake and
rem msbuild are on PATH. A no-op when already inside one.
if "%VisualStudioVersion%"=="18.0" exit /b 0

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto locate
echo vcvars: vswhere.exe not found, install Visual Studio 2026 >&2
exit /b 1

:locate
set "VSDIR="
"%VSWHERE%" -latest -products * -version "[18.0,19.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\cw-vsdir.txt"
set /p VSDIR=<"%TEMP%\cw-vsdir.txt"
del "%TEMP%\cw-vsdir.txt"
if defined VSDIR goto enter
echo vcvars: no Visual Studio 2026 with the C++ x64 tools found >&2
exit /b 1

:enter
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
