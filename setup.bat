@echo off
setlocal EnableExtensions
title hsh setup (v1)

rem ===================================================================
rem  hsh setup (v1)
rem    setup.bat              -> menu
rem    setup.bat install      -> install the ready-made hsh.exe
rem    setup.bat build        -> build from source, then install
rem    setup.bat uninstall    -> remove hsh and its PATH entry
rem  No administrator rights needed (winget may ask for one when it
rem  installs CMake; accept that prompt).
rem ===================================================================

set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
if not defined HSH_DEST set "HSH_DEST=%LOCALAPPDATA%\hsh"
set "DEST=%HSH_DEST%"

echo.
echo  ============================================
echo   hsh - bash-style shell for native Windows
echo   Version v1 (hsh 0.1)
echo  ============================================
echo.

if /i "%~1"=="install"   goto :do_install
if /i "%~1"=="build"     goto :do_build
if /i "%~1"=="uninstall" goto :do_uninstall

echo   1) Install hsh        (uses the ready-made hsh.exe)
echo   2) Build from source  (installs CMake + MinGW compiler if missing)
echo   3) Uninstall
echo   4) Quit
echo.
choice /c 1234 /n /m "Choose 1-4: "
if errorlevel 4 goto :end_pause
if errorlevel 3 goto :do_uninstall
if errorlevel 2 goto :do_build
if errorlevel 1 goto :do_install
goto :end_pause

rem -------------------------------------------------------------------
:do_install
set "EXE="
if exist "%SRC%\hsh.exe" set "EXE=%SRC%\hsh.exe"
if not defined EXE if exist "%SRC%\build\hsh.exe" set "EXE=%SRC%\build\hsh.exe"
if not defined EXE goto :no_exe
call :install_exe "%EXE%"
if errorlevel 1 goto :end_pause
goto :finish

:no_exe
echo   [!] hsh.exe was not found next to setup.bat.
echo       Choose option 2 (Build from source), or download hsh.exe
echo       from the project Releases page and put it beside this file.
goto :end_pause

rem -------------------------------------------------------------------
:do_build
if not exist "%SRC%\CMakeLists.txt" goto :no_src
set "NEED_CMAKE="
set "NEED_GCC="
where cmake >nul 2>&1 || set "NEED_CMAKE=1"
where gcc >nul 2>&1 || set "NEED_GCC=1"
where mingw32-make >nul 2>&1 || set "NEED_GCC=1"
if not defined NEED_CMAKE if not defined NEED_GCC goto :do_compile

where winget >nul 2>&1
if errorlevel 1 goto :no_winget
echo   Installing missing build tools with winget. This can take a few minutes.
if defined NEED_CMAKE winget install -e --id Kitware.CMake --accept-package-agreements --accept-source-agreements
if defined NEED_GCC winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT --accept-package-agreements --accept-source-agreements
call :refresh_path
where cmake >nul 2>&1
if errorlevel 1 goto :need_restart
where gcc >nul 2>&1
if errorlevel 1 goto :need_restart
where mingw32-make >nul 2>&1
if errorlevel 1 goto :need_restart

:do_compile
echo.
echo   Building hsh ...
pushd "%SRC%"
if exist build rmdir /s /q build
cmake -S . -B build -G "MinGW Makefiles"
if errorlevel 1 goto :build_fail
cmake --build build
if errorlevel 1 goto :build_fail
popd
if not exist "%SRC%\build\hsh.exe" goto :build_fail
call :install_exe "%SRC%\build\hsh.exe"
if errorlevel 1 goto :end_pause
goto :finish

:build_fail
popd 2>nul
echo.
echo   [!] The build failed. Scroll up and copy the FIRST error message
echo       and send it to the project owner (see README, "If setup fails").
goto :end_pause

:no_src
echo   [!] CMakeLists.txt not found. Run setup.bat from the project folder.
goto :end_pause

:no_winget
echo   [!] winget was not found, so the build tools cannot be installed
echo       automatically. Install "App Installer" from the Microsoft Store,
echo       or install CMake (cmake.org) and WinLibs MinGW (winlibs.com)
echo       by hand, then run this setup again.
goto :end_pause

:need_restart
echo.
echo   [!] The tools were installed but this window cannot see them yet.
echo       Close this window, open a NEW one, run setup.bat again, choose 2.
goto :end_pause

rem -------------------------------------------------------------------
:do_uninstall
set "HSH_DEST=%DEST%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d=$env:HSH_DEST; $p=[Environment]::GetEnvironmentVariable('Path','User'); if(-not $p){$p=''}; $parts=@($p -split ';' | Where-Object { $_ -ne '' -and $_ -ne $d }); [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User'); Write-Host '  Removed from your user PATH.'"
if exist "%DEST%" rmdir /s /q "%DEST%"
echo   hsh was uninstalled. Open a new terminal window to refresh PATH.
goto :end_pause

rem -------------------------------------------------------------------
:finish
echo.
echo   Testing the install ...
"%DEST%\hsh.exe" -c "echo hsh is working | grep working"
echo.
echo   ------------------------------------------------------------
echo   DONE.  hsh is installed in:  %DEST%
echo   Close ALL terminal windows (every Windows Terminal window, not
echo   just one tab), open a new cmd or PowerShell, and type:   hsh
echo   ------------------------------------------------------------
goto :end_pause

rem ===================================================================
rem  subroutines
rem ===================================================================
:install_exe
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~1" "%DEST%\hsh.exe" >nul
if errorlevel 1 goto :copy_fail
set "HSH_DEST=%DEST%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Unblock-File -LiteralPath (Join-Path $env:HSH_DEST 'hsh.exe')" >nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d=$env:HSH_DEST; $p=[Environment]::GetEnvironmentVariable('Path','User'); if(-not $p){$p=''}; $parts=@($p -split ';' | Where-Object { $_ -ne '' }); if($parts -notcontains $d){ $parts += $d; [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User'); Write-Host '  Added to your user PATH.' } else { Write-Host '  Already on your user PATH.' }"
exit /b 0

:copy_fail
echo   [!] Could not copy hsh.exe to "%DEST%".
echo       Close any running hsh window and try again.
exit /b 1

:refresh_path
for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "[Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')"`) do set "PATH=%%P"
exit /b 0

:end_pause
echo.
if "%~1"=="" pause
endlocal
exit /b 0
