# Vendored subset of gtkwave/libfst — see README.wolvicmod for provenance.
add_library(wolvicmod_fst STATIC
    src/fstapi.c
    src/fastlz.c
    src/lz4.c)
target_include_directories(wolvicmod_fst PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(wolvicmod_fst PRIVATE _GNU_SOURCE)
set_target_properties(wolvicmod_fst PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(ZLIB REQUIRED)
target_link_libraries(wolvicmod_fst PUBLIC ZLIB::ZLIB)

# Third-party code: silence compiler warnings.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wolvicmod_fst PRIVATE -w)
endif()
