cmake_minimum_required(VERSION 3.28.1)

set(CMAKE_GENERATOR_PLATFORM x64)

project( MapForGoblins
  VERSION   "2.1.7"
  LANGUAGES CXX RC)

# Which baked-data dir to compile. Default = ERR (src/generated). The vanilla
# build passes -DGENERATED_SUBDIR=generated_vanilla so the two bakes never
# clobber each other. Sources include the generated headers by bare name and
# resolve them via the include path below.
if(NOT DEFINED GENERATED_SUBDIR)
  set(GENERATED_SUBDIR "generated")
endif()
set(GEN_DIR "${CMAKE_SOURCE_DIR}/src/${GENERATED_SUBDIR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

# Fetched deps (minhook) still declare cmake_minimum_required < 3.5, which
# CMake 4 rejects outright; opt them into the 3.5 policy floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)

FetchContent_Declare(minhook
  GIT_REPOSITORY        https://github.com/TsudaKageyu/minhook.git
  GIT_TAG               f5485b8454544c2f034c78f8f127c1d03dea3636)

FetchContent_Declare(pattern16
  GIT_REPOSITORY        https://github.com/Dasaav-dsv/Pattern16.git
  GIT_TAG               728eac543cee7e7bf6fda445d7db06e3dc8a61d0
  CONFIGURE_COMMAND     ""
  BUILD_COMMAND         "")

FetchContent_Declare(mini
  GIT_REPOSITORY        https://github.com/metayeti/mINI.git
  GIT_TAG               0.9.15
  CONFIGURE_COMMAND     ""
  BUILD_COMMAND         "")

# spdlog declares both with option() under cmake_minimum_required(3.11), i.e. CMP0077 OLD: a plain
# set() lost to option()'s OFF on a build dir's FIRST configure and won on every later one, so the
# same source built bundled fmt in a fresh dir and std::format in a reused one. CACHE FORCE makes it
# one answer. Bundled fmt: std::format's float path drags in the CRT's to_chars Ryu tables (~110 KB).
set(SPDLOG_DISABLE_DEFAULT_LOGGER ON CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY        https://github.com/gabime/spdlog.git
  GIT_TAG               v1.13.0)

# Dear ImGui (in-game config overlay). Source-only: we compile the core + the
# DX11/Win32 backends into the DLL below (the overlay renders in its own D3D11 +
# DirectComposition window; the DX12 backend is also compiled but unused now).
FetchContent_Declare(imgui
  GIT_REPOSITORY        https://github.com/ocornut/imgui.git
  GIT_TAG               v1.90.9
  CONFIGURE_COMMAND     ""
  BUILD_COMMAND         "")

# miniz: tiny zlib-compatible inflate, used to decode our embedded DefineBitsLossless2 icon tags into
# RGBA for the ImGui overlay (so map + menu share ONE embedded icon source - the lossless tags). We only
# DOWNLOAD it (its own CMake forces CMAKE_BUILD_TYPE and breaks the VS multi-config build) and compile
# its sources straight into MapForGoblins - see below.
FetchContent_Declare(miniz
  GIT_REPOSITORY        https://github.com/richgel999/miniz.git
  GIT_TAG               3.0.2)

# stb_image: single-header PNG decoder for the dev "Icon Preview" tool (load a png off disk into an
# RGBA texture). Download only; we #define STB_IMAGE_IMPLEMENTATION in goblin_overlay.cpp.
FetchContent_Declare(stb
  GIT_REPOSITORY        https://github.com/nothings/stb.git
  # Pinned: upstream moved stb_image_resize2.h out of this repo on 2026-10-19.
  GIT_TAG               31c1ad37456438565541f4919958214b6e762fb4)

# Set iterator debug level to 0 for ELDEN RING ABI compatibility
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# Static CRT linking (/MT) - required for DLL injection into game process
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

FetchContent_MakeAvailable(minhook pattern16 mini spdlog imgui)
FetchContent_GetProperties(miniz)
if(NOT miniz_POPULATED)
  FetchContent_Populate(miniz)  # download only, no add_subdirectory (avoid its CMake)
endif()
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
  FetchContent_Populate(stb)    # header-only, download only
endif()

# imgui core + DX12/Win32 backends are compiled directly INTO the MapForGoblins
# target (see target_sources below), not as a separate static lib - a separate
# lib built as a single-target msbuild dependency lands in the wrong config
# (Debug vs the DLL's Release). Compiling them in keeps one config + one ABI.

add_library(pattern16 INTERFACE)
target_include_directories(pattern16 INTERFACE ${pattern16_SOURCE_DIR}/include)

add_library(mini INTERFACE)
target_include_directories(mini INTERFACE ${mini_SOURCE_DIR}/src)

add_library( MapForGoblins SHARED
  src/goblin_map_api.cpp
  src/goblin_map_api.hpp
  sdk/mfg_map_api.h
  src/from/params.hpp
  src/from/params.cpp
  src/from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp
  src/modutils.hpp
  src/modutils.cpp
  src/goblin/goblin_structs.hpp
  src/goblin/goblin_map_flags.hpp
  src/goblin/goblin_map_tiles.hpp
  src/goblin/goblin_map_exceptions.hpp
  # These four headers are hand-maintained and live in src/; only their .cpp is baked per
  # profile (and includes them as "../<name>.hpp"). generate_data.py stopped mirroring them
  # into the bake dir, so naming the bake copy here made a first build of a profile that has
  # no stale copy fail at configure time with "Cannot find source file".
  src/goblin_map_data.hpp
  src/goblin_map_blob.hpp
  src/goblin_map_blob.cpp
  ${GEN_DIR}/goblin_map_blob_data.cpp
  src/goblin_item_icons.hpp
  ${GEN_DIR}/goblin_item_icons.cpp
  src/goblin_enemy_names.hpp
  ${GEN_DIR}/goblin_enemy_names.cpp
  src/goblin_item_fallback.hpp
  ${GEN_DIR}/goblin_item_fallback.cpp
  src/goblin_names_blob.hpp
  src/goblin_names_blob.cpp
  ${GEN_DIR}/goblin_location_alt.hpp
  ${GEN_DIR}/goblin_location_alt.cpp
  ${GEN_DIR}/goblin_geof_models.hpp
  ${GEN_DIR}/goblin_geof_models.cpp
  ${GEN_DIR}/goblin_region_map.hpp
  ${GEN_DIR}/goblin_region_map.cpp
  src/goblin_config_schema.hpp
  src/goblin_config_schema.cpp
  src/goblin_i18n.hpp
  src/goblin_i18n.cpp
  src/goblin_i18n_bundle.hpp
  src/goblin_i18n_blob.cpp
  src/goblin_config.hpp
  src/goblin_config.cpp
  src/goblin_anchors.hpp
  src/goblin_anchors.cpp
  # src/goblin_diag.{hpp,cpp} - the map-icon status registry. Deliberately NOT compiled: it existed
  # so a player could screenshot a load-step summary into a bug report, and its only reader (the
  # overlay Debug tab readout) went on 2026-07-29 because nobody ever sent one - players send LOGS.
  # Fourteen of its sixteen setters only duplicated a neighbouring spdlog line; the two that did
  # not (the logo plaque outcome, the worldmap sprite pointer that shows the heap region) were
  # moved into the log before this. Same treatment as the add-on host and sfimage: sources stay.
  src/goblin_collected.hpp
  src/goblin_collected.cpp
  src/goblin_inject.hpp
  src/goblin_inject.cpp
  src/goblin_kindling.hpp
  src/goblin_kindling.cpp
  src/goblin_logic.hpp
  src/goblin_logic.cpp
  src/goblin_markers.hpp
  src/goblin_markers.cpp
  src/goblin_progress.hpp
  src/goblin_progress.cpp
  src/goblin_search.cpp
  src/goblin_status_line.cpp
  src/goblin_native_menu.hpp
  src/goblin_native_menu.cpp
    src/goblin_own_movie.cpp
  # src/goblin_menu_addons.{hpp,cpp} - the add-on SDK HOST. Deliberately NOT compiled: the SDK
  # ships as a separate mod, so the module walk that looks for MfgMenuAddonInit, the page list and
  # the row routing must not be in this binary. The sources stay in the tree and are still the
  # reference for that mod. To build the host back in, restore these two lines AND set
  # MFG_MENU_ADDON_HOST=1 (goblin_build_variants.hpp, or -DMFG_MENU_ADDON_HOST=1).
  # src/goblin_sfimage.{hpp,cpp} - the Scaleform image-resource builder. Deliberately NOT compiled:
  # its last external caller was icon_resource_for() in goblin_stall_probe.cpp, removed 2026-07-30
  # with the own-draw icon routes (menu icons are spliced into the movie instead). After that,
  # create_resource / make_raw_image / wrap_resource / heap / value_of / available called only each
  # other - a closed island. Same treatment as the add-on host above: the sources stay in the tree.
  # To build it back in, restore these two lines and re-add the two #includes noted in its header.
  src/goblin_messages.hpp
  src/goblin_messages.cpp
  src/goblin_overlay.hpp
  src/goblin_overlay.cpp
  src/goblin_map_timing.hpp
  src/goblin_map_timing.cpp
  src/goblin_stall_probe.hpp
  src/goblin_stall_probe.cpp
  src/goblin_gfx_probe.hpp
  src/goblin_gfx_probe.cpp
  src/goblin_maphover.hpp
  src/goblin_maphover.cpp
  src/goblin_mapproject.hpp
  src/goblin_mapproject.cpp
  src/goblin_worldmap_probe.hpp
  src/goblin_worldmap_probe.cpp
  src/goblin_crashdiag.hpp
  src/goblin_crashdiag.cpp
  src/generated_shared/goblin_overlay_icons.hpp
  src/generated_shared/goblin_overlay_icons.cpp
  src/generated_shared/goblin_map_icons.hpp
  src/generated_shared/goblin_map_icons.cpp
  src/generated_shared/goblin_logo.hpp
  src/generated_shared/goblin_logo.cpp
  src/generated_shared/goblin_i18n_strings.cpp
  ${miniz_SOURCE_DIR}/miniz.c
  ${miniz_SOURCE_DIR}/miniz_tinfl.c
  ${miniz_SOURCE_DIR}/miniz_tdef.c
  src/dllmain.cpp)

# miniz: inflate only (no zip/file/time); include its headers + our stub miniz_export.h (miniz's CMake
# would generate that, but we don't run it); compiled straight into the DLL.
target_include_directories(MapForGoblins PRIVATE ${miniz_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/vendor_miniz ${stb_SOURCE_DIR})
target_compile_definitions(MapForGoblins PRIVATE MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)

set_target_properties(MapForGoblins PROPERTIES OUTPUT_NAME "MapForGoblins")

# Dear ImGui (core + DX11/DX12/Win32 backends) compiled into the DLL for the
# in-game config overlay. Same target => same /MT + _ITERATOR_DEBUG_LEVEL=0 + config.
target_sources(MapForGoblins PRIVATE
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)

# sc2: in-swapchain D3D12 overlay backend. Our ImGui is rendered into the game's OWN backbuffer
# through a per-instance Present shadow, instead of into a separate top-most window - so there is no
# focus steal (the Linux/Proton alt-tab problem) and no second swapchain. Selected at runtime by
# overlay_render_mode = swapchain_2; every other mode leaves this code dormant.
# Origin: the backend v2 core written for CustomTalismanEffects / QuestPath (imCioco), used here
# with the author's permission; its design notes are BACKEND_V2_ARCHITECTURE.md and
# OVERLAY_V2_PORTING_GUIDE.md in that project.
target_sources(MapForGoblins PRIVATE
  src/sc2/overlay_frame.cpp
  src/sc2/overlay_dxgi_shadow.cpp
  src/sc2/overlay_d3d12.cpp
  src/sc2/overlay_present.cpp
  src/sc2/hooks.cpp
  src/sc2/overlay_coexist.cpp
  src/sc2/log.cpp)
# Makes the backend's PRIVATE window-class names unique per mod. The SHARED arbitration mutex names
# must stay identical across mods that embed this backend - see sc2/overlay_coexist.hpp.
target_compile_definitions(MapForGoblins PRIVATE QP_BACKEND_MOD_ID=MapForGoblins)

target_include_directories(MapForGoblins PRIVATE
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
  ${CMAKE_SOURCE_DIR}/src/sc2
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/generated_shared)

# Disable the Win32 backend's built-in XInput gamepad polling. The overlay polls
# XInput itself in feed_gamepad() and masks the menu-toggle combo out of the nav
# feed; letting the backend also poll would double-feed nav and fight our masking.
target_compile_definitions(MapForGoblins PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# Build variants (src/goblin_build_variants.hpp). Each defaults in the header; passing one here is how
# the documented "switch it back with a rebuild" actually works, e.g.
#   cmake -DMFG_STALL_PROFILER=1 ...
# MFG_OVERLAY_OWN_WINDOW / MFG_OWN_WINDOW_MODE were missing from this list, so the one variant
# with ~1100 lines behind it could not be switched from the command line at all - and, unbuilt,
# it silently stopped compiling (it still read a config field that had been renamed away).
foreach(mfg_variant MFG_LEGACY_PIN_MARKERS MFG_SLOW_MAP_OPEN MFG_NO_SELF_DETACH
                    MFG_ATTACH_ALL_MARKERS MFG_STALL_PROFILER
                    MFG_OVERLAY_OWN_WINDOW MFG_OWN_WINDOW_MODE
                    MFG_MENU_ADDON_HOST MFG_CMDLIST_PROTO)
  if(DEFINED ${mfg_variant})
    target_compile_definitions(MapForGoblins PRIVATE ${mfg_variant}=${${mfg_variant}})
    message(STATUS "MapForGoblins build variant: ${mfg_variant}=${${mfg_variant}}")
  endif()
endforeach()

# Reproducible build: /Brepro makes the compiler emit deterministic object files, so the same
# source always produces a byte-identical DLL. Without it MSVC stamps wall-clock build time into
# the PE header, which changes the bytes every build and lets a borderline AV ML model
# (Wacatac.B!ml) flip its verdict between otherwise-identical builds. Stable bytes
# => stable verdict (and a clean variant stays clean).
#
# CORRECTION 2026-08-01: the linker's /Brepro does NOT zero the PE TimeDateStamp, as this comment
# claimed for months - it writes a CONTENT HASH there. That reads back as a nonsense date (VT saw
# 2064-12-05 on the 2.1.0 sample; a local 2.1.1 build read 1979-11-17) and VirusTotal scores it as
# timestomping: MBC "Hidden Files and Directories -> Timestamp F0005.004" plus MITRE T1070
# Indicator Removal. Our own anti-FP measure was generating an FP signal.
# The obvious fix does NOT work on this toolchain: /TIMESTAMP:0x<unix_hex> was TRIED 2026-08-01 and
# the linker answered "LNK4044: unrecognized option ... ignored" (MSVC linker 14.36, VS2022 17.6).
# Ignored silently means the stamp fell back to WALL-CLOCK build time, which costs determinism -
# strictly worse than a nonsense date. So the linker /Brepro stays until the toolchain is new
# enough to take /TIMESTAMP; do not re-try it without checking the link log for LNK4044.
# ACCEPTANCE TEST if this line is ever touched: two consecutive full builds must produce a
# byte-identical DLL.
target_compile_options(MapForGoblins PRIVATE /Brepro /utf-8)
target_link_options(MapForGoblins PRIVATE /Brepro)
# /MAP for CRASH SYMBOLIZATION. The shipped DLL carries no PDB and no debug directory (that is
# deliberate - see the Defender false-positive notes), so a crash breadcrumb gives us
# "MapForGoblins.dll+0xRVA" and nothing else. The link map turns those RVAs back into function
# names. It is a separate .map file next to the DLL: it does not change codegen, does not add a
# debug directory to the binary, and /Brepro keeps the build byte-identical.
target_link_options(MapForGoblins PRIVATE /MAP)

# Non-ERR bakes (vanilla, convergence): omit + disable ERR-only config
# entries at runtime. MFG_VANILLA here means "no ERR-only features".
if(NOT GENERATED_SUBDIR STREQUAL "generated")
  target_compile_definitions(MapForGoblins PRIVATE MFG_VANILLA=1)
endif()

# Vanilla-ONLY (not Convergence): used to default the live-loot / randomizer-
# compat options ON, since the plain vanilla build is what randomizer players
# use. ERR and Convergence keep them OFF (opt-in).
if(GENERATED_SUBDIR STREQUAL "generated_vanilla")
  target_compile_definitions(MapForGoblins PRIVATE MFG_PROFILE_VANILLA=1)
endif()

# Tiny build-time tool that emits the default ini from the in-code schema
# (src/goblin_config_schema.cpp). build.bat runs it to generate the shipped
# ini per profile instead of copying a hand-maintained file.
add_executable(mfg_inigen src/inigen_main.cpp src/goblin_config_schema.cpp src/goblin_i18n.cpp
  src/goblin_i18n_blob.cpp src/generated_shared/goblin_i18n_strings.cpp
  # the i18n strings ship deflated (goblin_i18n_blob.cpp), so the tool inflates them like the DLL
  ${miniz_SOURCE_DIR}/miniz.c
  ${miniz_SOURCE_DIR}/miniz_tinfl.c
  ${miniz_SOURCE_DIR}/miniz_tdef.c)
target_include_directories(mfg_inigen PRIVATE ${CMAKE_SOURCE_DIR}/src ${miniz_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/vendor_miniz)
# MFG_INIGEN: goblin_i18n_blob.cpp reports to stderr instead of the DLL's log, which this tool lacks.
target_compile_definitions(mfg_inigen PRIVATE MFG_INIGEN=1 MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
target_compile_options(mfg_inigen PRIVATE /utf-8)
# Keep the generated ini's defaults in sync with the DLL's compiled defaults.
if(GENERATED_SUBDIR STREQUAL "generated_vanilla")
  target_compile_definitions(mfg_inigen PRIVATE MFG_PROFILE_VANILLA=1)
endif()

# Build-once-promote: snapshots and releases bake the SAME version string, so a
# tested snapshot DLL is byte-identical to the release (with /Brepro + same data).
# The snapshot-vs-release distinction lives in the output folder, not the binary.
set(FULL_VERSION "${CMAKE_PROJECT_VERSION}")
# Build identity for the runtime startup log (helps triage user bug reports): the profile
# name (derived from the bake dir) + the short git commit the DLL was built from, with a "d"
# PREFIX when the working tree has uncommitted tracked changes (d947216c). Same commit => same
# string => the build-once-promote byte-identity (above) still holds. git on PATH is not
# guaranteed under the VS build env, so it falls back to "nogit".
if(GENERATED_SUBDIR STREQUAL "generated")
  set(BUILD_NAME "err")
else()
  string(REPLACE "generated_" "" BUILD_NAME "${GENERATED_SUBDIR}")
endif()
# version.h is generated on EVERY BUILD, not at configure time. Configure-time capture meant the
# hash went stale the moment a commit landed without CMake re-running: a DLL built from 97c3534
# reported 947216c-dirty on 2026-07-30. cmake/mfg_version.cmake only rewrites the header when its
# content actually changes, so incremental builds do not rebuild the four TUs that include it.
add_custom_target(mfg_version ALL
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/version.h
  COMMAND ${CMAKE_COMMAND}
          -DSRC_DIR=${CMAKE_SOURCE_DIR}
          -DIN_FILE=${CMAKE_SOURCE_DIR}/src/version.h.in
          -DOUT_FILE=${CMAKE_BINARY_DIR}/generated/version.h
          -DFULL_VERSION=${FULL_VERSION}
          -DBUILD_NAME=${BUILD_NAME}
          -P ${CMAKE_SOURCE_DIR}/cmake/mfg_version.cmake
  COMMENT "Stamping build identity (version + profile + git hash)"
  VERBATIM)
add_dependencies(MapForGoblins mfg_version)
# Generate it once now as well, so the very first configure leaves a usable header on disk.
execute_process(COMMAND ${CMAKE_COMMAND}
                -DSRC_DIR=${CMAKE_SOURCE_DIR}
                -DIN_FILE=${CMAKE_SOURCE_DIR}/src/version.h.in
                -DOUT_FILE=${CMAKE_BINARY_DIR}/generated/version.h
                -DFULL_VERSION=${FULL_VERSION}
                -DBUILD_NAME=${BUILD_NAME}
                -P ${CMAKE_SOURCE_DIR}/cmake/mfg_version.cmake)
target_include_directories(MapForGoblins PRIVATE ${CMAKE_BINARY_DIR}/generated)
# Win32 VERSIONINFO resource (CompanyName/FileDescription/versions) so the DLL
# carries proper metadata - better for AV reputation than a blank unsigned DLL.
configure_file(${CMAKE_SOURCE_DIR}/src/version.rc.in
               ${CMAKE_BINARY_DIR}/generated/version.rc @ONLY)
target_sources(MapForGoblins PRIVATE ${CMAKE_BINARY_DIR}/generated/version.rc)
# Resolve a bare `#include "goblin_region_map.hpp"` (and the other genuinely generated
# headers) to the active bake dir.
target_include_directories(MapForGoblins PRIVATE ${GEN_DIR})

# MapForGoblins.ini is no longer copied: build.bat generates it per profile in
# :package via mfg_inigen, and the DLL self-creates/repairs it at runtime
# (goblin::ensure_ini), so dev runs need no shipped ini. Keep the dependency so
# a plain `build.bat` also builds the generator.
add_dependencies(MapForGoblins mfg_inigen)

target_link_libraries( MapForGoblins
  minhook
  pattern16
  mini
  spdlog
  d3d11
  d3d12
  dxgi
  dcomp
  comdlg32
  # sc2 backend: compiles its own root signature / shaders (d3dcompiler) and uses COM apartment
  # helpers for the DXGI work (ole32).
  d3dcompiler
  ole32)
