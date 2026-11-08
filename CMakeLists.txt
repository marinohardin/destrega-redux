cmake_minimum_required(VERSION 3.20)
project(destrega_recomp C)
if(APPLE)
  enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(ZLIB REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Unit tests (the ones tools/run_tests.sh compiles by hand) as CTest targets; CI turns this on.
option(DESTREGA_TESTS "Build the module unit tests as CTest targets" OFF)
# Without the disc there is no generated code. DESTREGA_STUB_GEN links the runtime against a
# stand-in (tools/ci/stub_gen) instead: the executable builds and starts, but cannot run the
# game. Used by CI; the default is the real thing (gen/ from disc/SLUS_007.32).
option(DESTREGA_STUB_GEN "Link against the CI stub instead of the recompiled game (no disc needed)" OFF)

set(GEN_DIR ${CMAKE_SOURCE_DIR}/gen)

if(DESTREGA_STUB_GEN)
  add_library(gen STATIC tools/ci/stub_gen/stub.c)
  target_include_directories(gen PRIVATE runtime tools/ci/stub_gen)
  set(GEN_INCLUDE ${CMAKE_SOURCE_DIR}/tools/ci/stub_gen)
else()
  # Regenerate C from the disc image whenever the recompiler or analysis changes.
  file(GLOB RECOMP_PY ${CMAKE_SOURCE_DIR}/recomp/*.py)
  file(GLOB ANALYSIS_JSON ${CMAKE_SOURCE_DIR}/analysis/*.json ${CMAKE_SOURCE_DIR}/analysis/overlays/*.json)
  if(NOT EXISTS ${GEN_DIR}/files.cmake)
    execute_process(COMMAND ${Python3_EXECUTABLE} recomp/recomp.py --out gen
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()
  if(NOT EXISTS ${GEN_DIR}/files.cmake)
    message(FATAL_ERROR "No generated code: recomp/recomp.py needs disc/SLUS_007.32 "
            "(python3 tools/extract_disc.py <Destrega.bin|destrega.pak> disc). "
            "To build without the game (CI), configure with -DDESTREGA_STUB_GEN=ON.")
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
  set(GEN_INCLUDE ${GEN_DIR})
endif()

add_executable(destrega
  runtime/main.c runtime/rt.c runtime/bus.c runtime/bios.c runtime/interp.c
  runtime/cdrom.c runtime/disc.c runtime/sio.c runtime/memcard.c runtime/gpu.c runtime/hires.c runtime/hires_gpu.c runtime/gte.c runtime/pgxp.c runtime/spu.c runtime/xa.c runtime/mdec.c
  runtime/overrides/override.c runtime/overrides/collision.c runtime/menu_stub.c
  runtime/overrides/widescreen.c runtime/platform.c)
target_include_directories(destrega PRIVATE runtime ${GEN_INCLUDE})
target_compile_options(destrega PRIVATE -Wall -Wno-unused-function -fno-strict-aliasing)
target_link_libraries(destrega PRIVATE gen SDL3::SDL3 ZLIB::ZLIB)
if(UNIX)
  target_link_libraries(destrega PRIVATE m)
endif()
if(WIN32)
  # Embedded manifest (UTF-8 code page for file names) and a larger main-thread stack: the
  # recompiled code nests C calls like the game nests MIPS calls, and Windows' default 1 MiB
  # is far below the 8 MiB macOS and Linux give the main thread.
  enable_language(RC)
  target_sources(destrega PRIVATE runtime/win/destrega.rc)
  set_source_files_properties(runtime/win/destrega.rc PROPERTIES
    OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/win/destrega.manifest)
  target_include_directories(destrega PRIVATE runtime/win)
  if(MSVC)
    target_link_options(destrega PRIVATE /STACK:16777216)
  else()
    target_link_options(destrega PRIVATE -Wl,--stack,16777216)
  endif()
endif()
if(APPLE)
  # native File menu (Cocoa); menu_stub.c compiles to nothing on Apple
  target_sources(destrega PRIVATE runtime/menu_mac.m)
  set_source_files_properties(runtime/menu_mac.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall")
  target_link_libraries(destrega PRIVATE "-framework Cocoa")
endif()

if(DESTREGA_TESTS)
  enable_testing()
  # same sources as tools/run_tests.sh; the tests write scratch files under build/tests,
  # relative to the source tree
  file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/build/tests)
  set(_t_gte     runtime/gte.c runtime/pgxp.c)
  set(_t_gpu     runtime/gpu.c runtime/pgxp.c)
  set(_t_hires   runtime/gpu.c runtime/hires.c runtime/pgxp.c runtime/platform.c)
  set(_t_pgxp    runtime/gte.c runtime/gpu.c runtime/hires.c runtime/pgxp.c runtime/platform.c)
  set(_t_spu     runtime/spu.c runtime/xa.c)
  set(_t_mdec    runtime/mdec.c)
  set(_t_memcard runtime/memcard.c runtime/platform.c)
  foreach(t gte gpu hires pgxp spu mdec memcard)
    add_executable(test_${t} ${_t_${t}} runtime/tests/test_${t}.c)
    target_include_directories(test_${t} PRIVATE runtime)
    target_compile_options(test_${t} PRIVATE -O2 -Wall -Wextra)
    if(UNIX)
      target_link_libraries(test_${t} PRIVATE m)
    endif()
    if(UNIX AND NOT APPLE)
      target_link_libraries(test_${t} PRIVATE pthread)
    endif()
    add_test(NAME ${t} COMMAND test_${t} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()
  # SDL_GPU backend vs software on random command streams (needs a GPU device; exit 77 =
  # skipped when there is none, e.g. a CI runner without Vulkan)
  add_executable(test_hires_gpu runtime/gpu.c runtime/hires.c runtime/hires_gpu.c runtime/pgxp.c
                 runtime/platform.c runtime/tests/test_hires_gpu.c)
  target_include_directories(test_hires_gpu PRIVATE runtime)
  target_compile_options(test_hires_gpu PRIVATE -O2 -Wall)
  target_link_libraries(test_hires_gpu PRIVATE SDL3::SDL3)
  if(UNIX)
    target_link_libraries(test_hires_gpu PRIVATE m)
  endif()
  add_test(NAME hires_gpu COMMAND test_hires_gpu auto WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(hires_gpu PROPERTIES SKIP_RETURN_CODE 77)
  if(APPLE)   # MoltenVK, when the Vulkan loader is installed (Homebrew vulkan-loader + molten-vk)
    add_test(NAME hires_gpu_vulkan COMMAND test_hires_gpu vulkan WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(hires_gpu_vulkan PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

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
