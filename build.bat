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

REM Build profile: default = ERR; pass "--vanilla" or "--convergence" (any position).
REM   ERR:         data/, src/generated, build/, pre-release/
REM   vanilla:     data/vanilla, src/generated_vanilla, build-vanilla/, pre-release-vanilla/
REM   convergence: data/convergence, src/generated_convergence, build-convergence/, pre-release-convergence/
REM MFG_PROFILE is exported so build_pipeline.py + config.py pick the data source.
set "GEN_SUBDIR=generated"
set "PKG_PREFIX=ERR"
set "SNAP_DIR=%SCRIPT_DIR%pre-release"
set "DISP_PROFILE=err"
set "README_SRC=%SCRIPT_DIR%assets\README.txt"
set "GFX_SRC=%SCRIPT_DIR%assets\menu\02_120_worldmap_err.gfx"
echo %*| findstr /i /c:"--vanilla" >nul && set "MFG_PROFILE=vanilla"
echo %*| findstr /i /c:"--convergence" >nul && set "MFG_PROFILE=convergence"
echo %*| findstr /i /c:"--erte" >nul && set "MFG_PROFILE=erte"
if "%MFG_PROFILE%"=="vanilla" set "BUILD_DIR=%SCRIPT_DIR%build-vanilla"
if "%MFG_PROFILE%"=="vanilla" set "GEN_SUBDIR=generated_vanilla"
if "%MFG_PROFILE%"=="vanilla" set "PKG_PREFIX=Vanilla"
if "%MFG_PROFILE%"=="vanilla" set "SNAP_DIR=%SCRIPT_DIR%pre-release-vanilla"
if "%MFG_PROFILE%"=="vanilla" set "DISP_PROFILE=vanilla"
if "%MFG_PROFILE%"=="vanilla" set "README_SRC=%SCRIPT_DIR%assets\README_vanilla.txt"
if "%MFG_PROFILE%"=="vanilla" set "GFX_SRC=%SCRIPT_DIR%assets\menu\02_120_worldmap_vanilla.gfx"
if "%MFG_PROFILE%"=="convergence" set "BUILD_DIR=%SCRIPT_DIR%build-convergence"
if "%MFG_PROFILE%"=="convergence" set "GEN_SUBDIR=generated_convergence"
if "%MFG_PROFILE%"=="convergence" set "PKG_PREFIX=Convergence"
if "%MFG_PROFILE%"=="convergence" set "SNAP_DIR=%SCRIPT_DIR%pre-release-convergence"
if "%MFG_PROFILE%"=="convergence" set "DISP_PROFILE=convergence"
if "%MFG_PROFILE%"=="convergence" set "README_SRC=%SCRIPT_DIR%assets\README_convergence.txt"
if "%MFG_PROFILE%"=="convergence" set "GFX_SRC=%SCRIPT_DIR%assets\menu\02_120_worldmap_convergence.gfx"
if "%MFG_PROFILE%"=="erte" set "BUILD_DIR=%SCRIPT_DIR%build-erte"
if "%MFG_PROFILE%"=="erte" set "GEN_SUBDIR=generated_erte"
if "%MFG_PROFILE%"=="erte" set "PKG_PREFIX=ERTE"
if "%MFG_PROFILE%"=="erte" set "SNAP_DIR=%SCRIPT_DIR%pre-release-erte"
if "%MFG_PROFILE%"=="erte" set "DISP_PROFILE=erte"
if "%MFG_PROFILE%"=="erte" set "README_SRC=%SCRIPT_DIR%assets\README_erte.txt"
if "%MFG_PROFILE%"=="erte" set "GFX_SRC=%SCRIPT_DIR%assets\menu\02_120_worldmap_erte.gfx"
echo [PROFILE] %DISP_PROFILE%  build=%BUILD_DIR%  gen=%GEN_SUBDIR%

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
cmake -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DGENERATED_SUBDIR=%GEN_SUBDIR%
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
echo Snapshot version: %VER%

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

REM Package into pre-release folder (SNAP_DIR set per profile at top)
call :package "%SNAP_DIR%"
powershell -NoProfile -Command "(Get-Content '%README_SRC%') -replace '%%VERSION%%','%VER%' | Set-Content '%SNAP_DIR%\README.txt'"

echo.
echo [SUCCESS] Snapshot packaged: %SNAP_DIR% (%VER%)
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

