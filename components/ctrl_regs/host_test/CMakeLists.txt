# SPDX-License-Identifier: Apache-2.0
# Host (Linux/gcc) tests for the ctrl_regs component:
#   cmake -S components/ctrl_regs/host_test -B build_host_ctrl && cmake --build build_host_ctrl && ctest --test-dir build_host_ctrl --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(ctrl_regs_host_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-O2 -Wall -Wextra -Werror)

add_subdirectory(.. ctrl_regs)

enable_testing()
add_executable(test_ctrl_regs test_ctrl_regs.c)
target_link_libraries(test_ctrl_regs PRIVATE ctrl_regs)
add_test(NAME test_ctrl_regs COMMAND test_ctrl_regs)
