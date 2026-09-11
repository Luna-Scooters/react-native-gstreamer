# Android build for react-native-gstreamer: the JNI library plus everything
# GStreamer-related. Gradle configures this file (see RCTGstPlayer/build.gradle).

cmake_minimum_required(VERSION 3.22)

# CXX is required even though no C++ is compiled here: the SDK's
# GStreamerMobile target sets LINKER_LANGUAGE CXX for gstsoundtouch.
project(rctgstplayer C CXX)

set(MODULE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# ABI mapping, plugin set and extra deps, shared with tests/android.
include(${CMAKE_CURRENT_LIST_DIR}/gst-android-config.cmake)

find_package(GStreamerMobile REQUIRED COMPONENTS
    fonts
    ca_certificates
    api_player
    api_video
    ${GSTREAMER_PLUGINS})

# The shared platform-independent code, with GStreamer supplied from here.
add_subdirectory(${MODULE_ROOT}/common common)
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
