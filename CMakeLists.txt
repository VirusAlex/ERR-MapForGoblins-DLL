cmake_minimum_required(VERSION 3.28.1)

set(CMAKE_GENERATOR_PLATFORM x64)

project( MapForGoblins
  VERSION   "2.0.1"
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

set(SPDLOG_DISABLE_DEFAULT_LOGGER ON)
set(SPDLOG_USE_STD_FORMAT ON)
FetchContent_Declare(spdlog
  GIT_REPOSITORY        https://github.com/gabime/spdlog.git
  GIT_TAG               v1.13.0)

# Dear ImGui (in-game config overlay). Source-only: we compile the core + the
# DX12/Win32 backends into a small static lib below. Pinned tag with the stable
# positional ImGui_ImplDX12_Init signature.
FetchContent_Declare(imgui
  GIT_REPOSITORY        https://github.com/ocornut/imgui.git
  GIT_TAG               v1.90.9
  CONFIGURE_COMMAND     ""
  BUILD_COMMAND         "")

# Set iterator debug level to 0 for ELDEN RING ABI compatibility
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# Static CRT linking (/MT) - required for DLL injection into game process
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

FetchContent_MakeAvailable(minhook pattern16 mini spdlog imgui)

# imgui core + DX12/Win32 backends are compiled directly INTO the MapForGoblins
# target (see target_sources below), not as a separate static lib - a separate
# lib built as a single-target msbuild dependency lands in the wrong config
# (Debug vs the DLL's Release). Compiling them in keeps one config + one ABI.

add_library(pattern16 INTERFACE)
target_include_directories(pattern16 INTERFACE ${pattern16_SOURCE_DIR}/include)

add_library(mini INTERFACE)
target_include_directories(mini INTERFACE ${mini_SOURCE_DIR}/src)

add_library( MapForGoblins SHARED
  src/from/params.hpp
  src/from/params.cpp
  src/from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp
  src/modutils.hpp
  src/modutils.cpp
  src/goblin/goblin_structs.hpp
  src/goblin/goblin_map_flags.hpp
  src/goblin/goblin_map_tiles.hpp
  src/goblin/goblin_map_exceptions.hpp
  ${GEN_DIR}/goblin_map_data.hpp
  ${GEN_DIR}/goblin_map_data.cpp
  ${GEN_DIR}/goblin_item_icons.hpp
  ${GEN_DIR}/goblin_item_icons.cpp
  ${GEN_DIR}/goblin_enemy_names.hpp
  ${GEN_DIR}/goblin_enemy_names.cpp
  ${GEN_DIR}/goblin_location_alt.hpp
  ${GEN_DIR}/goblin_location_alt.cpp
  ${GEN_DIR}/goblin_geof_models.hpp
  ${GEN_DIR}/goblin_geof_models.cpp
  src/goblin_config_schema.hpp
  src/goblin_config_schema.cpp
  src/goblin_config.hpp
  src/goblin_config.cpp
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
  src/goblin_messages.hpp
  src/goblin_messages.cpp
  src/goblin_overlay.hpp
  src/goblin_overlay.cpp
  src/goblin_map_timing.hpp
  src/goblin_map_timing.cpp
  src/generated_shared/goblin_overlay_icons.hpp
  src/generated_shared/goblin_overlay_icons.cpp
  src/dllmain.cpp)

set_target_properties(MapForGoblins PROPERTIES OUTPUT_NAME "MapForGoblins")

# Dear ImGui (core + DX12/Win32 backends) compiled into the DLL for the in-game
# config overlay. Same target => same /MT + _ITERATOR_DEBUG_LEVEL=0 + config.
target_sources(MapForGoblins PRIVATE
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(MapForGoblins PRIVATE
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
  ${CMAKE_SOURCE_DIR}/src/generated_shared)

# Disable the Win32 backend's built-in XInput gamepad polling. We feed gamepad
# ourselves in feed_input() from the HOOKED XInput (so the game is gated and we
# control deadzone/repeat). Leaving the backend's polling on double-feeds nav:
# our feed says button-DOWN (pre-NewFrame) and the backend reads our zeroed
# XInput and feeds button-UP (in NewFrame) every frame -> a fresh press each
# frame -> continuous/instant nav movement.
target_compile_definitions(MapForGoblins PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# Reproducible build: /Brepro makes the compiler emit deterministic object files
# and the linker zero the PE TimeDateStamp, so the same source always produces a
# byte-identical DLL. Without it MSVC stamps wall-clock build time into the PE
# header, which changes the bytes every build and lets a borderline AV ML model
# (Wacatac.B!ml) flip its verdict between otherwise-identical builds. Stable bytes
# => stable verdict (and a clean variant stays clean).
target_compile_options(MapForGoblins PRIVATE /Brepro)
target_link_options(MapForGoblins PRIVATE /Brepro)

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
add_executable(mfg_inigen src/inigen_main.cpp src/goblin_config_schema.cpp)
target_include_directories(mfg_inigen PRIVATE ${CMAKE_SOURCE_DIR}/src)
# Keep the generated ini's defaults in sync with the DLL's compiled defaults.
if(GENERATED_SUBDIR STREQUAL "generated_vanilla")
  target_compile_definitions(mfg_inigen PRIVATE MFG_PROFILE_VANILLA=1)
endif()

# Build-once-promote: snapshots and releases bake the SAME version string, so a
# tested snapshot DLL is byte-identical to the release (with /Brepro + same data).
# The snapshot-vs-release distinction lives in the output folder, not the binary.
set(FULL_VERSION "${CMAKE_PROJECT_VERSION}")
configure_file(${CMAKE_SOURCE_DIR}/src/version.h.in
               ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)
target_include_directories(MapForGoblins PRIVATE ${CMAKE_BINARY_DIR}/generated)
# Win32 VERSIONINFO resource (CompanyName/FileDescription/versions) so the DLL
# carries proper metadata - better for AV reputation than a blank unsigned DLL.
configure_file(${CMAKE_SOURCE_DIR}/src/version.rc.in
               ${CMAKE_BINARY_DIR}/generated/version.rc @ONLY)
target_sources(MapForGoblins PRIVATE ${CMAKE_BINARY_DIR}/generated/version.rc)
# Resolve bare `#include "goblin_map_data.hpp"` etc. to the active bake dir.
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
  d3d12
  dxgi)
