@echo off
setlocal
set "HERE=%~dp0"
call "%HERE%..\env.bat"
call "%HERE%vcvars.bat" || exit /b 1

rem The generator appends the configuration name, giving bin\msvc\<config>.
set "OUT=%ROOT:\=/%/bin/%TOOLCHAIN%"
cmake -S "%ROOT%" -B "%BUILD_DIR%" -G "Visual Studio 18 2026" -A x64 ^
	-DCMAKE_RUNTIME_OUTPUT_DIRECTORY="%OUT%" ^
	-DCMAKE_LIBRARY_OUTPUT_DIRECTORY="%OUT%" ^
	-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="%OUT%"
