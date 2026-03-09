@echo off
setlocal enabledelayedexpansion

set PROJECT_DIR=%~dp0..
set BUILD_DIR=%PROJECT_DIR%\build\x64-release

:: Read version from the single-source-of-truth VERSION file
set /p VERSION=<"%PROJECT_DIR%\VERSION"

if "%VERSION%"=="" (
    echo ERROR: Could not read version from %PROJECT_DIR%\VERSION
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
echo [1/4] CMake Release build...
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

:: Step 2: Sign exe files (signtool from Windows SDK or PATH)
:: Certificate thumbprint: set env SIGNING_SHA1, or create scripts\.signing_sha1 (one line, thumbprint; add to .gitignore)
if not defined SIGNING_SHA1 (
    if exist "%~dp0.signing_sha1" (
        for /f "usebackq delims=" %%i in ("%~dp0.signing_sha1") do set "SIGNING_SHA1=%%i"
    )
)
if not defined SIGNING_SHA1 (
    echo ERROR: Set SIGNING_SHA1 env var, or create scripts\.signing_sha1 with your cert thumbprint ^(SHA1^).
    exit /b 1
)
echo.
echo [2/4] Signing exe files...
set SIGNTOOL=signtool
set SIGN_ARGS=/v /fd sha256 /sha1 %SIGNING_SHA1% /tr http://rfc3161timestamp.globalsign.com/advanced /td sha256
"%SIGNTOOL%" sign %SIGN_ARGS% "%BUILD_DIR%\tenbox-manager.exe"
if errorlevel 1 (
    echo ERROR: Signing tenbox-manager.exe failed.
    exit /b 1
)
"%SIGNTOOL%" sign %SIGN_ARGS% "%BUILD_DIR%\tenbox-vm-runtime.exe"
if errorlevel 1 (
    echo ERROR: Signing tenbox-vm-runtime.exe failed.
    exit /b 1
)

:: Step 3: Build MSI with WiX
echo.
echo [3/4] Building MSI with WiX...
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

:: Step 4: Sign MSI installer
echo.
echo [4/4] Signing MSI...
"%SIGNTOOL%" sign %SIGN_ARGS% "%BUILD_DIR%\TenBox_%VERSION%.msi"
if errorlevel 1 (
    echo ERROR: Signing MSI failed.
    exit /b 1
)

echo.
echo Success: %BUILD_DIR%\TenBox_%VERSION%.msi
