# Copyright © 2026 Nilton Volpato
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16.0)

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../../tether-worktree/main/cpp/tether"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../tether-worktree/main/cpp/tether_idf")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(coprocessor_qemu)
