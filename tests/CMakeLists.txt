# Host-side GstCheck suites for the platform-independent code in common/.
#
#   cmake -S tests -B tests/build
#   cmake --build tests/build
#   ctest --test-dir tests/build --output-on-failure
#   ctest --test-dir tests/build -R codec
#
# Android has its own build (tests/android): a cross-compile needs the NDK
# toolchain file, and the suites run on a device rather than the host.

cmake_minimum_required(VERSION 3.22)
project(rn_gstreamer_tests C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-check-1.0
    gstreamer-video-1.0)

set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# GStreamer's macOS plugins touch CoreFoundation, which cannot be used in a
# forked child without exec(), so fork mode aborts there in any test that
# instantiates one. Everywhere else keep it on: it is what gives GstCheck its
# per-test isolation. CTest supplies the timeout either way.
if(APPLE)
    set(CK_FORK_DEFAULT no)
else()
    set(CK_FORK_DEFAULT yes)
endif()
set(CK_FORK ${CK_FORK_DEFAULT} CACHE STRING "GstCheck fork mode (yes/no)")

set(SUITE_TIMEOUT 120 CACHE STRING "Per-suite timeout in seconds")

enable_testing()

# One executable per suite, linking only the code it exercises, so a failure
# points at one file.
function(add_gst_suite name)
    set(target check_${name})

    add_executable(${target}
        common/check_${name}.c
        host/platform.c
        ${ARGN})

    target_include_directories(${target} PRIVATE
        ${COMMON_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(${target} PRIVATE PkgConfig::GST)

    target_compile_options(${target} PRIVATE -Wall -Wno-deprecated-declarations)
    if(APPLE)
        # cb_error() dispatches a block to the main queue on Apple platforms.
        target_compile_options(${target} PRIVATE -fblocks)
    endif()

    add_test(NAME ${name} COMMAND ${target})
    set_tests_properties(${name} PROPERTIES
        TIMEOUT ${SUITE_TIMEOUT}
        ENVIRONMENT "CK_FORK=${CK_FORK}")
endfunction()

add_gst_suite(codec
    ${COMMON_DIR}/src/gstreamer_codec.c)

add_gst_suite(backend
    ${COMMON_DIR}/src/gstreamer_backend.c
    ${COMMON_DIR}/src/gstreamer_codec.c
    ${COMMON_DIR}/src/gstreamer_encoder.c
    ${COMMON_DIR}/src/gstreamer_event_recorder.c
    ${COMMON_DIR}/src/gstreamer_recorder.c
    ${COMMON_DIR}/src/gstreamer_video_writer.c)
