# GstCheck suites cross-compiled for the iOS Simulator.
#
#   cmake -S tests/ios -B tests/ios/build \
#         -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
#         -DCMAKE_OSX_ARCHITECTURES=arm64
#   cmake --build tests/ios/build
#
# The iOS GStreamer distribution is a repackaged xcframework: one merged
# libGStreamer.a with no pkg-config and no separate plugin libraries, so the
# link recipe is spelled out here rather than coming from FindGStreamerMobile.

cmake_minimum_required(VERSION 3.22)
project(rn_gstreamer_ios_tests C)

set(GSTREAMER_XCFRAMEWORK "$ENV{HOME}/code/luna-oculus/GStreamer-iOS/GStreamer.xcframework"
    CACHE PATH "Path to GStreamer.xcframework")
set(CMAKE_OSX_DEPLOYMENT_TARGET 18.0)

if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(GST_SLICE "ios-arm64_x86_64-simulator")
else()
    set(GST_SLICE "ios-arm64")
endif()

# find_library is re-rooted into the SDK on iOS (CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
# is ONLY), so without NO_CMAKE_FIND_ROOT_PATH it cannot see the xcframework.
find_library(GSTREAMER_LIBRARY
    NAMES GStreamer
    PATHS "${GSTREAMER_XCFRAMEWORK}/${GST_SLICE}"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
    REQUIRED)
message(STATUS "GStreamer: ${GSTREAMER_LIBRARY}")

add_library(GStreamer::static STATIC IMPORTED)
set_target_properties(GStreamer::static PROPERTIES
    IMPORTED_LOCATION "${GSTREAMER_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${GSTREAMER_XCFRAMEWORK}/${GST_SLICE}/Headers")

set(GST_SYSTEM_LIBS
    "-ObjC" resolv iconv c++ z bz2
    "-framework Foundation" "-framework UIKit" "-framework AssetsLibrary"
    "-framework Metal" "-framework MetalKit" "-framework QuartzCore"
    "-framework CoreGraphics" "-framework CoreVideo" "-framework CoreMedia"
    "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio"
    "-framework VideoToolbox" "-framework IOSurface" "-framework Security"
    "-framework Network" "-framework OpenGLES" "-framework CoreText"
    "-framework ImageIO" "-framework MobileCoreServices")

set(TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(MODULE_DIR ${TESTS_DIR}/..)
set(COMMON_DIR ${MODULE_DIR}/common)

# GST_IOS_PLUGIN_LIST_DEFINE, for the gst_ios_init.m each suite compiles.
include(${MODULE_DIR}/gst-plugins.cmake)

function(add_ios_suite name)
    set(target check_${name})

    add_executable(${target}
        ${TESTS_DIR}/common/check_${name}.c
        ${TESTS_DIR}/ios/platform.c
        ${MODULE_DIR}/ios/RCTGstPlayer/gst_ios_init.m
        ${ARGN})

    target_include_directories(${target} PRIVATE
        ${COMMON_DIR}/include
        ${MODULE_DIR}/ios/RCTGstPlayer
        ${TESTS_DIR})

    target_compile_definitions(${target} PRIVATE ${GST_IOS_PLUGIN_LIST_DEFINE})

    target_link_libraries(${target} PRIVATE GStreamer::static ${GST_SYSTEM_LIBS})
    target_compile_options(${target} PRIVATE -Wall -Wno-deprecated-declarations -fobjc-arc)
endfunction()

add_ios_suite(codec ${COMMON_DIR}/src/gstreamer_codec.c)

add_ios_suite(backend
    ${COMMON_DIR}/src/gstreamer_backend.c
    ${COMMON_DIR}/src/gstreamer_codec.c
    ${COMMON_DIR}/src/gstreamer_encoder.c
    ${COMMON_DIR}/src/gstreamer_event_recorder.c
    ${COMMON_DIR}/src/gstreamer_recorder.c
    ${COMMON_DIR}/src/gstreamer_video_writer.c)
