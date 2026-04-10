cmake_minimum_required(VERSION 3.28.1)

set(CMAKE_GENERATOR_PLATFORM x64)

project( RunePieceTracker
  VERSION   "0.9.1"
  LANGUAGES CXX)

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

set(SPDLOG_DISABLE_DEFAULT_LOGGER ON)
set(SPDLOG_USE_STD_FORMAT ON)
FetchContent_Declare(spdlog
  GIT_REPOSITORY        https://github.com/gabime/spdlog.git
  GIT_TAG               v1.13.0)

# ELDEN RING ABI compatibility
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# Static CRT linking (/MT) - required for DLL injection into game process
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

FetchContent_MakeAvailable(minhook pattern16 spdlog)

add_library(pattern16 INTERFACE)
target_include_directories(pattern16 INTERFACE ${pattern16_SOURCE_DIR}/include)

add_library( RunePieceTracker SHARED
  ../src/modutils.hpp
  ../src/modutils.cpp
  src/tracker.hpp
  src/tracker.cpp
  src/dllmain.cpp)

set_target_properties(RunePieceTracker PROPERTIES OUTPUT_NAME "RunePieceTracker")

add_definitions(-DPROJECT_VERSION="${CMAKE_PROJECT_VERSION}")

target_include_directories(RunePieceTracker PRIVATE ../src)

target_link_libraries( RunePieceTracker
  minhook
  pattern16
  spdlog)
