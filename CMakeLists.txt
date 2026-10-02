# CMakeLists.txt -- builds every hsh module into a single hsh.exe
#
#   MSVC   : cmake -S . -B build && cmake --build build --config Release
#   MinGW  : cmake -S . -B build -G "MinGW Makefiles" && cmake --build build
#
# All *.c files next to this file (or under ./src) are picked up
# automatically, so the parser / executor / jobs / ui modules written by
# teammates are compiled in without editing this file.

cmake_minimum_required(VERSION 3.15)
project(hsh LANGUAGES C)

if(NOT WIN32)
  message(FATAL_ERROR "hsh uses the Win32 API: build on Windows (MSVC or MinGW-w64).")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Collect sources (CONFIGURE_DEPENDS re-globs when files are added).
file(GLOB HSH_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

# test_*.c have their own main(): keep them out of hsh.exe (built separately below).
list(FILTER HSH_SOURCES EXCLUDE REGEX "test_[^/\\\\]*\\.c$")

add_executable(hsh ${HSH_SOURCES})
target_include_directories(hsh PRIVATE
     "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Target Windows 10+. We call the *W (wide) Win32 functions explicitly, so we
# do NOT define UNICODE; strings in the shell itself stay UTF-8 char*.
target_compile_definitions(hsh PRIVATE
     _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS)

# shell32 -> ShellExecuteExW, CommandLineToArgvW   advapi32 -> token APIs
# ole32   -> CoInitializeEx (executor.c, needed before ShellExecuteEx)
target_link_libraries(hsh PRIVATE shell32 advapi32 ole32)

# Parser unit tests:  cmake --build build --target test_parser
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_parser.c")
  add_executable(test_parser test_parser.c parser.c)
  target_include_directories(test_parser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(test_parser PRIVATE _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
  set_target_properties(test_parser PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

if(MSVC)
  target_compile_options(hsh PRIVATE /W4 /utf-8)
  # Static CRT: hsh.exe runs without installing the VC++ redistributable.
  set_property(TARGET hsh PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(hsh PRIVATE -Wall -Wextra)
  # Static link: no libgcc/winpthread DLLs needed next to hsh.exe.
  target_link_options(hsh PRIVATE -static)
endif()
