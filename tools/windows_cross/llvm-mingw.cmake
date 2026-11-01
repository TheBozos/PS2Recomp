# Cross-compiles for x86_64-w64-mingw32 (UCRT, libc++) with llvm-mingw from a Linux host.
#   LLVM_MINGW  llvm-mingw's root (the directory holding bin/x86_64-w64-mingw32-clang)
#   WIN_FFMPEG  an FFmpeg Windows dev build (include/, lib/, lib/pkgconfig/, bin/*.dll), found with pkg-config
# Both are read from the environment, so CMake's try_compile projects see them too; tools/windows_cross/build.sh sets
# them.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(LLVM_MINGW "$ENV{LLVM_MINGW}")
if(NOT LLVM_MINGW OR NOT EXISTS "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang")
    message(FATAL_ERROR "LLVM_MINGW must name llvm-mingw's root (got '${LLVM_MINGW}')")
endif()

set(CMAKE_C_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR "${LLVM_MINGW}/bin/llvm-ar")
set(CMAKE_RANLIB "${LLVM_MINGW}/bin/llvm-ranlib")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW}/x86_64-w64-mingw32" "$ENV{WIN_FFMPEG}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
