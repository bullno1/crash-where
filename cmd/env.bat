@rem Called by every cmd\<toolchain>\*.bat script; the caller sets HERE first.
@rem The Visual Studio generator is multi-config, so the build tree is shared
@rem by all configurations and only the output directory carries BUILD_TYPE.
@set "_HERE=%HERE:~0,-1%"
@for %%D in ("%_HERE%\..\..") do @set "ROOT=%%~fD"
@for %%D in ("%_HERE%") do @set "TOOLCHAIN=%%~nxD"
@set "_HERE="
@if not defined BUILD_TYPE set "BUILD_TYPE=RelWithDebInfo"
@set "BUILD_DIR=%ROOT%\.build\%TOOLCHAIN%"
@set "BIN_DIR=%ROOT%\bin\%TOOLCHAIN%\%BUILD_TYPE%"
