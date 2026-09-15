# Platform-independent GStreamer code shared by Android, iOS and the test suites.
#
#   android/CMakeLists.txt            links GStreamer::mobile
#   the project generated for iOS     links GStreamer.xcframework

cmake_minimum_required(VERSION 3.22)
project(rngstcommon C)

set(RN_GST_COMMON_DIR ${CMAKE_CURRENT_LIST_DIR}/common)
file(GLOB RN_GST_COMMON_SOURCES ${RN_GST_COMMON_DIR}/src/*.c)

add_library(rngstcommon STATIC ${RN_GST_COMMON_SOURCES})

target_include_directories(rngstcommon PUBLIC ${RN_GST_COMMON_DIR}/include)

target_compile_options(rngstcommon PRIVATE -Wall -Wno-deprecated-declarations)

if(APPLE)
    # cb_error() dispatches a block to the main queue on Apple platforms.
    target_compile_options(rngstcommon PRIVATE -fblocks)
endif()
