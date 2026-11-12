cmake_minimum_required(VERSION 3.20)
project(destrega_recomp C)
if(APPLE)
  enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
  # MinGW GCC implements _Thread_local with emutls on top of libwinpthread-1.dll; link
  # libgcc and winpthread statically so the .exe needs only SDL3.dll and the system UCRT.
  add_link_options(-static)
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

# Per-function execution coverage of the recompiled game (tools/coverage.py): slows nothing else down.
option(DESTREGA_COVERAGE "Record which recompiled functions run (DESTREGA_COVERAGE_OUT)" OFF)
if(DESTREGA_COVERAGE)
  add_compile_definitions(DESTREGA_COVERAGE=1)
endif()

# Game module layout (the launcher, docs/RELEASES.md option A): the executable holds no game
# code; the recompiled game is a shared library loaded at start-up (runtime/game_abi.h). The
# launcher builds it on the player's machine from their disc (runtime/launcher/). With
# DESTREGA_DEV_GAME_MODULE CMake also builds it from gen/ next to the executable (development);
# without it nothing here needs the disc or gen/ (what a launcher release is built from).
option(DESTREGA_GAME_MODULE "Executable without game code; the game is a module built by the launcher" OFF)
option(DESTREGA_DEV_GAME_MODULE "With DESTREGA_GAME_MODULE: also build destrega_game from gen/ (development)" ON)
set(_need_gen ON)
if(DESTREGA_GAME_MODULE AND NOT DESTREGA_DEV_GAME_MODULE)
  set(_need_gen OFF)
endif()
if(DESTREGA_GAME_MODULE AND DESTREGA_STUB_GEN)
  message(FATAL_ERROR "DESTREGA_GAME_MODULE builds no game code; use it without DESTREGA_STUB_GEN")
endif()

set(GEN_DIR ${CMAKE_SOURCE_DIR}/gen)

if(NOT _need_gen)
  set(GEN_INCLUDE "")
