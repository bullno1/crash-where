@echo off
setlocal
set "HERE=%~dp0"
call "%HERE%..\env.bat"

call "%HERE%build.bat" || exit /b 1
if not exist "%BUILD_DIR%\test" mkdir "%BUILD_DIR%\test"
cd /d "%BUILD_DIR%\test"
"%BIN_DIR%\cw_test.exe" %*
