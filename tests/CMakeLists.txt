# Host-side GoogleTest suites for the platform-independent code in common/.
#
#   cmake --build tests/build


cmake_minimum_required(VERSION 3.24)
project(rn_gstreamer_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(googletest.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-base-1.0
    gstreamer-video-1.0)

# CTest timeout for each test suite
set(SUITE_TIMEOUT 120 CACHE STRING "Per-suite timeout in seconds")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/build)
target_link_libraries(rngstcommon PUBLIC PkgConfig::GST)

set(WARNINGS -Wall -Wno-deprecated-declarations)
enable_testing()

add_subdirectory(common_tests)