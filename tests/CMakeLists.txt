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

add_executable(wolvicmod_tests test_main.cpp ${WOLVICMOD_TEST_SOURCES})
target_link_libraries(wolvicmod_tests PRIVATE wolvicmod wolvicmod_test_flags)

add_test(NAME wolvicmod_tests COMMAND wolvicmod_tests)
