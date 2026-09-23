# Platform-independent GStreamer code shared by Android, iOS and the test suites.
#
#   android/CMakeLists.txt   links GStreamer::mobile
#   tests/                   compile only the sources each suite exercises

cmake_minimum_required(VERSION 3.22)
project(rngstcommon C)

file(GLOB RN_GST_COMMON_SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/*.c)

add_library(rngstcommon STATIC ${RN_GST_COMMON_SOURCES})

target_include_directories(rngstcommon PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)

target_compile_options(rngstcommon PRIVATE -Wall -Wno-deprecated-declarations)

if(APPLE)
    # cb_error() dispatches a block to the main queue on Apple platforms.
    target_compile_options(rngstcommon PRIVATE -fblocks)
endif()
