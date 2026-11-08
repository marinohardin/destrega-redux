cmake_minimum_required(VERSION 3.20)
project(destrega_recomp C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(ZLIB REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GEN_DIR ${CMAKE_SOURCE_DIR}/gen)

# Regenerate C from the disc image whenever the recompiler or analysis changes.
file(GLOB RECOMP_PY ${CMAKE_SOURCE_DIR}/recomp/*.py)
file(GLOB ANALYSIS_JSON ${CMAKE_SOURCE_DIR}/analysis/*.json ${CMAKE_SOURCE_DIR}/analysis/overlays/*.json)
if(NOT EXISTS ${GEN_DIR}/files.cmake)
  execute_process(COMMAND ${Python3_EXECUTABLE} recomp/recomp.py --out gen
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
include(${GEN_DIR}/files.cmake)
add_custom_command(
  OUTPUT ${GEN_SOURCES} ${GEN_DIR}/gen_protos.h
  COMMAND ${Python3_EXECUTABLE} recomp/recomp.py --out gen
  DEPENDS ${RECOMP_PY} ${ANALYSIS_JSON} ${CMAKE_SOURCE_DIR}/analysis/extra_seeds.txt
          ${CMAKE_SOURCE_DIR}/analysis/overrides.txt
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Recompiling MIPS code to C")

add_library(gen STATIC ${GEN_SOURCES})
target_include_directories(gen PRIVATE runtime gen)
# Generated code is huge and goto-heavy; -O1 keeps build times sane.
target_compile_options(gen PRIVATE -O1 -w -fno-strict-aliasing)

add_executable(destrega
  runtime/main.c runtime/rt.c runtime/bus.c runtime/bios.c runtime/interp.c
  runtime/cdrom.c runtime/disc.c runtime/sio.c runtime/gpu.c runtime/hires.c runtime/gte.c runtime/spu.c runtime/xa.c runtime/mdec.c
  runtime/overrides/override.c runtime/overrides/collision.c
  runtime/overrides/widescreen.c)
target_include_directories(destrega PRIVATE runtime gen)
target_compile_options(destrega PRIVATE -Wall -Wno-unused-function -fno-strict-aliasing)
target_link_libraries(destrega PRIVATE gen SDL3::SDL3 ZLIB::ZLIB m)

# macOS: also wrap the binary in a self-contained Destrega.app (bundled SDL3,
# app icon, ad-hoc signature). The plain `destrega` binary is left in place for
# the CLI/headless flags and tools/run_tests.sh.
if(APPLE)
  option(DESTREGA_MACOS_APP "Build Destrega.app next to the destrega binary" ON)
  set(DESTREGA_APP_VERSION "1.0" CACHE STRING "CFBundle version of Destrega.app")
  set(DESTREGA_APP_ICON ${CMAKE_SOURCE_DIR}/assets/icon/destrega.png CACHE FILEPATH
      "App icon (PNG >= 512px or .icns)")
  set(DESTREGA_APP_SIGN "-" CACHE STRING "codesign identity for Destrega.app (- = ad-hoc)")
  if(DESTREGA_MACOS_APP)
    set(APP_STAMP ${CMAKE_BINARY_DIR}/Destrega.app.stamp)
    add_custom_command(
      OUTPUT ${APP_STAMP}
      COMMAND ${CMAKE_SOURCE_DIR}/tools/make_app.sh
              --bin $<TARGET_FILE:destrega>
              --out ${CMAKE_BINARY_DIR}/Destrega.app
              --icon ${DESTREGA_APP_ICON}
              --version ${DESTREGA_APP_VERSION}
              --sign ${DESTREGA_APP_SIGN}
      COMMAND ${CMAKE_COMMAND} -E touch ${APP_STAMP}
      DEPENDS destrega ${CMAKE_SOURCE_DIR}/tools/make_app.sh ${DESTREGA_APP_ICON}
      COMMENT "Bundling Destrega.app"
      VERBATIM)
    add_custom_target(destrega_app ALL DEPENDS ${APP_STAMP})
  endif()
endif()
