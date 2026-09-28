@echo off
setlocal
set "HERE=%~dp0"
call "%HERE%..\env.bat"
call "%HERE%vcvars.bat" || exit /b 1

if exist "%BUILD_DIR%\crash_where.sln" goto build
call "%HERE%configure.bat" || exit /b 1

:build
cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% --parallel
