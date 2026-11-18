# aar_add_executable(name SOURCES src... [FREESTANDING] [PROFILE a32|a64])
# Links crt0, libaar and the PIC sysroot the same way mk/aar.mk does, then
# runs aar-check. Include this after project(), with aar-toolchain-a32.cmake or
# aar-toolchain-a64.cmake as CMAKE_TOOLCHAIN_FILE.
#
# The profile comes from the toolchain file: one build directory is one
# profile. PROFILE is optional; when given it must name that profile, so a
# CMakeLists written for one profile fails loudly under the other file.
#
# FREESTANDING links without newlib, like AAR_FREESTANDING=1 in mk/aar.mk:
# crt0 calls aar_exit, syscalls.c and malloc are left out, the link takes
# -lgcc but no -lc or -lm, and a post-build step fails when the link map holds
# an archive other than libaar.a and libgcc.a. Configuring with
# -DAAR_FREESTANDING=ON makes every target freestanding. See the developer
# guide under docs/.
#
# SPDX-License-Identifier: MIT

if(NOT AAR_SDK)
  if(DEFINED ENV{AAR_SDK} AND NOT "$ENV{AAR_SDK}" STREQUAL "")
    set(AAR_SDK "$ENV{AAR_SDK}")
  else()
    get_filename_component(AAR_SDK "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  endif()
endif()

if(NOT AAR_PROFILE)
  message(FATAL_ERROR
    "Configure with -DCMAKE_TOOLCHAIN_FILE=.../cmake/aar-toolchain-a32.cmake "
    "(or -a64.cmake)")
endif()

if(NOT PYTHON)
  if(DEFINED ENV{PYTHON} AND NOT "$ENV{PYTHON}" STREQUAL "")
    set(PYTHON "$ENV{PYTHON}")
  else()
    find_program(PYTHON NAMES python3 python REQUIRED)
  endif()
endif()

set(CMAKE_EXECUTABLE_SUFFIX ".elf")

function(aar_add_executable name)
  cmake_parse_arguments(AA "FREESTANDING" "PROFILE" "SOURCES" ${ARGN})
  if(NOT AA_SOURCES)
    message(FATAL_ERROR "aar_add_executable(${name}) needs SOURCES")
  endif()
  if(AA_PROFILE AND NOT AA_PROFILE STREQUAL AAR_PROFILE)
    message(FATAL_ERROR
      "aar_add_executable(${name}) asks for profile ${AA_PROFILE}, but this "
      "build directory is configured for ${AAR_PROFILE}. Configure another "
      "directory with cmake/aar-toolchain-${AA_PROFILE}.cmake.")
  endif()
  if(AA_FREESTANDING OR AAR_FREESTANDING)
    set(freestanding TRUE)
  else()
    set(freestanding FALSE)
  endif()

  if(freestanding)
    add_executable(${name}
      "${AAR_SDK}/libaar/${AAR_PROFILE}/crt0.S"
      "${AAR_SDK}/libaar/src/aar.c"
      "${AAR_SDK}/libaar/src/gl.c"
      "${AAR_SDK}/libaar/src/freestanding.c"
      ${AA_SOURCES})
    # The mem* routines must not be rewritten into calls to themselves.
    set_source_files_properties("${AAR_SDK}/libaar/src/freestanding.c" PROPERTIES
      COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")
    target_compile_definitions(${name} PRIVATE AAR_FREESTANDING=1)
    target_link_libraries(${name} PRIVATE gcc)
  else()
    add_executable(${name}
      "${AAR_SDK}/libaar/${AAR_PROFILE}/crt0.S"
      "${AAR_SDK}/libaar/src/aar.c"
      "${AAR_SDK}/libaar/src/gl.c"
      "${AAR_SDK}/libaar/src/syscalls.c"
      ${AA_SOURCES})
    target_link_libraries(${name} PRIVATE
      -Wl,--start-group c gcc m -Wl,--end-group)
  endif()
  target_link_options(${name} PRIVATE "-Wl,-Map=$<TARGET_FILE:${name}>.map")
  target_include_directories(${name} PRIVATE
    "${AAR_SDK}/include"
    "${AAR_SDK}/libaar/src")
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${PYTHON} "${AAR_SDK}/tools/aar-check" --profile ${AAR_PROFILE}
            "$<TARGET_FILE:${name}>"
    VERBATIM)
  if(freestanding)
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${PYTHON} "${AAR_SDK}/tools/aar-check" --no-libc
              "$<TARGET_FILE:${name}>.map"
      VERBATIM)
  endif()
endfunction()
