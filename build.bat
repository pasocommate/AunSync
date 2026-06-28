@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 > nul
title AunSync Build Script

for %%I in ("%~dp0.") do set "PLUGIN_DIR=%%~fI"
set "PLUGIN_VERSION=unknown"
if exist "%PLUGIN_DIR%\CMakeLists.txt" (
    for /f "usebackq tokens=4 delims= " %%V in (`findstr /B /C:"project(aunsync VERSION " "%PLUGIN_DIR%\CMakeLists.txt"`) do (
        set "PLUGIN_VERSION=%%V"
    )
)
set "PLUGIN_VERSION=%PLUGIN_VERSION:)=%"
set "BANNER_VERSION="
if not "%PLUGIN_VERSION%"=="unknown" set "BANNER_VERSION= v%PLUGIN_VERSION%"

echo ================================================================
echo  AunSync%BANNER_VERSION%  Build Script
echo ================================================================
echo.

set "OBS_SOURCE_DIR="
set "OBS_LEGACY_INSTALL="
set "OBS_CI="
set "CLEAN_BUILD=0"
set "OBS_STUDIO_REF=unknown"
if exist "%PLUGIN_DIR%\OBS_STUDIO_REF" (
    for /f "usebackq" %%R in ("%PLUGIN_DIR%\OBS_STUDIO_REF") do set "OBS_STUDIO_REF=%%R"
)
set "ENV_FILE=%PLUGIN_DIR%\build.env"
if exist "%ENV_FILE%" (
    for /f "usebackq eol=# tokens=1,* delims==" %%A in ("%ENV_FILE%") do (
        if /I "%%A"=="OBS_SOURCE_DIR" if not "%%B"=="" set "OBS_SOURCE_DIR=%%B"
        if /I "%%A"=="OBS_LEGACY_INSTALL" if not "%%B"=="" set "OBS_LEGACY_INSTALL=%%B"
        if /I "%%A"=="OBS_CI" if not "%%B"=="" set "OBS_CI=%%B"
    )
)

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="--clean" (
    set "CLEAN_BUILD=1"
    shift
    goto :parse_args
)
if /I "%~1"=="/clean" (
    set "CLEAN_BUILD=1"
    shift
    goto :parse_args
)
if "%OBS_SOURCE_DIR%"=="" set "OBS_SOURCE_DIR=%~1"
shift
goto :parse_args

:args_done
if "%OBS_SOURCE_DIR%"=="" set "OBS_SOURCE_DIR=%PLUGIN_DIR%\third_party\obs-studio"
if "%OBS_LEGACY_INSTALL%"=="" set OBS_LEGACY_INSTALL=0
set OBS_INSTALL_DIR=C:\ProgramData\obs-studio
if "%OBS_LEGACY_INSTALL%"=="1" set OBS_INSTALL_DIR=C:\Program Files\obs-studio
set BUILD_DIR=%PLUGIN_DIR%\build
set CI_MODE=0
if /I "%OBS_CI%"=="1" set CI_MODE=1
if /I "%CI%"=="true" set CI_MODE=1
if /I "%CI%"=="1" set CI_MODE=1

echo [Step 0] Checking environment...
where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] cmake not found. Install CMake 3.16+ from: https://cmake.org/download/
    goto :error
)
echo   cmake: OK

where git >nul 2>&1
if errorlevel 1 (
    echo [ERROR] git not found. Install Git for Windows from: https://git-scm.com/
    goto :error
)
echo   git: OK

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo [ERROR] Visual Studio not found.
    goto :error
)
echo   Visual Studio: OK
echo.

echo ================================================================
echo  [Step 1] OBS Studio %OBS_STUDIO_REF%...
echo ================================================================

:: --- clone ---
if not exist "%OBS_SOURCE_DIR%\.git" (
    echo   OBS Studio not found. Cloning. This may take a few minutes...
    git clone --depth 1 --branch %OBS_STUDIO_REF% --recurse-submodules https://github.com/obsproject/obs-studio.git "%OBS_SOURCE_DIR%"
    if errorlevel 1 (
        echo [ERROR] Failed to clone OBS Studio.
        goto :error
    )
    echo   Clone complete.
    echo.
)