elseif(DESTREGA_STUB_GEN)
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
            ${CMAKE_SOURCE_DIR}/analysis/idle_loops.txt
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Recompiling MIPS code to C")

  if(DESTREGA_GAME_MODULE)
    # development module: the same objects the launcher builds (runtime/launcher/launcher.c flags)
    add_library(destrega_game SHARED ${GEN_SOURCES} runtime/game_module.c)
    target_include_directories(destrega_game PRIVATE runtime gen ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_options(destrega_game PRIVATE -O1 -w -fno-strict-aliasing -fvisibility=hidden
                           -include ${CMAKE_SOURCE_DIR}/runtime/game_module_side.h)
    set_target_properties(destrega_game PROPERTIES PREFIX "")
    target_sources(destrega_game PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/game_sdk.h)
  else()
    add_library(gen STATIC ${GEN_SOURCES})
    target_include_directories(gen PRIVATE runtime gen)
    # Generated code is huge and goto-heavy; -O1 keeps build times sane.
    target_compile_options(gen PRIVATE -O1 -w -fno-strict-aliasing)
  endif()
  set(GEN_INCLUDE ${GEN_DIR})
endif()

add_executable(destrega
  runtime/main.c runtime/rt.c runtime/bus.c runtime/bios.c runtime/interp.c
  runtime/cdrom.c runtime/disc.c runtime/sio.c runtime/memcard.c runtime/gpu.c runtime/hires.c runtime/hires_gpu.c runtime/gte.c runtime/pgxp.c runtime/spu.c runtime/xa.c runtime/mdec.c
  runtime/overrides/override.c runtime/overrides/collision.c runtime/menu_stub.c
  runtime/overrides/widescreen.c runtime/overrides/char_mesh.c runtime/platform.c runtime/platform_net.c
  runtime/savestate.c runtime/replay.c runtime/netplay.c runtime/net.c runtime/smooth.c runtime/portmap.c
  runtime/netplay_ui.c runtime/input.c runtime/settings.c
  runtime/menu_overlay.c runtime/hlr/hlr.c runtime/hlr/hlr_stage.c runtime/hlr/hlr_char.c runtime/hlr/hlr_native.c runtime/hlr/hlr_gpu.c)
target_include_directories(destrega PRIVATE runtime ${GEN_INCLUDE} ${CMAKE_CURRENT_BINARY_DIR})
# Emulation source identity for the netplay handshake (rule in tools/source_ident.py): the same
# on every platform for the same sources.
file(GLOB _ident_deps CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/*.c ${CMAKE_SOURCE_DIR}/runtime/*.h
  ${CMAKE_SOURCE_DIR}/runtime/overrides/*.c ${CMAKE_SOURCE_DIR}/runtime/overrides/*.h
  ${CMAKE_SOURCE_DIR}/runtime/launcher/recompiler.c ${CMAKE_SOURCE_DIR}/runtime/launcher/recompiler.h
  ${CMAKE_SOURCE_DIR}/recomp/*.py ${CMAKE_SOURCE_DIR}/analysis/*.txt ${CMAKE_SOURCE_DIR}/analysis/*.json
  ${CMAKE_SOURCE_DIR}/analysis/overlays/*)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/source_ident.h
  COMMAND ${Python3_EXECUTABLE} tools/source_ident.py ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/source_ident.h
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/source_ident.py ${_ident_deps}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Emulation source identity (netplay)")
target_sources(destrega PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/source_ident.h)
target_compile_options(destrega PRIVATE -Wall -Wno-unused-function -fno-strict-aliasing)
target_link_libraries(destrega PRIVATE SDL3::SDL3 ZLIB::ZLIB)
if(DESTREGA_GAME_MODULE)
  # module SDK + analysis inputs embedded for the launcher (ids: tools/embed_files.py)
  file(GLOB _embed_deps CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/analysis/*.txt ${CMAKE_SOURCE_DIR}/analysis/*.json
       ${CMAKE_SOURCE_DIR}/analysis/overlays/*.json)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/game_sdk.h ${CMAKE_CURRENT_BINARY_DIR}/game_sdk_files.c
    COMMAND ${Python3_EXECUTABLE} tools/embed_files.py ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS tools/embed_files.py runtime/psx.h runtime/recomp.h runtime/pgxp.h runtime/game_abi.h
            runtime/game_module_side.h runtime/game_module.c runtime/launcher/recompiler.c
            runtime/launcher/recompiler.h ${_embed_deps}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Embedding the game module SDK")
  target_compile_definitions(destrega PRIVATE DESTREGA_GAME_MODULE=1)
  target_sources(destrega PRIVATE runtime/game_load.c runtime/launcher/launcher.c runtime/launcher/proc.c
                 runtime/launcher/sha256.c runtime/launcher/recompiler.c
                 ${CMAKE_CURRENT_BINARY_DIR}/game_sdk_files.c ${CMAKE_CURRENT_BINARY_DIR}/game_sdk.h)
  if(APPLE)
    target_sources(destrega PRIVATE runtime/launcher/launcher_ui_mac.m)
    set_source_files_properties(runtime/launcher/launcher_ui_mac.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall")
  elseif(WIN32)
    target_sources(destrega PRIVATE runtime/launcher/launcher_ui_win.c)
    target_link_libraries(destrega PRIVATE comctl32)
  else()
    target_sources(destrega PRIVATE runtime/launcher/launcher_ui_sdl.c)
  endif()
  target_link_libraries(destrega PRIVATE ${CMAKE_DL_LIBS})
  if(TARGET destrega_game)
    add_dependencies(destrega destrega_game)
  endif()
else()
  target_link_libraries(destrega PRIVATE gen)
endif()
if(UNIX)
  target_link_libraries(destrega PRIVATE m)
endif()
if(WIN32)
  # netplay UDP (net.c), local address list (platform.c), native menus (menu_win.c)
  target_link_libraries(destrega PRIVATE ws2_32 iphlpapi)
  target_sources(destrega PRIVATE runtime/menu_win.c runtime/controls_win.c)
  # Embedded manifest (UTF-8 code page for file names) and a larger main-thread stack: the
  # recompiled code nests C calls like the game nests MIPS calls, and Windows' default 1 MiB
  # is far below the 8 MiB macOS and Linux give the main thread.
  # plus the icon and version info (version from VERSION + git, regenerated every build but
  # only rewritten when it changes).
  enable_language(RC)
  add_custom_target(destrega_version
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/version.py rc-header
            ${CMAKE_CURRENT_BINARY_DIR}/destrega_version.h
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/destrega_version.h
    COMMENT "Version resource header" VERBATIM)
  add_dependencies(destrega destrega_version)
  target_sources(destrega PRIVATE runtime/win/destrega.rc)
  set_source_files_properties(runtime/win/destrega.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/win/destrega.manifest;${CMAKE_SOURCE_DIR}/runtime/win/destrega.ico;${CMAKE_CURRENT_BINARY_DIR}/destrega_version.h")
  target_include_directories(destrega PRIVATE runtime/win ${CMAKE_CURRENT_BINARY_DIR})
  if(MSVC)
    target_link_options(destrega PRIVATE /STACK:16777216)
  else()
    target_link_options(destrega PRIVATE -Wl,--stack,16777216)
  endif()
endif()
if(APPLE)
  # native File menu (Cocoa); menu_stub.c compiles to nothing on Apple
  target_sources(destrega PRIVATE runtime/menu_mac.m runtime/controls_mac.m)
  set_source_files_properties(runtime/menu_mac.m runtime/controls_mac.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall")
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
  # input mapping + settings file (needs SDL for key names, not a display)
  add_executable(test_input runtime/input.c runtime/settings.c runtime/platform.c runtime/tests/test_input.c)
  target_include_directories(test_input PRIVATE runtime)
  target_compile_options(test_input PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(test_input PRIVATE SDL3::SDL3)
  if(UNIX)
    target_link_libraries(test_input PRIVATE m)
  endif()
  add_test(NAME input COMMAND test_input WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  # port mapping for the netplay host: PCP / NAT-PMP / UPnP encoding and parsing (no network use)
  add_executable(test_portmap runtime/portmap.c runtime/settings.c runtime/platform.c runtime/platform_net.c
                 runtime/tests/test_portmap.c)
  target_include_directories(test_portmap PRIVATE runtime)
  target_compile_options(test_portmap PRIVATE -O2 -Wall -Wextra)
  if(WIN32)
    target_link_libraries(test_portmap PRIVATE ws2_32 iphlpapi)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(test_portmap PRIVATE pthread)
  endif()
  add_test(NAME portmap COMMAND test_portmap WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  # SDL_GPU backend vs software on random command streams (needs a GPU device; exit 77 =
  # skipped when there is none, e.g. a CI runner without Vulkan)
  add_executable(test_hires_gpu runtime/gpu.c runtime/hires.c runtime/hires_gpu.c runtime/pgxp.c
                 runtime/platform.c runtime/hlr/hlr_gpu.c runtime/tests/test_hires_gpu.c)
  target_include_directories(test_hires_gpu PRIVATE runtime)
  target_compile_options(test_hires_gpu PRIVATE -O2 -Wall)
  target_link_libraries(test_hires_gpu PRIVATE SDL3::SDL3)
  if(UNIX)
    target_link_libraries(test_hires_gpu PRIVATE m)
  endif()
  add_test(NAME hires_gpu COMMAND test_hires_gpu auto WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(hires_gpu PROPERTIES SKIP_RETURN_CODE 77)
  # View > Renderer's API switch: shut down / restart the backend on every available API in turn
  add_test(NAME hires_gpu_switch COMMAND test_hires_gpu switch WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(hires_gpu_switch PROPERTIES SKIP_RETURN_CODE 77)
  # fast precision (--gpu-precision fast): the same streams within a tolerance
  add_test(NAME hires_gpu_fast COMMAND test_hires_gpu fast auto WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(hires_gpu_fast PROPERTIES SKIP_RETURN_CODE 77)
  # HLR: an empty external render pass mid-stream leaves the target unchanged
  add_test(NAME hlr_gpu COMMAND test_hires_gpu external auto WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(hlr_gpu PROPERTIES SKIP_RETURN_CODE 77)
  # HLR phase 2a without the disc: switches, a synthetic stage through the capture and audit,
  # frame pairing in the linked-list filter, save-state drop, gpu.c suppression
  add_executable(test_hlr runtime/hlr/hlr.c runtime/hlr/hlr_stage.c runtime/hlr/hlr_char.c runtime/hlr/hlr_native.c runtime/hlr/hlr_gpu.c
                 runtime/gte.c runtime/gpu.c runtime/hires.c runtime/hires_gpu.c runtime/pgxp.c runtime/platform.c
                 runtime/tests/test_hlr.c)
  target_include_directories(test_hlr PRIVATE runtime)
  target_compile_options(test_hlr PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(test_hlr PRIVATE SDL3::SDL3)
  if(UNIX)
    target_link_libraries(test_hlr PRIVATE m)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(test_hlr PRIVATE pthread)
  endif()
  add_test(NAME hlr COMMAND test_hlr WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  if(APPLE)   # MoltenVK, when the Vulkan loader is installed (Homebrew vulkan-loader + molten-vk)
    add_test(NAME hires_gpu_vulkan COMMAND test_hires_gpu vulkan WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(hires_gpu_vulkan PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME hlr_gpu_vulkan COMMAND test_hires_gpu external vulkan WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(hlr_gpu_vulkan PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

# macOS: also wrap the binary in a self-contained Destrega.app (bundled SDL3,
# app icon, ad-hoc signature). The plain `destrega` binary is left in place for
# the CLI/headless flags and tools/run_tests.sh.
if(APPLE)
  option(DESTREGA_MACOS_APP "Build Destrega.app next to the destrega binary" ON)
  set(DESTREGA_APP_VERSION "" CACHE STRING "CFBundleShortVersionString of Destrega.app (empty = tools/version.py: VERSION file + git)")
  set(DESTREGA_APP_ICON ${CMAKE_SOURCE_DIR}/assets/icon/destrega.png CACHE FILEPATH
      "App icon (PNG >= 512px or .icns)")
  set(DESTREGA_APP_SIGN "-" CACHE STRING "codesign identity for Destrega.app (- = ad-hoc)")
  if(DESTREGA_MACOS_APP)
    set(_app_version_args "")
    if(DESTREGA_APP_VERSION)
      set(_app_version_args --version ${DESTREGA_APP_VERSION})
    endif()
    set(APP_STAMP ${CMAKE_BINARY_DIR}/Destrega.app.stamp)
    add_custom_command(
      OUTPUT ${APP_STAMP}
      COMMAND ${CMAKE_SOURCE_DIR}/tools/make_app.sh
              --bin $<TARGET_FILE:destrega>
              --out ${CMAKE_BINARY_DIR}/Destrega.app
              --icon ${DESTREGA_APP_ICON}
              ${_app_version_args}
              --sign ${DESTREGA_APP_SIGN}
      COMMAND ${CMAKE_COMMAND} -E touch ${APP_STAMP}
      DEPENDS destrega ${CMAKE_SOURCE_DIR}/tools/make_app.sh ${CMAKE_SOURCE_DIR}/tools/version.py ${CMAKE_SOURCE_DIR}/VERSION ${DESTREGA_APP_ICON}
      COMMENT "Bundling Destrega.app"
      VERBATIM)
    add_custom_target(destrega_app ALL DEPENDS ${APP_STAMP})
  endif()
endif()
