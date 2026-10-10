cmake_minimum_required(VERSION 3.21)
project(ClaudeUsageWin LANGUAGES CXX RC)

# MSVC only: the code is Win32 + WinHTTP + GDI+ with no third-party dependency.
if(NOT MSVC)
  message(FATAL_ERROR "Build with MSVC (Visual Studio 2022 or later, or its Build Tools).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: one self-contained exe, nothing to install beside it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(APP_VERSION "0.0.0-dev" CACHE STRING "Version stamped into the binaries (CI passes the tag)")

add_compile_definitions(
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006
  APP_VERSION="${APP_VERSION}")
add_compile_options(
  $<$<COMPILE_LANGUAGE:CXX>:/W4>
  $<$<COMPILE_LANGUAGE:CXX>:/WX>
  $<$<COMPILE_LANGUAGE:CXX>:/permissive->
  $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
  $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
  $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/GL>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/Gy>)
add_link_options($<$<CONFIG:Release>:/LTCG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

# Everything but the two entry points and the UI: shared by the tray, the CLI and the tests.
add_library(usagecore STATIC
  src/json.cpp
  src/util.cpp
  src/http.cpp
  src/config.cpp
  src/pace.cpp
  src/providers.cpp
  src/reminders.cpp
  src/cli.cpp
  src/ui_model.cpp)
target_include_directories(usagecore PUBLIC src)
target_link_libraries(usagecore PUBLIC winhttp iphlpapi advapi32)

# ClaudeUsage.exe: the tray app (and, given a command, the CLI — the tray runs itself as `tick`).
add_executable(ClaudeUsage WIN32
  src/app.cpp
  src/card.cpp
  src/draw.cpp
  src/app.rc
  src/app.manifest)
target_link_libraries(ClaudeUsage PRIVATE usagecore gdiplus shcore dwmapi shell32 ole32 comctl32)

# claude-usage.exe: the same CLI as a console program, for a terminal and scripts.
add_executable(claude-usage
  src/console_main.cpp
  src/app.rc
  src/app.manifest)
target_link_libraries(claude-usage PRIVATE usagecore)

enable_testing()
add_executable(usage_tests tests/tests.cpp)
target_link_libraries(usage_tests PRIVATE usagecore ws2_32)
add_test(NAME usage_tests
  COMMAND usage_tests $<TARGET_FILE:claude-usage> ${CMAKE_CURRENT_SOURCE_DIR}/src)