REM Build the release DLL. Same version string as snapshots, so with /Brepro +
REM the same data this is byte-identical to the snapshot you already tested/scanned.
call "%VS_PATH%" -arch=amd64 >nul 2>&1
cd /d "%SCRIPT_DIR%"
cmake -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DGENERATED_SUBDIR=%GEN_SUBDIR%
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

REM Package into release folder (PKG_PREFIX = ERR or Vanilla per profile)
set "REL_DIR=%SCRIPT_DIR%%PKG_PREFIX% - MapForGoblins - DLL - v%VER%"
call :package "%REL_DIR%"
powershell -NoProfile -Command "(Get-Content '%README_SRC%') -replace '%%VERSION%%','%VER%' | Set-Content '%REL_DIR%\README.txt'"

echo.
echo Release packaged: %REL_DIR%

REM Bump patch version: X.Y.Z -> X.Y.(Z+1). Pass --no-bump to skip (used when
REM releasing BOTH profiles for the same version: run the first release with
REM --no-bump, the second one bumps once).
echo %*| findstr /i /c:"--no-bump" >nul && goto :release_nobump

set /a "V_NEXT=%V_PATCH%+1"
set "NEXT_VER=%V_MAJOR%.%V_MINOR%.%V_NEXT%"
echo Bumping to pre-%NEXT_VER%...

REM Update CMakeLists.txt version
powershell -Command "(Get-Content '%SCRIPT_DIR%CMakeLists.txt') -replace '  VERSION   \"%VER%\"', '  VERSION   \"%NEXT_VER%\"' | Set-Content '%SCRIPT_DIR%CMakeLists.txt'"

REM Reconfigure with pre- prefix
cmake -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DGENERATED_SUBDIR=%GEN_SUBDIR% >nul 2>&1

echo Done. Next dev version: %NEXT_VER%
echo.
exit /b 0

:release_nobump
REM Reconfigure at the SAME version (no bump)
cmake -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DGENERATED_SUBDIR=%GEN_SUBDIR% >nul 2>&1
echo Done. Version kept at %VER% (--no-bump).
echo.
exit /b 0

:package
REM %1 = target root dir. Lays out the package per profile.
REM   ERR              : <root>\dll\offline\{dll,ini} + <root>\addons\MapForGoblins\menu\gfx
REM   vanilla/convergence : <root>\MapForGoblins\{dll,ini} + <root>\MapForGoblins\menu\gfx
REM The ini is GENERATED from the in-code schema via mfg_inigen (non-ERR builds
REM pass --vanilla to omit ERR-only sections), not copied.
set "PKG_ROOT=%~1"
set "INIGEN=%BUILD_DIR%\Release\mfg_inigen.exe"
if not exist "%INIGEN%" set "INIGEN=%BUILD_DIR%\Debug\mfg_inigen.exe"
if not exist "%INIGEN%" (
    echo [FAILED] mfg_inigen.exe not found under %BUILD_DIR%
    exit /b 1
)
if exist "%PKG_ROOT%" rmdir /s /q "%PKG_ROOT%"
if defined MFG_PROFILE (
    REM non-ERR gfx = that game's own worldmap base + our icon frames
    REM (tools/build_vanilla_gfx.py), NOT the ERR-based _new.gfx (that one
    REM carries re-iconed markers + references to textures absent elsewhere).
    REM GFX_SRC is set per profile at the top of this script.
    mkdir "%PKG_ROOT%\MapForGoblins\menu" 2>nul
    copy /Y "%BUILD_DIR%\Release\MapForGoblins.dll" "%PKG_ROOT%\MapForGoblins\" >nul
    "%INIGEN%" "%PKG_ROOT%\MapForGoblins\MapForGoblins.ini" --vanilla
    copy /Y "%GFX_SRC%" "%PKG_ROOT%\MapForGoblins\menu\02_120_worldmap.gfx" >nul
    copy /Y "%SCRIPT_DIR%LICENSE.txt" "%PKG_ROOT%\" >nul
) else (
    mkdir "%PKG_ROOT%\dll\offline" 2>nul
    mkdir "%PKG_ROOT%\addons\MapForGoblins\menu" 2>nul
    copy /Y "%BUILD_DIR%\Release\MapForGoblins.dll" "%PKG_ROOT%\dll\offline\" >nul
    "%INIGEN%" "%PKG_ROOT%\dll\offline\MapForGoblins.ini"
    copy /Y "%GFX_SRC%" "%PKG_ROOT%\addons\MapForGoblins\menu\02_120_worldmap.gfx" >nul
    copy /Y "%SCRIPT_DIR%LICENSE.txt" "%PKG_ROOT%\" >nul
)
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
