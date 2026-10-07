cmake_minimum_required(VERSION 3.20)
project(sm2mario C CXX)

set(SM2MARIO_VERSION "0.6.1")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT WIN32)
  message(FATAL_ERROR "sm2mario.dll targets Windows. Use cmake/mingw-w64.cmake to cross-compile, "
                      "or tests/run_tests.sh for the host unit tests.")
endif()

# MinHook (vendored, BSD-2-Clause)
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde32.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

file(GLOB_RECURSE SM2MARIO_SOURCES CONFIGURE_DEPENDS src/*.cpp)
if(NOT MSVC)
  # Crash-guard stub (MSVC builds use __try/__except instead).
  enable_language(ASM)
  list(APPEND SM2MARIO_SOURCES src/win/guard_x64.S)
endif()
add_library(sm2mario SHARED ${SM2MARIO_SOURCES})
option(SM2MARIO_TESTING "Test build: allows the smoke test's fake ROM (never ship this)" OFF)
if(SM2MARIO_TESTING)
  target_compile_definitions(sm2mario PRIVATE SM2MARIO_TESTING=1)
endif()
set_target_properties(sm2mario PROPERTIES PREFIX "" OUTPUT_NAME "sm2mario")
target_include_directories(sm2mario PRIVATE src third_party/libsm64 third_party/minhook/include)
target_compile_definitions(sm2mario PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE SM2MARIO_VERSION="${SM2MARIO_VERSION}")
target_link_libraries(sm2mario PRIVATE minhook user32 gdi32)

if(MSVC)
  # Static CRT: nothing extra to install next to the game.
  set_property(TARGET sm2mario minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(sm2mario PRIVATE /W4 /permissive- /utf-8 /wd4100 /wd4201)
else()
  # C99 printf formats (%zu, %llu) in vsnprintf with MinGW's msvcrt.
  target_compile_definitions(sm2mario PRIVATE __USE_MINGW_ANSI_STDIO=1)
  target_compile_options(sm2mario PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
                                          -Wno-cast-function-type)
  target_link_options(sm2mario PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols $<$<CONFIG:Release>:-s>)
endif()
