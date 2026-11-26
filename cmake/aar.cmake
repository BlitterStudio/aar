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
# A source ending in .cpp, .cc or .cxx makes the target a C++ target: the
# project must enable CXX (project(app C CXX ASM)), the link takes libstdc++
# and libgcc_eh from the sysroot (build_pic_libstdcxx.sh) and the script
# libaar/<profile>/aar-cxx.ld, and libaar/src/cxx.c is linked in. FREESTANDING
# targets refuse C++ sources. -DAAR_CXX_EXCEPTIONS=OFF compiles C++ without
# exceptions.
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

  set(has_cxx FALSE)
  foreach(src IN LISTS AA_SOURCES)
    get_filename_component(ext "${src}" LAST_EXT)
    string(TOLOWER "${ext}" ext)
    if(ext STREQUAL ".cpp" OR ext STREQUAL ".cc" OR ext STREQUAL ".cxx")
      set(has_cxx TRUE)
    endif()
  endforeach()
  if(has_cxx)
    get_property(enabled_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    if(NOT "CXX" IN_LIST enabled_languages)
      message(FATAL_ERROR
        "aar_add_executable(${name}) has C++ sources: enable CXX, for "
        "example project(<name> C CXX ASM)")
    endif()
    if(freestanding)
      message(FATAL_ERROR
        "aar_add_executable(${name}) has C++ sources and is freestanding: "
        "the freestanding flavour has no C++ runtime, use the libc flavour")
    endif()
    set(ld_script "${AAR_SDK}/libaar/${AAR_PROFILE}/aar-cxx.ld")
  else()
    set(ld_script "${AAR_SDK}/libaar/${AAR_PROFILE}/aar.ld")
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
    set(cxx_support "")
    if(has_cxx)
      set(cxx_support "${AAR_SDK}/libaar/src/cxx.c")
    endif()
    add_executable(${name}
      "${AAR_SDK}/libaar/${AAR_PROFILE}/crt0.S"
      "${AAR_SDK}/libaar/src/aar.c"
      "${AAR_SDK}/libaar/src/gl.c"
      "${AAR_SDK}/libaar/src/syscalls.c"
      ${cxx_support}
      ${AA_SOURCES})
    if(has_cxx)
      target_link_libraries(${name} PRIVATE
        -Wl,--start-group stdc++ c gcc_eh gcc m -Wl,--end-group)
    else()
      target_link_libraries(${name} PRIVATE
        -Wl,--start-group c gcc m -Wl,--end-group)
    endif()
  endif()
  target_link_options(${name} PRIVATE
    "-Wl,-T,${ld_script}"
    "-Wl,-Map=$<TARGET_FILE:${name}>.map")
  set_target_properties(${name} PROPERTIES LINK_DEPENDS "${ld_script}")
  if(has_cxx)
    # Per target and read at every configure, so flipping AAR_CXX_EXCEPTIONS in
    # an existing build directory takes effect.
    if(NOT DEFINED AAR_CXX_EXCEPTIONS OR AAR_CXX_EXCEPTIONS)
      target_compile_options(${name} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fexceptions>")
    else()
      target_compile_options(${name} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-unwind-tables>"
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-asynchronous-unwind-tables>")
    endif()
  endif()
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
