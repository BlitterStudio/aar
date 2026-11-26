# Host build of AAR apps. libaar-host provides main() and an SDL2 aar_api
# table. Apps supply aar_main. Do not link the Arm crt0, syscalls.c or the
# linker scripts of libaar.
#
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(aar-host LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# libaar's aar.c uses the GCC atomic builtins, so a GNU or Clang compiler
# (including MinGW) is required.
if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR
    "aar-host needs GCC or Clang (found ${CMAKE_C_COMPILER_ID}).")
endif()

function(aar_host_find_sdl2 out_target)
  find_package(SDL2 QUIET)
  if(TARGET SDL2::SDL2)
    set(${out_target} SDL2::SDL2 PARENT_SCOPE)
    return()
  endif()
  if(TARGET SDL2::SDL2-static)
    set(${out_target} SDL2::SDL2-static PARENT_SCOPE)
    return()
  endif()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PC_SDL2 QUIET sdl2)
  endif()
  if(PC_SDL2_FOUND)
    add_library(aar_sdl2 INTERFACE)
    target_include_directories(aar_sdl2 INTERFACE ${PC_SDL2_INCLUDE_DIRS})
    target_link_directories(aar_sdl2 INTERFACE ${PC_SDL2_LIBRARY_DIRS})
    target_link_libraries(aar_sdl2 INTERFACE ${PC_SDL2_LIBRARIES})
    set(${out_target} aar_sdl2 PARENT_SCOPE)
    return()
  endif()
  if(SDL2_LIBRARIES)
    add_library(aar_sdl2 INTERFACE)
    if(SDL2_INCLUDE_DIRS)
      target_include_directories(aar_sdl2 INTERFACE ${SDL2_INCLUDE_DIRS})
    endif()
    target_link_libraries(aar_sdl2 INTERFACE ${SDL2_LIBRARIES})
    set(${out_target} aar_sdl2 PARENT_SCOPE)
    return()
  endif()
  message(FATAL_ERROR
    "SDL2 not found. Install libsdl2-dev, or set SDL2_DIR / PKG_CONFIG_PATH.")
endfunction()

aar_host_find_sdl2(AAR_SDL2)

