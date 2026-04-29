@echo off
setlocal enabledelayedexpansion

REM ============================================
REM Build Script for MapForGoblins DLL Mod
REM ============================================

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%build"

REM Find Visual Studio 2022 using vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2022.
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -latest`) do set "VS_INSTALL=%%i"
if not defined VS_INSTALL (
    echo ERROR: Visual Studio 2022 with C++ tools not found!
    exit /b 1
)
set "VS_PATH=%VS_INSTALL%\Common7\Tools\VsDevCmd.bat"

if /i "%~1"=="clean" goto :clean
if /i "%~1"=="configure" goto :configure
if /i "%~1"=="generate" goto :generate
if /i "%~1"=="snapshot" goto :snapshot
if /i "%~1"=="release" goto :release

REM Default: configure if needed, then build
call :ensure_configured
if errorlevel 1 exit /b 1

echo.
echo Building MapForGoblins...
echo ----------------------------------------

cmd /c "call "%VS_PATH%" -arch=amd64 >nul 2>&1 && cd /d "%BUILD_DIR%" && msbuild MapForGoblins.sln /p:Configuration=Release /p:Platform=x64 /t:MapForGoblins /v:minimal /m"

if errorlevel 1 (
    echo [FAILED] MapForGoblins
    exit /b 1
)

echo [SUCCESS] MapForGoblins
echo.
echo Output: %BUILD_DIR%\Release\MapForGoblins.dll
dir /b "%BUILD_DIR%\Release\MapForGoblins.*" 2>nul
echo.
exit /b 0

:generate
echo.
echo Running data pipeline (incremental, hash-cached)...
echo ============================================
py "%SCRIPT_DIR%tools\build_pipeline.py" %*
echo.
exit /b 0

:ensure_configured
if not exist "%BUILD_DIR%\MapForGoblins.sln" (
    echo Build not configured. Running configure...
    call :configure
    if errorlevel 1 exit /b 1
)
exit /b 0

:configure
echo.
echo Configuring CMake...
echo ============================================

if exist "%BUILD_DIR%" (
    rmdir /s /q "%BUILD_DIR%"
)
mkdir "%BUILD_DIR%"

call "%VS_PATH%" -arch=amd64 >nul 2>&1
cd /d "%SCRIPT_DIR%"
cmake -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo ERROR: CMake configuration failed!
    exit /b 1
)
echo CMake configuration complete.
exit /b 0

:snapshot
echo.
echo === Building snapshot (pre-release) ===

call :parse_version
if errorlevel 1 exit /b 1
echo Snapshot version: pre-%VER%

REM Incremental data pipeline (hash-based cache). Pass --force-all to rebuild everything.
echo Running incremental pipeline...
py "%SCRIPT_DIR%tools\build_pipeline.py" %*
if errorlevel 1 (
    echo [FAILED] build_pipeline.py
    exit /b 1
)

call :ensure_configured
if errorlevel 1 exit /b 1

echo.
echo Building MapForGoblins...
echo ----------------------------------------

REM /t:MapForGoblins:Rebuild forces clean rebuild of just the DLL target,
REM avoiding LTCG "copied from previous compilation" cache quirks that can
REM embed stale code (e.g. the PROJECT_VERSION macro not propagating).
cmd /c "call "%VS_PATH%" -arch=amd64 >nul 2>&1 && cd /d "%BUILD_DIR%" && msbuild MapForGoblins.sln /p:Configuration=Release /p:Platform=x64 /t:MapForGoblins:Rebuild /v:minimal /m"
if errorlevel 1 (
    echo [FAILED] Snapshot build
    exit /b 1
)

REM Package into pre-release folder
set "SNAP_DIR=%SCRIPT_DIR%pre-release"
if exist "%SNAP_DIR%" rmdir /s /q "%SNAP_DIR%"
mkdir "%SNAP_DIR%\dll\offline" 2>nul
mkdir "%SNAP_DIR%\addons\MapForGoblins\menu" 2>nul

copy /Y "%BUILD_DIR%\Release\MapForGoblins.dll" "%SNAP_DIR%\dll\offline\" >nul
copy /Y "%BUILD_DIR%\Release\MapForGoblins.ini" "%SNAP_DIR%\dll\offline\" >nul
copy /Y "%SCRIPT_DIR%assets\menu\02_120_worldmap_new.gfx" "%SNAP_DIR%\addons\MapForGoblins\menu\02_120_worldmap.gfx" >nul
powershell -NoProfile -Command "(Get-Content '%SCRIPT_DIR%assets\README.txt') -replace '%%VERSION%%','pre-%VER%' | Set-Content '%SNAP_DIR%\README.txt'"

echo.
echo [SUCCESS] Snapshot packaged: %SNAP_DIR% (pre-%VER%)
echo.
exit /b 0

:release
echo.
echo === Building release ===

call :parse_version
if errorlevel 1 exit /b 1

REM Split X.Y.Z
for /f "tokens=1,2,3 delims=." %%a in ("%VER%") do (
    set "V_MAJOR=%%a"
    set "V_MINOR=%%b"
    set "V_PATCH=%%c"
)
echo Release version: %VER% (%V_MAJOR%.%V_MINOR%.%V_PATCH%)

REM Incremental data pipeline (hash-based cache). Pass --force-all to rebuild everything.
echo Running incremental pipeline...
py "%SCRIPT_DIR%tools\build_pipeline.py" %*
if errorlevel 1 (
    echo [FAILED] build_pipeline.py
    exit /b 1
)

REM Build with VERSION_PRE="" (release, no pre- prefix)
call "%VS_PATH%" -arch=amd64 >nul 2>&1
cd /d "%SCRIPT_DIR%"
cmake -B build -G "Visual Studio 17 2022" -A x64 -DVERSION_PRE=""
if errorlevel 1 (
    echo ERROR: CMake configuration failed!
    exit /b 1
)

cd /d "%BUILD_DIR%"
REM Rebuild forces a clean LTCG pass so the version macro and any recently
REM changed sources are actually re-emitted into the DLL.
msbuild MapForGoblins.sln /p:Configuration=Release /p:Platform=x64 /t:MapForGoblins:Rebuild /v:minimal /m
if errorlevel 1 (
    echo [FAILED] Release build
    exit /b 1
)
cd /d "%SCRIPT_DIR%"

REM Package into release folder
set "REL_DIR=%SCRIPT_DIR%ERR - MapForGoblins - DLL - v%VER%"
mkdir "%REL_DIR%\dll\offline" 2>nul
mkdir "%REL_DIR%\addons\MapForGoblins\menu" 2>nul

copy /Y "%BUILD_DIR%\Release\MapForGoblins.dll" "%REL_DIR%\dll\offline\" >nul
copy /Y "%BUILD_DIR%\Release\MapForGoblins.ini" "%REL_DIR%\dll\offline\" >nul
copy /Y "%SCRIPT_DIR%assets\menu\02_120_worldmap_new.gfx" "%REL_DIR%\addons\MapForGoblins\menu\02_120_worldmap.gfx" >nul
powershell -NoProfile -Command "(Get-Content '%SCRIPT_DIR%assets\README.txt') -replace '%%VERSION%%','%VER%' | Set-Content '%REL_DIR%\README.txt'"

echo.
echo Release packaged: %REL_DIR%

REM Bump patch version: X.Y.Z -> X.Y.(Z+1)
set /a "V_NEXT=%V_PATCH%+1"
set "NEXT_VER=%V_MAJOR%.%V_MINOR%.%V_NEXT%"
echo Bumping to pre-%NEXT_VER%...

REM Update CMakeLists.txt version
powershell -Command "(Get-Content '%SCRIPT_DIR%CMakeLists.txt') -replace '  VERSION   \"%VER%\"', '  VERSION   \"%NEXT_VER%\"' | Set-Content '%SCRIPT_DIR%CMakeLists.txt'"

REM Reconfigure with pre- prefix
cmake -B build -G "Visual Studio 17 2022" -A x64 >nul 2>&1

echo Done. Next dev version: pre-%NEXT_VER%
echo.
exit /b 0

:parse_version
set "VER="
for /f "tokens=2" %%v in ('findstr /C:"  VERSION   " "%SCRIPT_DIR%CMakeLists.txt"') do set "VER=%%~v"
if "%VER%"=="" (
    echo ERROR: Could not parse version from CMakeLists.txt
    exit /b 1
)
exit /b 0

:clean
echo Cleaning build directory...
if exist "%BUILD_DIR%" (
    rmdir /s /q "%BUILD_DIR%"
    echo Done.
) else (
    echo Nothing to clean.
)
exit /b 0
