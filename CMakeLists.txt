cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

# prc_obj_rw builds in one of two ways:
#
#  - as a subdirectory of nanoPRC (a git submodule there): the nano_prc target
#    already exists and is used as is;
#  - on its own: NANOPRC_SOURCE_DIR names a nanoPRC checkout, which is added
#    here with its demos, tests and SDL switched off.
#
# This repository must never contain nanoPRC as a submodule. nanoPRC contains
# this one, and each containing the other would nest copies without end under
# `git submodule update --recursive`.

if(NOT TARGET nano_prc)
    project(prc_obj_rw C)

    set(NANOPRC_SOURCE_DIR "" CACHE PATH "Path to a nanoPRC checkout")
    if(NOT EXISTS "${NANOPRC_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "prc_obj_rw needs nanoPRC: configure with -DNANOPRC_SOURCE_DIR=<path to a nanoPRC checkout>")
    endif()

    set(NANOPRC_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(NANOPRC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NANOPRC_BUILD_SDL   OFF CACHE BOOL "" FORCE)
    add_subdirectory("${NANOPRC_SOURCE_DIR}" nanoprc)

    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    endif()

    # nanoPRC sets its own output directory, so its shared library would land
    # apart from the tools and they would not run from the build directory.
    set_target_properties(nano_prc PROPERTIES
                          RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()

set(FAST_OBJ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/fast_obj" CACHE PATH
    "Path to the fast_obj sources (git submodule thirdparty/fast_obj)")

if(EXISTS "${FAST_OBJ_DIR}/fast_obj.h")
    add_subdirectory(obj_import)
else()
    message(WARNING
        "fast_obj not found at ${FAST_OBJ_DIR}: obj_import is not built. "
        "Run `git submodule update --init` in prc_obj_rw.")
endif()

add_subdirectory(obj_export)
