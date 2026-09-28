@echo off
setlocal
set "HERE=%~dp0"
call "%HERE%..\env.bat"
call "%HERE%vcvars.bat" || exit /b 1

:loop
call "%HERE%test.bat" %*
powershell -NoProfile -Command ^
	"$w = New-Object IO.FileSystemWatcher '%ROOT%'; $w.IncludeSubdirectories = $true;" ^
	"do { $r = $w.WaitForChanged('All') } while ($r.Name -notmatch '^(src|include|test|samples|cmake)\\|^CMakeLists\.txt$');" ^
	"Start-Sleep -Milliseconds 200"
goto loop