set(AAR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(AAR_HOST_INCLUDES
  ${AAR_ROOT}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# aar.c (the aar_* wrappers) and gl.c (the gl* wrappers) are libaar's own
# files; neither needs a libc. syscalls.c and crt0.S are not part of the host.
add_library(aar-host STATIC
  ${AAR_ROOT}/libaar/src/aar.c
  ${AAR_ROOT}/libaar/src/gl.c
  src/main.c
  src/memory.c
  src/files.c
  src/hostenv.c
  src/display.c
  src/hostgl.c
  src/input.c
  src/audio.c
  src/console.c
  src/messages.c
  src/services.c
  src/sleep.c
)
set_target_properties(aar-host PROPERTIES OUTPUT_NAME aar-host)
target_include_directories(aar-host
  PUBLIC ${AAR_HOST_INCLUDES}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${AAR_ROOT}/libaar/src
)
target_link_libraries(aar-host PUBLIC ${AAR_SDL2})
if(WIN32)
  target_link_libraries(aar-host PUBLIC user32)
else()
  # hostgl.c calls tan() for gluPerspective.
  target_link_libraries(aar-host PUBLIC m)
endif()
target_compile_options(aar-host PRIVATE -Wall -Wextra -Werror)

# name SOURCES file...
# Links the app's aar_main against libaar-host. The library already contains
# libaar's aar.c and gl.c and main(). Do not pass crt0, syscalls.c or the
# freestanding.c of libaar.
function(aar_host_executable name)
  cmake_parse_arguments(AH "" "" "SOURCES" ${ARGN})
  if(NOT AH_SOURCES)
    message(FATAL_ERROR "aar_host_executable(${name}) needs SOURCES")
  endif()
  add_executable(${name} ${AH_SOURCES})
  target_link_libraries(${name} PRIVATE aar-host)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
endfunction()

set(AAR_TEMPLATE_DIR ${AAR_ROOT}/templates)
aar_host_executable(hello SOURCES ${AAR_TEMPLATE_DIR}/hello.c)
aar_host_executable(console SOURCES ${AAR_TEMPLATE_DIR}/console.c)
aar_host_executable(filter SOURCES ${AAR_TEMPLATE_DIR}/filter.c)
aar_host_executable(freestanding SOURCES ${AAR_TEMPLATE_DIR}/freestanding.c)
aar_host_executable(ringflood SOURCES ${AAR_TEMPLATE_DIR}/ringflood.c)
aar_host_executable(echo SOURCES ${AAR_TEMPLATE_DIR}/echo.c)
aar_host_executable(pixels SOURCES ${AAR_TEMPLATE_DIR}/pixels.c)
aar_host_executable(tone SOURCES ${AAR_TEMPLATE_DIR}/tone.c)
aar_host_executable(memhog SOURCES ${AAR_TEMPLATE_DIR}/memhog.c)
# Draws through the GL group when the runner has it, and exits 2 at
# aar_gl_bind when it does not (the "nogl" test checks that).
aar_host_executable(mgl-conformance SOURCES ${AAR_TEMPLATE_DIR}/mgl_conformance.c)
aar_host_executable(gltest SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/gltest.c)
aar_host_executable(contract SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.c)
aar_host_executable(msgflood SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/msgflood.c)
aar_host_executable(conshort SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/conshort.c)
aar_host_executable(mailbox SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/mailbox.c)
aar_host_executable(api06 SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/api06.c)

# Conformance payloads (conformance/payloads) that make sense on the host. The
# host runner is the reference for app semantics (spec 11.1): the core group
# must pass. A test that needs a client or a platform prints "SKIP host: ..."
# here. There is no list to edit: a payload builds for the host when the file
# conformance/payloads/conf_<area>.host exists.
set(AAR_CONF_DIR ${AAR_ROOT}/conformance)
file(GLOB AAR_CONF_HOST_MARKERS CONFIGURE_DEPENDS
  ${AAR_CONF_DIR}/payloads/conf_*.host)
set(AAR_CONF_HOST_PAYLOADS "")
foreach(marker IN LISTS AAR_CONF_HOST_MARKERS)
  get_filename_component(marker_name ${marker} NAME_WE)
  string(REGEX REPLACE "^conf_" "" marker_area ${marker_name})
  list(APPEND AAR_CONF_HOST_PAYLOADS ${marker_area})
endforeach()
foreach(area IN LISTS AAR_CONF_HOST_PAYLOADS)
  aar_host_executable(conf_${area}
    SOURCES ${AAR_CONF_DIR}/payloads/conf_${area}.c)
endforeach()

# Direct unit checks: they include the src file under test, so they do not
# link the runner library.
set(AAR_UNIT_INCLUDES
  ${AAR_HOST_INCLUDES}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${AAR_ROOT}/libaar/src
)
# files.c uses the shared blocks of memory.c for map_file.
add_executable(files_unit tests/files_unit.c src/memory.c)
target_include_directories(files_unit PRIVATE ${AAR_UNIT_INCLUDES})
add_executable(memory_unit tests/memory_unit.c ${AAR_ROOT}/libaar/src/aar.c)
target_include_directories(memory_unit PRIVATE ${AAR_UNIT_INCLUDES})
add_executable(sleep_unit tests/sleep_unit.c)
target_include_directories(sleep_unit PRIVATE ${AAR_UNIT_INCLUDES})
target_link_libraries(sleep_unit PRIVATE ${AAR_SDL2})

# The C-FREE.8 decision of the conformance runner (plain C, no SDL).
add_executable(free_judge_test ${AAR_CONF_DIR}/tests/free_judge_test.c)
target_compile_options(free_judge_test PRIVATE -Wall -Wextra -Werror)

# The selection decisions of the conformance runner core (plain C, no SDL).
add_executable(select_test ${AAR_CONF_DIR}/tests/select_test.c)
target_compile_options(select_test PRIVATE -Wall -Wextra -Werror)

# Compiles libaar's aar.c and gl.c with AAR_FREESTANDING and freestanding.c by
# inclusion.
add_executable(freestanding_unit tests/freestanding_unit.c)
target_include_directories(freestanding_unit PRIVATE ${AAR_UNIT_INCLUDES})

# freestanding.c as its own object with the mem* names prefixed, so the test
# can define a strong aarfs_memcpy over the weak one.
add_executable(freestanding_memmove_unit
  tests/freestanding_memmove_unit.c
  ${AAR_ROOT}/libaar/src/freestanding.c)
set_source_files_properties(${AAR_ROOT}/libaar/src/freestanding.c PROPERTIES
  COMPILE_DEFINITIONS "memcpy=aarfs_memcpy;memmove=aarfs_memmove;memset=aarfs_memset;memcmp=aarfs_memcmp"
  COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")
target_compile_options(files_unit PRIVATE -Wall -Wextra)
target_compile_options(memory_unit PRIVATE -Wall -Wextra)
target_compile_options(sleep_unit PRIVATE -Wall -Wextra)
target_compile_options(freestanding_unit PRIVATE -Wall -Wextra)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(AAR_CHECK ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_runner.py)
  set(AAR_TEST_ENV "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

  # aar_host_test(name [EXE target] [ENV VAR=value])
  # runs check_runner.py <name> <exe> with the SDL dummy drivers.
  function(aar_host_test name)
    cmake_parse_arguments(HT "" "EXE;ENV" "" ${ARGN})
    if(NOT HT_EXE)
      set(HT_EXE ${name})
    endif()
    set(test_env ${AAR_TEST_ENV})
    if(HT_ENV)
      list(APPEND test_env ${HT_ENV})
    endif()
    add_test(
      NAME ${name}
      COMMAND ${Python3_EXECUTABLE} ${AAR_CHECK} ${name} $<TARGET_FILE:${HT_EXE}>
    )
    set_tests_properties(${name} PROPERTIES
      TIMEOUT 60
      ENVIRONMENT "${test_env}"
    )
  endfunction()

  aar_host_test(hello)
  aar_host_test(console)
  aar_host_test(filter)
  aar_host_test(freestanding)
  aar_host_test(ringflood)
  aar_host_test(echo)
  aar_host_test(pixels)
  aar_host_test(tone)
  aar_host_test(msgflood)
  aar_host_test(conshort)
  aar_host_test(mailbox)
  aar_host_test(constop EXE conshort)
  aar_host_test(pixelsstop EXE pixels)
  aar_host_test(msgwaitstop EXE contract)
  aar_host_test(msgwaitidle EXE contract)
  aar_host_test(nogl EXE mgl-conformance)

  # The GL group. These need a GL context (EGL offscreen for --headless,
  # Mesa llvmpipe will do), and exit 77 (skipped) where the host has none.
  # Set AAR_TEST_REQUIRE_GL=1 to make that a failure, as CI does.
  aar_host_test(glabsent EXE gltest)
  aar_host_test(gl EXE gltest)
  aar_host_test(glpump EXE gltest)
  aar_host_test(glwindow EXE gltest)
  aar_host_test(glconf EXE mgl-conformance)
  aar_host_test(glresize EXE gltest)
  set_tests_properties(gl glpump glwindow glconf glresize PROPERTIES SKIP_RETURN_CODE 77)
  set_tests_properties(glconf PROPERTIES TIMEOUT 180)
  aar_host_test(keys EXE hello ENV "AAR_HOST_SELFTEST=1")
  aar_host_test(contract)

  # The API 0.6 calls and events (tests/api06.c); api06_pads needs SDL's
  # virtual joystick and exits 77 where that is missing.
  foreach(t files time locale env clip display present resize events input pads window)
    aar_host_test(api06_${t} EXE api06)
  endforeach()
  set_tests_properties(api06_pads api06_window PROPERTIES SKIP_RETURN_CODE 77)

  # conf_<area> runs the payload and checks its AAR-CONF lines against
  # conformance/TESTS.md; conf_catalogue checks the catalogue itself.
  foreach(area IN LISTS AAR_CONF_HOST_PAYLOADS)
    string(TOUPPER ${area} AREA_UPPER)
    add_test(
      NAME conf_${area}
      COMMAND ${Python3_EXECUTABLE} ${AAR_CONF_DIR}/tools/check_results.py
              payload --area C-${AREA_UPPER} --profile host --
              $<TARGET_FILE:conf_${area}> --headless
    )
    set_tests_properties(conf_${area} PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "${AAR_TEST_ENV}"
    )
  endforeach()
  add_test(
    NAME conf_catalogue
    COMMAND ${Python3_EXECUTABLE} ${AAR_CONF_DIR}/tools/conf_catalogue.py check
  )
  add_test(
    NAME conf_tools
    COMMAND ${Python3_EXECUTABLE} ${AAR_CONF_DIR}/tests/tools_test.py
  )
  add_test(NAME files_unit COMMAND files_unit)
  add_test(NAME memory_unit COMMAND memory_unit)
  add_test(NAME sleep_unit COMMAND sleep_unit)
  add_test(NAME conf_free_judge COMMAND free_judge_test)
  add_test(NAME conf_select COMMAND select_test)
  add_test(NAME freestanding_unit COMMAND freestanding_unit)
  add_test(NAME freestanding_memmove_unit COMMAND freestanding_memmove_unit)
endif()
