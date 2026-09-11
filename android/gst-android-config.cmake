# GStreamer build configuration shared by the app's JNI library (android/) and
# the GstCheck suites (tests/android/). Consumers include this, then call
# find_package(GStreamerMobile) with the components they need -- adjusting
# GSTREAMER_PLUGINS or GStreamer_EXTRA_DEPS first if they have to.

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
include(${CMAKE_CURRENT_LIST_DIR}/../gst-plugins.cmake)

set(GStreamer_EXTRA_DEPS glib-2.0)