:: --- build ---
if exist "%OBS_SOURCE_DIR%\build_x64\libobs\RelWithDebInfo\obs.lib" goto :obs_found
if exist "%OBS_SOURCE_DIR%\build_x64\libobs\Debug\obs.lib" goto :obs_found
echo   OBS library not found. Building OBS Studio (this may take 30+ minutes)...
pushd "%OBS_SOURCE_DIR%"
cmake --preset windows-x64
if errorlevel 1 (
    popd
    echo [ERROR] OBS Studio CMake configure failed.
    goto :error
)
cmake --build build_x64 --config RelWithDebInfo --parallel
if errorlevel 1 (
    popd
    echo [ERROR] OBS Studio build failed.
    goto :error
)
popd

if not exist "%OBS_SOURCE_DIR%\build_x64\libobs\RelWithDebInfo\obs.lib" (
    echo [ERROR] Build succeeded but OBS library not found.
    goto :error
)

:obs_found
echo   OBS library: OK (%OBS_SOURCE_DIR%)
echo.

if "%CLEAN_BUILD%"=="1" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set OBS_PLUGIN_LEGACY_INSTALL=OFF
if "%OBS_LEGACY_INSTALL%"=="1" set OBS_PLUGIN_LEGACY_INSTALL=ON

echo ================================================================
echo  [Step 2] Running CMake configure...
echo ================================================================
cmake -S "%PLUGIN_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DOBS_SOURCE_DIR="%OBS_SOURCE_DIR%" -DOBS_PLUGIN_LEGACY_INSTALL=%OBS_PLUGIN_LEGACY_INSTALL%
if errorlevel 1 goto :error

echo.
echo ================================================================
echo  [Step 3] Building plugin...
echo ================================================================
cmake --build "%BUILD_DIR%" --config RelWithDebInfo --parallel
if errorlevel 1 goto :error

if not exist "%BUILD_DIR%\RelWithDebInfo\aunsync.dll" (
    echo [ERROR] DLL was not generated.
    goto :error
)
echo   Build OK: %BUILD_DIR%\RelWithDebInfo\aunsync.dll

if "%CI_MODE%"=="1" goto :skip_install

echo.
echo ================================================================
echo  [Step 4] Installing to OBS...
echo ================================================================
if "%OBS_LEGACY_INSTALL%"=="1" goto :legacy_paths
set PLUGIN_DEST=%OBS_INSTALL_DIR%\plugins\aunsync\bin\64bit
set LOCALE_DEST=%OBS_INSTALL_DIR%\plugins\aunsync\data\locale
goto :paths_set
:legacy_paths
set PLUGIN_DEST=%OBS_INSTALL_DIR%\obs-plugins\64bit
set LOCALE_DEST=%OBS_INSTALL_DIR%\data\obs-plugins\aunsync\locale
:paths_set

if not exist "%PLUGIN_DEST%" mkdir "%PLUGIN_DEST%"
if not exist "%LOCALE_DEST%" mkdir "%LOCALE_DEST%"
copy /Y "%BUILD_DIR%\RelWithDebInfo\aunsync.dll" "%PLUGIN_DEST%\"
if errorlevel 1 goto :error
copy /Y "%PLUGIN_DIR%\data\locale\*.ini" "%LOCALE_DEST%\"
if errorlevel 1 goto :error

echo.
echo ================================================================
echo  SUCCESS: Build and install complete!
echo ================================================================
echo.
if "%CI_MODE%"=="1" exit /b 0
pause
exit /b 0

:skip_install
echo.
echo ================================================================
echo  SUCCESS: Build complete (CI mode, install skipped).
echo ================================================================
echo.
exit /b 0

:error
echo.
echo ================================================================
echo  ERROR: Build failed.
echo ================================================================
echo.
if "%CI_MODE%"=="1" exit /b 1
pause
exit /b 1
