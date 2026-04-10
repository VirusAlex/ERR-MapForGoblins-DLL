cmake_minimum_required(VERSION 3.28.1)

set(CMAKE_GENERATOR_PLATFORM x64)

project( MapForGoblins
  VERSION   "1.0.8"
  LANGUAGES CXX)

set(VERSION_PRE "pre")  # set to "" for release builds

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

# Set iterator debug level to 0 for ELDEN RING ABI compatibility
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# Static CRT linking (/MT) - required for DLL injection into game process
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

FetchContent_MakeAvailable(minhook pattern16 mini spdlog)

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
  src/generated/goblin_map_data.hpp
  src/generated/goblin_map_data.cpp
  src/generated/goblin_text_data.hpp
  src/generated/goblin_text_data.cpp
  src/goblin_config.hpp
  src/goblin_config.cpp
  src/goblin_collected.hpp
  src/goblin_collected.cpp
  src/goblin_inject.hpp
  src/goblin_inject.cpp
  src/goblin_logic.hpp
  src/goblin_logic.cpp
  src/goblin_messages.hpp
  src/goblin_messages.cpp
  src/dllmain.cpp)

set_target_properties(MapForGoblins PROPERTIES OUTPUT_NAME "MapForGoblins")

if(VERSION_PRE)
  set(FULL_VERSION "${VERSION_PRE}-${CMAKE_PROJECT_VERSION}")
else()
  set(FULL_VERSION "${CMAKE_PROJECT_VERSION}")
endif()
add_definitions(-DPROJECT_VERSION="${FULL_VERSION}")

add_custom_command(TARGET MapForGoblins POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:MapForGoblins>
  ${CMAKE_SOURCE_DIR}/MapForGoblins.ini
  COMMAND_EXPAND_LISTS)

target_link_libraries( MapForGoblins
  minhook
  pattern16
  mini
  spdlog)
