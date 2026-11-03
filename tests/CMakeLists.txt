cmake_minimum_required(VERSION 3.21)
project(pspcap_host_tests LANGUAGES C CXX)
enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Assertions are the test checks, including in Release builds.
string(REPLACE "/DNDEBUG" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
string(REPLACE "/DNDEBUG" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

# PSP function addresses are u32. Use a 32-bit host target so mocked function
# pointers have the same representation (Visual Studio: -A Win32).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure the PSP mock tests for a 32-bit target")
endif()

add_executable(capture_test capture_test.c
    ../psp/src/display.c ../psp/src/audio.c ../psp/src/capture_hooks.c)
target_include_directories(capture_test PRIVATE mocks ../psp/src ../common)
if(MSVC)
    target_compile_options(capture_test PRIVATE /W4 /utf-8 /wd4152)
    # Distinct firmware exports must keep distinct addresses even when their
    # mock implementations compile to identical machine code.
    target_link_options(capture_test PRIVATE /OPT:NOICF)
else()
    target_compile_options(capture_test PRIVATE -Wall -Wextra -UNDEBUG)
endif()
foreach(scenario polling pacing resume race disabled invalid_getter partial_audio
        audio_retry audio_overflow audio_ring audio_unknown no_audio late_imports)
    add_test(NAME ${scenario} COMMAND capture_test ${scenario})
endforeach()

add_executable(pixel_format_test ../pc/tests/pixel_format_test.cpp ../pc/src/pixel_format.cpp)
target_compile_features(pixel_format_test PRIVATE cxx_std_17)
target_include_directories(pixel_format_test PRIVATE ../common ../pc/src)
if(MSVC)
    target_compile_options(pixel_format_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME pixel_format COMMAND pixel_format_test)
