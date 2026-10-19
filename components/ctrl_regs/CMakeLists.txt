# SPDX-License-Identifier: Apache-2.0
# Register file of the I2C control interface: builds as an ESP-IDF component or as a host library for tests.
if(ESP_PLATFORM)
    idf_component_register(SRCS ctrl_regs.c
                           INCLUDE_DIRS include)
else()
    add_library(ctrl_regs STATIC ctrl_regs.c)
    target_include_directories(ctrl_regs PUBLIC include)
endif()
