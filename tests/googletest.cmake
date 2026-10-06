include(FetchContent)

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

function(add_gst_suite name)
    cmake_parse_arguments(SUITE "" "LINK" "" ${ARGN})
    if(NOT SUITE_LINK)
        set(SUITE_LINK test_main)
    endif()
    set(target ${name}_tests)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/${name}_test.cpp)

    add_executable(${target} ${source} ${SUITE_UNPARSED_ARGUMENTS})
    target_link_libraries(${target} PRIVATE ${SUITE_LINK})
    target_compile_options(${target} PRIVATE ${WARNINGS})
    if(APPLE)
        # cb_error() dispatches a block to the main queue on Apple platforms.
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:-fblocks>)
    endif()

    add_test(NAME ${name} COMMAND ${target})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${SUITE_TIMEOUT})
endfunction()
