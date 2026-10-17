# SPDX-License-Identifier: Apache-2.0
# Host (Linux/gcc) tests for the audio_dsp component:
#   cmake -S components/audio_dsp/host_test -B build_host && cmake --build build_host && ctest --test-dir build_host --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(audio_dsp_host_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-O2 -Wall -Wextra -Werror)

add_subdirectory(.. audio_dsp)

enable_testing()
file(GLOB TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
foreach(src ${TEST_SRCS})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE audio_dsp m)
    add_test(NAME ${name} COMMAND ${name})
endforeach()
