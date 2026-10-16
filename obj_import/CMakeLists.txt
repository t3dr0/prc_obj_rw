cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(CMAKE_C_STANDARD 99)

# fast_obj's own CMakeLists.txt is not used: at the pinned tag (v1.3) it
# declares a minimum CMake version that current CMake rejects. Its one source
# file is compiled here instead.
add_executable(nano_prc_obj_import
               src/obj_import.c
               ${FAST_OBJ_DIR}/fast_obj.c)

target_link_libraries(nano_prc_obj_import PRIVATE nano_prc)
target_include_directories(nano_prc_obj_import PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/src
                           ${FAST_OBJ_DIR})

# Only link the math library if we aren't targeting Windows
if(NOT WIN32)
    target_link_libraries(nano_prc_obj_import PRIVATE m)
endif()
