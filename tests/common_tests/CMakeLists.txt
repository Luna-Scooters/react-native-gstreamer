add_library(test_main STATIC gst_test.cpp)
target_include_directories(test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_main PUBLIC PkgConfig::GST GTest::gtest rngstcommon)
target_compile_options(test_main PRIVATE ${WARNINGS})
if(APPLE)
    # waitUntil() services the main queue the way UIKit's run loop does.
    target_link_libraries(test_main PUBLIC "-framework CoreFoundation")
endif()

add_gst_suite(codec)
add_gst_suite(backend)
