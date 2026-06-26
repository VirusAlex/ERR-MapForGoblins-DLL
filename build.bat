@echo off
setlocal enabledelayedexpansion

REM ============================================
REM Build Script for MapForGoblins DLL Mod
REM ============================================

set "SCRIPT_DIR=%~dp0"
REM All build trees live under builds\, all release/snapshot packages under releases\.
set "BUILD_DIR=%SCRIPT_DIR%builds\build"

REM Force Python UTF-8 mode for the whole pipeline so open()/json default to UTF-8.
REM Without it, non-ASCII game data (e.g. the Golden Age Chinese overhaul) trips a
REM cp1252 'charmap' decode error. No-op for ASCII data and for binary reads.
set "PYTHONUTF8=1"

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

REM Build profile: default = ERR; pass "--vanilla", "--convergence2", "--convergence3",
REM "--erte", "--goldenage", "--vins" or "--reborn" (any position).
REM   ERR:          data/, src/generated, build/, pre-release/
REM   vanilla:      data/vanilla, src/generated_vanilla, build-vanilla/, pre-release-vanilla/
REM   convergence2: data/convergence2, src/generated_convergence2, build-convergence2/, ... (Convergence 2.x, ME2)
REM   convergence3: data/convergence3, src/generated_convergence3, build-convergence3/, ... (Convergence 3.x, ME3)
REM MFG_PROFILE is exported so build_pipeline.py + config.py pick the data source.
set "GEN_SUBDIR=generated"
set "PKG_PREFIX=ERR"
set "SNAP_DIR=%SCRIPT_DIR%releases\pre-release-err"
set "DISP_PROFILE=err"
set "README_SRC=%SCRIPT_DIR%assets\README.txt"
echo %*| findstr /i /c:"--vanilla" >nul && set "MFG_PROFILE=vanilla"
echo %*| findstr /i /c:"--convergence2" >nul && set "MFG_PROFILE=convergence2"
echo %*| findstr /i /c:"--convergence3" >nul && set "MFG_PROFILE=convergence3"
echo %*| findstr /i /c:"--erte" >nul && set "MFG_PROFILE=erte"
echo %*| findstr /i /c:"--goldenage" >nul && set "MFG_PROFILE=goldenage"
echo %*| findstr /i /c:"--vins" >nul && set "MFG_PROFILE=vins"
echo %*| findstr /i /c:"--reborn" >nul && set "MFG_PROFILE=reborn"
REM Dashboard progress tracking: profile (err if blank) + mode, used by the :dash calls below.
set "DASH_PROF=%MFG_PROFILE%"
if "%DASH_PROF%"=="" set "DASH_PROF=err"
set "DASH_MODE=%~1"
if "%DASH_MODE%"=="" set "DASH_MODE=build"
if "%MFG_PROFILE%"=="vanilla" set "BUILD_DIR=%SCRIPT_DIR%builds\build-vanilla"
if "%MFG_PROFILE%"=="vanilla" set "GEN_SUBDIR=generated_vanilla"
if "%MFG_PROFILE%"=="vanilla" set "PKG_PREFIX=Vanilla"
if "%MFG_PROFILE%"=="vanilla" set "SNAP_DIR=%SCRIPT_DIR%releases\pre-release-vanilla"
if "%MFG_PROFILE%"=="vanilla" set "DISP_PROFILE=vanilla"
if "%MFG_PROFILE%"=="vanilla" set "README_SRC=%SCRIPT_DIR%assets\README_vanilla.txt"
if "%MFG_PROFILE%"=="convergence2" set "BUILD_DIR=%SCRIPT_DIR%builds\build-convergence2"
if "%MFG_PROFILE%"=="convergence2" set "GEN_SUBDIR=generated_convergence2"
if "%MFG_PROFILE%"=="convergence2" set "PKG_PREFIX=Convergence 2.x"
if "%MFG_PROFILE%"=="convergence2" set "SNAP_DIR=%SCRIPT_DIR%releases\pre-release-convergence2"
if "%MFG_PROFILE%"=="convergence2" set "DISP_PROFILE=convergence2"
if "%MFG_PROFILE%"=="convergence2" set "README_SRC=%SCRIPT_DIR%assets\README_convergence2.txt"
if "%MFG_PROFILE%"=="convergence3" set "BUILD_DIR=%SCRIPT_DIR%builds\build-convergence3"
if "%MFG_PROFILE%"=="convergence3" set "GEN_SUBDIR=generated_convergence3"
if "%MFG_PROFILE%"=="convergence3" set "PKG_PREFIX=Convergence 3.x"
if "%MFG_PROFILE%"=="convergence3" set "SNAP_DIR=%SCRIPT_DIR%releases\pre-release-convergence3"
if "%MFG_PROFILE%"=="convergence3" set "DISP_PROFILE=convergence3"
if "%MFG_PROFILE%"=="convergence3" set "README_SRC=%SCRIPT_DIR%assets\README_convergence3.txt"
if "%MFG_PROFILE%"=="erte" set "BUILD_DIR=%SCRIPT_DIR%builds\build-erte"
if "%MFG_PROFILE%"=="erte" set "GEN_SUBDIR=generated_erte"
if "%MFG_PROFILE%"=="erte" set "PKG_PREFIX=ERTE"
if "%MFG_PROFILE%"=="erte" set "SNAP_DIR=%SCRIPT_DIR%releases\pre-release-erte"
if "%MFG_PROFILE%"=="erte" set "DISP_PROFILE=erte"
if "%MFG_PROFILE%"=="erte" set "README_SRC=%SCRIPT_DIR%assets\README_erte.txt"
if "%MFG_PROFILE%"=="goldenage" set "BUILD_DIR=%SCRIPT_DIR%builds\build-goldenage"
if "%MFG_PROFILE%"=="goldenage" set "GEN_SUBDIR=generated_goldenage"
if "%MFG_PROFILE%"=="goldenage" set "PKG_PREFIX=GoldenAge"
if "%MFG_PROFILE%"=="goldenage" set "SNAP_DIR=%SCRIPT_DIR%releases\pre-release-goldenage"
if "%MFG_PROFILE%"=="goldenage" set "DISP_PROFILE=goldenage"
if "%MFG_PROFILE%"=="goldenage" set "README_SRC=%SCRIPT_DIR%assets\README_goldenage.txt"
if "%MFG_PROFILE%"=="vins" set "BUILD_DIR=%SCRIPT_DIR%builds\build-vins"
if "%MFG_PROFILE%"=="vins" set "GEN_SUBDIR=generated_vins"
if "%MFG_PROFILE%"=="vins" set "PKG_PREFIX=Vins"
if "%MFG_PROFILE%"=="vins" set "SNAP_DIR=%SCRIPT_DIR%releases\pre-release-vins"
if "%MFG_PROFILE%"=="vins" set "DISP_PROFILE=vins"
if "%MFG_PROFILE%"=="vins" set "README_SRC=%SCRIPT_DIR%assets\README_vins.txt"
if "%MFG_PROFILE%"=="reborn" set "BUILD_DIR=%SCRIPT_DIR%builds\build-reborn"
if "%MFG_PROFILE%"=="reborn" set "GEN_SUBDIR=generated_reborn"
if "%MFG_PROFILE%"=="reborn" set "PKG_PREFIX=Reborn"
if "%MFG_PROFILE%"=="reborn" set "SNAP_DIR=%SCRIPT_DIR%releases\pre-release-reborn"
if "%MFG_PROFILE%"=="reborn" set "DISP_PROFILE=reborn"
if "%MFG_PROFILE%"=="reborn" set "README_SRC=%SCRIPT_DIR%assets\README_reborn.txt"
echo [PROFILE] %DISP_PROFILE%  build=%BUILD_DIR%  gen=%GEN_SUBDIR%
REM --skip-shared: caller (tools\build_all.py orchestrator) already ran gen_shared
REM once; skip it here so parallel per-profile builds don't race on src\generated_shared.
echo %*| findstr /i /c:"--skip-shared" >nul && set "SKIP_SHARED=1"

