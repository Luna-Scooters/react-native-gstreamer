# Android build for react-native-gstreamer: the JNI library plus everything
# GStreamer-related. Gradle configures this file (see RCTGstPlayer/build.gradle).

cmake_minimum_required(VERSION 3.22)

# CXX is required even though no C++ is compiled here: the SDK's
# GStreamerMobile target sets LINKER_LANGUAGE CXX for gstsoundtouch.
project(rctgstplayer C CXX)

set(MODULE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

if(NOT GSTREAMER_ROOT_ANDROID)
    message(FATAL_ERROR
        "GSTREAMER_ROOT_ANDROID must point at the unpacked universal "
        "GStreamer Android binaries")
endif()

set(GST_ABI_DIRS
    armeabi        arm
    armeabi-v7a    armv7
    arm64-v8a      arm64
    x86            x86
    x86_64         x86_64)
list(FIND GST_ABI_DIRS ${ANDROID_ABI} _abi_index)
if(_abi_index EQUAL -1)
    message(FATAL_ERROR "Target arch ABI not supported: ${ANDROID_ABI}")
endif()
math(EXPR _abi_index "${_abi_index} + 1")
list(GET GST_ABI_DIRS ${_abi_index} GST_ARCH)

set(GStreamer_ROOT_DIR "${GSTREAMER_ROOT_ANDROID}/${GST_ARCH}")
list(APPEND CMAKE_MODULE_PATH "${GStreamer_ROOT_DIR}/share/cmake")

include("${GStreamer_ROOT_DIR}/share/gst-android/ndk-build/plugins.cmake")

# The shared plugins list
include(${MODULE_ROOT}/gst-plugins.cmake)

set(GStreamer_EXTRA_DEPS glib-2.0)

find_package(GStreamerMobile REQUIRED COMPONENTS
    fonts
    ca_certificates
    api_player
    api_video
    ${GSTREAMER_PLUGINS})

# The shared platform-independent code, with GStreamer supplied from here.
add_subdirectory(${MODULE_ROOT} common)
target_link_libraries(rngstcommon PUBLIC GStreamer::mobile)

# GStreamer's static libs carry full DWARF; keeping it makes libgstreamer_android.so
# ~800MB per ABI, and AGP strips it before packaging anyway. Release keeps it so
# debugSymbolLevel FULL still has symbols to extract.
option(GST_STRIP_DEBUG "Link GStreamer without DWARF in Debug builds" ON)
if(GST_STRIP_DEBUG)
    target_link_options(GStreamerMobile PRIVATE $<$<CONFIG:Debug>:-Wl,--strip-debug>)
endif()

add_library(rctgstplayer SHARED
    ${CMAKE_CURRENT_LIST_DIR}/RCTGstPlayer/src/main/jni/rctgstplayer.c)

target_link_libraries(rctgstplayer PRIVATE
    rngstcommon
    GStreamer::mobile
    log
    android)

# Keep the app loadable on 16KB page-size devices.
target_link_options(rctgstplayer PRIVATE
    -Wl,-z,max-page-size=16384
    -Wl,-z,common-page-size=16384)
