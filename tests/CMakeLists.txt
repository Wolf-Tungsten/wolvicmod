add_library(wolvicmod_test_flags INTERFACE)
target_include_directories(wolvicmod_test_flags INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
# Audit instrumentation is compiled in for the test suite; the runtime
# switches (auditOn etc.) still default to off (§6.4).
target_compile_definitions(wolvicmod_test_flags INTERFACE WOLVICMOD_AUDIT=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wolvicmod_test_flags INTERFACE -Wall -Wextra)
endif()

file(GLOB WOLVICMOD_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
list(REMOVE_ITEM WOLVICMOD_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp)

# 每个 test 源文件一个可执行 + 独立 ctest 条目（ctest -R <模块> 可单跑）；
# test_main.cpp 是 doctest main，逐文件复用链接。
foreach(src ${WOLVICMOD_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} test_main.cpp ${src})
    target_link_libraries(${name} PRIVATE wolvicmod wolvicmod_test_flags)
    add_test(NAME ${name} COMMAND ${name})
endforeach()
