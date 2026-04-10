cmake_minimum_required(VERSION 3.28.1)
set(CMAKE_GENERATOR_PLATFORM x64)

project(FmgPatcher VERSION "1.0.0" LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(FmgPatcher fmg_patcher.cpp)
set_target_properties(FmgPatcher PROPERTIES OUTPUT_NAME "fmg_patcher")