if /i "%~1"=="clean" goto :clean
if /i "%~1"=="configure" goto :configure
if /i "%~1"=="generate" goto :generate
if /i "%~1"=="snapshot" goto :snapshot
if /i "%~1"=="release" goto :release

REM Default: generate shared art, configure if needed, then build.
REM gen_shared MUST run BEFORE configure: it (re)creates the src/generated_shared/*
REM sources CMake lists (logo, map/overlay icons, baked i18n strings), so a fresh
REM tree configures cleanly instead of failing on missing generated files.
if not defined SKIP_SHARED (
    call :gen_shared
    if errorlevel 1 exit /b 1
)

call :ensure_configured
if errorlevel 1 exit /b 1

REM Incremental data pipeline (hash-cached): ~1s when nothing data-related changed, rebuilds only the
REM affected stages otherwise. Without this, a plain build refreshed the icon tags (gen_shared, above)
REM but NOT the pipeline-baked markers/z-order - so reordering map_categories.py left markers stale and
REM icons appeared mixed between categories. Pass --force-all to rebuild everything.
echo.
echo Running data pipeline (incremental, hash-cached)...
py "%SCRIPT_DIR%tools\build_pipeline.py" %*
if errorlevel 1 (
    echo [FAILED] build_pipeline.py
    exit /b 1
)

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

:gen_shared
REM Shared, profile-INDEPENDENT generated sources -> src/generated_shared. Derives from
REM tools/map_categories.py + i18n/*.json (NOT per-profile baked data), so every build emits
REM the identical complete superset. No gfx involved. Runs BEFORE configure (these files are
REM CMake sources). (Was a manual step.)
echo.
echo Generating shared icon art (map_categories) + i18n strings...
py "%SCRIPT_DIR%tools\generate_logo.py"
if errorlevel 1 exit /b 1
py "%SCRIPT_DIR%tools\generate_map_icons.py"
if errorlevel 1 exit /b 1
py "%SCRIPT_DIR%tools\generate_overlay_icons.py"
if errorlevel 1 exit /b 1
REM Bake + validate the localization bundles (fails the build if a key is missing in any locale).
py "%SCRIPT_DIR%tools\generate_i18n.py"
if errorlevel 1 exit /b 1
exit /b 0

:ensure_configured
if not exist "%BUILD_DIR%\MapForGoblins.sln" (
    echo Build not configured. Running configure...
    call :configure
    if errorlevel 1 exit /b 1
    exit /b 0
)
REM Self-heal a RELOCATED build tree: if the cache's recorded binary dir no longer
REM matches BUILD_DIR (e.g. after moving build* under builds\), CMake would abort
REM with a "cache directory is different" error. Detect the mismatch and reconfigure.
set "FS_BUILD=%BUILD_DIR:\=/%"
findstr /C:"CMAKE_CACHEFILE_DIR:INTERNAL=%FS_BUILD%" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1
if errorlevel 1 (
    echo Build cache is stale or relocated - reconfiguring...
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

call :dash pipeline
REM Incremental data pipeline (hash-based cache). Pass --force-all to rebuild everything.
echo Running incremental pipeline...
py "%SCRIPT_DIR%tools\build_pipeline.py" %*
if errorlevel 1 (
    echo [FAILED] build_pipeline.py
    exit /b 1
)

REM gen_shared before configure: it creates the generated_shared/* CMake sources.
if not defined SKIP_SHARED (
    call :gen_shared
    if errorlevel 1 exit /b 1
)

call :ensure_configured
if errorlevel 1 exit /b 1

echo.
echo Building MapForGoblins...
echo ----------------------------------------

REM /t:MapForGoblins:Rebuild forces clean rebuild of just the DLL target,
REM avoiding LTCG "copied from previous compilation" cache quirks that can
REM embed stale code (e.g. the PROJECT_VERSION macro not propagating).
call :dash compiling
cmd /c "call "%VS_PATH%" -arch=amd64 >nul 2>&1 && cd /d "%BUILD_DIR%" && msbuild MapForGoblins.sln /p:Configuration=Release /p:Platform=x64 /t:MapForGoblins:Rebuild /v:minimal /m"
if errorlevel 1 (
    echo [FAILED] Snapshot build
    call :dash failed
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

call :dash pipeline
REM Incremental data pipeline (hash-based cache). Pass --force-all to rebuild everything.
echo Running incremental pipeline...
py "%SCRIPT_DIR%tools\build_pipeline.py" %*
if errorlevel 1 (
    echo [FAILED] build_pipeline.py
    exit /b 1
)

call :gen_shared
if errorlevel 1 exit /b 1

REM Self-heal a relocated build tree before the explicit reconfigure below.
call :ensure_configured
if errorlevel 1 exit /b 1

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
call :dash compiling
msbuild MapForGoblins.sln /p:Configuration=Release /p:Platform=x64 /t:MapForGoblins:Rebuild /v:minimal /m
if errorlevel 1 (
    echo [FAILED] Release build
    call :dash failed
    exit /b 1
)
cd /d "%SCRIPT_DIR%"

REM Package into release folder (PKG_PREFIX = ERR or Vanilla per profile)
set "REL_DIR=%SCRIPT_DIR%releases\%PKG_PREFIX% - MapForGoblins - DLL - v%VER%"
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
REM %1 = target root dir. PURE-DLL, FLAT layout (no nesting, same for every profile): the package is just
REM the DLL + generated ini + LICENSE at the root (README is added by the snapshot/release step). The mod
REM is a single DLL with no extra files, so there is nothing to nest.
REM   <root>\MapForGoblins.dll
REM   <root>\MapForGoblins.ini   (GENERATED from the in-code schema via mfg_inigen; non-ERR builds pass
REM                               --vanilla to omit ERR-only sections. show_* default ON in the schema.)
REM   <root>\LICENSE.txt
set "PKG_ROOT=%~1"
set "INIGEN=%BUILD_DIR%\Release\mfg_inigen.exe"
if not exist "%INIGEN%" set "INIGEN=%BUILD_DIR%\Debug\mfg_inigen.exe"
if not exist "%INIGEN%" (
    echo [FAILED] mfg_inigen.exe not found under %BUILD_DIR%
    exit /b 1
)
if exist "%PKG_ROOT%" rmdir /s /q "%PKG_ROOT%"
mkdir "%PKG_ROOT%" 2>nul
copy /Y "%BUILD_DIR%\Release\MapForGoblins.dll" "%PKG_ROOT%\" >nul
copy /Y "%SCRIPT_DIR%LICENSE.txt" "%PKG_ROOT%\" >nul
if defined MFG_PROFILE (
    "%INIGEN%" "%PKG_ROOT%\MapForGoblins.ini" --vanilla
) else (
    "%INIGEN%" "%PKG_ROOT%\MapForGoblins.ini"
)
REM Mark this build DONE in the dashboard (records the final hash/size; best-effort).
call :dash ok
exit /b 0

:dash
REM %1 = build phase (pipeline|compiling|ok|failed). Live dashboard progress; never fails the build.
py "%SCRIPT_DIR%tools\dashboard\record_build.py" --profile %DASH_PROF% --phase %~1 --mode %DASH_MODE% --version %VER% >nul 2>&1
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
