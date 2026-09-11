# GstCheck suites for the platform-independent code in common/, built for
# Android and run on an emulator or an attached device.
#
#   cmake -S tests/android -B tests/android/build \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-27 \
#     -DGSTREAMER_ROOT_ANDROID=$HOME/code/luna-oculus/GStreamer-Android
#   cmake --build tests/android/build
#   ctest --test-dir tests/android/build --output-on-failure
#   ctest --test-dir tests/android/build -R codec
#
# Each test boots a headless emulator through emulator.sh if one is not already
# up; TARGET=device in the environment runs against an attached phone instead.

cmake_minimum_required(VERSION 3.22)

# CXX for the same reason as the app: GStreamerMobile sets LINKER_LANGUAGE CXX.
project(rn_gstreamer_tests_android C CXX)

set(TESTS_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(MODULE_ROOT ${TESTS_DIR}/..)
set(COMMON_DIR ${MODULE_ROOT}/common)

# The suite and libgstreamer_android.so land together, so the runner pushes one
# directory. Set before find_package, which creates the GStreamerMobile target.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)

# The suites are bare executables and never load the Java binding, but the find
# module copies GStreamer.java somewhere regardless; keep it out of the tree.
set(GStreamer_JAVA_SRC_DIR ${CMAKE_BINARY_DIR}/java)

# The backend suite drives the isDebugging pipeline (videotestsrc ! glimagesink),
# which a default app build leaves out. Set before the include: gst-plugins.cmake
# declares this with option(), which honours an existing cache entry.
set(GST_DEBUG_PIPELINE ON CACHE BOOL "Include plugins for the isDebugging test pipeline" FORCE)

# ABI mapping, plugin set and extra deps, shared with the app's android/ build
# so the suites register the same plugins production does.
include(${MODULE_ROOT}/android/gst-android-config.cmake)

# androidmedia's plugin_init reaches JNI_CreateJavaVM during gst_init(), which
# aborts in a bare executable. Covering amc* needs an instrumented test.
list(REMOVE_ITEM GSTREAMER_PLUGINS androidmedia)

list(APPEND GStreamer_EXTRA_DEPS gstreamer-check-1.0)

find_package(GStreamerMobile REQUIRED COMPONENTS
    api_player
    api_video
    ${GSTREAMER_PLUGINS})

set(SUITE_TIMEOUT 300 CACHE STRING "Per-suite timeout in seconds")

enable_testing()

# One executable per suite, linking only the code it exercises, so a failure
# points at one file.
function(add_gst_suite name)
    set(target check_${name})

    add_executable(${target}
        ${TESTS_DIR}/common/check_${name}.c
        ${TESTS_DIR}/android/platform.c
        ${ARGN})

    target_include_directories(${target} PRIVATE
        ${COMMON_DIR}/include
        ${TESTS_DIR})

    target_link_libraries(${target} PRIVATE GStreamer::mobile log android)

    target_compile_options(${target} PRIVATE -Wall -Wno-deprecated-declarations)

    # Keep the suites loadable on 16KB page-size devices, like the app.
    target_link_options(${target} PRIVATE
        -Wl,-z,max-page-size=16384
        -Wl,-z,common-page-size=16384)

    add_test(NAME ${name}
        COMMAND ${CMAKE_CURRENT_LIST_DIR}/run-on-device.sh $<TARGET_FILE:${target}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT ${SUITE_TIMEOUT})
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
