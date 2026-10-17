# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Include only main and what it depends on
idf_build_set_property(MINIMAL_BUILD ON)
project(auracast_s31)
