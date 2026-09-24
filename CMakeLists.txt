cmake_minimum_required(VERSION 3.20)
project(wolvicmod CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # editor IntelliSense (clangd / cpptools)
endif()

add_library(wolvicmod INTERFACE)
add_library(wolvicmod::wolvicmod ALIAS wolvicmod)
target_include_directories(wolvicmod INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(wolvicmod INTERFACE cxx_std_20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(WOLVICMOD_BUILD_TESTS_DEFAULT ON)
else()
    set(WOLVICMOD_BUILD_TESTS_DEFAULT OFF)
endif()
option(WOLVICMOD_BUILD_TESTS "Build wolvicmod tests" ${WOLVICMOD_BUILD_TESTS_DEFAULT})

if(WOLVICMOD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(WOLVICMOD_WITH_FST "FST waveform support via vendored libfst" ON)
if(WOLVICMOD_WITH_FST)
    enable_language(C)
    add_subdirectory(third_party/libfst)
    target_link_libraries(wolvicmod INTERFACE wolvicmod_fst)
    target_compile_definitions(wolvicmod INTERFACE WOLVICMOD_WITH_FST=1)
endif()

option(WOLVICMOD_AUDIT "Compile in read-set audit instrumentation (§6.4)" OFF)
if(WOLVICMOD_AUDIT)
    target_compile_definitions(wolvicmod INTERFACE WOLVICMOD_AUDIT=1)
endif()
