@echo off
setlocal enabledelayedexpansion

set PROJECT_DIR=%~dp0..
set BUILD_DIR=%PROJECT_DIR%\build\x64-release

:: Extract version from CMakeLists.txt (e.g. "project(TenBox VERSION 0.1.0 ...")
for /f "tokens=3" %%v in ('findstr /c:"VERSION" "%PROJECT_DIR%\CMakeLists.txt" ^| findstr /r "^project"') do set VERSION=%%v

if "%VERSION%"=="" (
    echo ERROR: Could not extract version from CMakeLists.txt
    exit /b 1
)
echo Building TenBox v%VERSION% MSI installer...

:: Initialize VS build environment (cmake, ninja, cl, etc.)
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "delims=" %%i in ('%VSWHERE% -latest -property installationPath') do set VS_PATH=%%i
if not exist "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo ERROR: Could not find vcvarsall.bat. Is Visual Studio installed?
    exit /b 1
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

:: Step 1: CMake configure + build (Release)
echo.
echo [1/2] CMake Release build...
cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo ERROR: CMake build failed.
    exit /b 1
)

:: Step 2: Build MSI with WiX
echo.
echo [2/2] Building MSI with WiX...
set WIX_PATH=C:\Program Files\WiX Toolset v6.0\bin\wix.exe
if not exist "%WIX_PATH%" (
    echo ERROR: WiX not found at %WIX_PATH%
    exit /b 1
)
"%WIX_PATH%" build "%PROJECT_DIR%\installer\TenBox.wxs" ^
    "%PROJECT_DIR%\installer\zh-CN.wxl" ^
    -arch x64 ^
    -culture zh-CN ^
    -ext WixToolset.UI.wixext ^
    -ext WixToolset.Util.wixext ^
    -d ProductVersion=%VERSION% ^
    -d BuildDir=%BUILD_DIR% ^
    -d ProjectDir=%PROJECT_DIR% ^
    -o "%BUILD_DIR%\TenBox_%VERSION%.msi"
if errorlevel 1 (
    echo ERROR: WiX build failed.
    exit /b 1
)

echo.
echo Success: %BUILD_DIR%\TenBox_%VERSION%.msi
