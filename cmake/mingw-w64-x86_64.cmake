# Cross-compile the Windows (x86_64) executable from macOS or Linux with MinGW-w64 GCC
# (Homebrew `mingw-w64`, Debian/Ubuntu `gcc-mingw-w64-x86-64`).
#
#   cmake -S . -B build/win -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#     -DCMAKE_PREFIX_PATH=/path/to/win-deps       # SDL3 + zlib built for x86_64-w64-mingw32
#
# SDL3 and zlib must be MinGW builds (SDL's "SDL3-devel-*-mingw" release, or built from
# source with this same toolchain file). See AGENTS.md "Windows and Linux".
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLE x86_64-w64-mingw32 CACHE STRING "MinGW-w64 target triple")
find_program(CMAKE_C_COMPILER ${MINGW_TRIPLE}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER ${MINGW_TRIPLE}-g++)
find_program(CMAKE_RC_COMPILER ${MINGW_TRIPLE}-windres REQUIRED)

# Look for libraries/headers only in the target environment (CMAKE_PREFIX_PATH and the
# toolchain's own sysroot), programs only on the host.
get_filename_component(_mingw_bin ${CMAKE_C_COMPILER} DIRECTORY)
get_filename_component(_mingw_root ${_mingw_bin} DIRECTORY)
set(CMAKE_FIND_ROOT_PATH ${_mingw_root}/${MINGW_TRIPLE})
if(EXISTS ${_mingw_root}/opt/mingw-w64/toolchain-x86_64/${MINGW_TRIPLE})
  list(APPEND CMAKE_FIND_ROOT_PATH ${_mingw_root}/opt/mingw-w64/toolchain-x86_64/${MINGW_TRIPLE})
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# The dependency prefixes are target trees too (a path under a root is not re-rooted).
list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})
