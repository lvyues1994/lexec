include(FetchContent)

FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
    URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
# lexec compiles doctest's main itself so it shares the exception mode of the tests.
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
set(DOCTEST_NO_INSTALL ON)
FetchContent_MakeAvailable(doctest)

file(GLOB_RECURSE lexec_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER lexec_test_sources EXCLUDE REGEX "/asm/")
if(LEXEC_SANITIZERS)
    # Sanitizer runtimes define the global allocation functions the counter replaces.
    list(FILTER lexec_test_sources EXCLUDE REGEX "/allocation_(counter|test)\\.cpp$")
endif()
add_executable(lexec_tests ${lexec_test_sources})
target_link_libraries(lexec_tests PRIVATE lexec::lexec lexec::runtime lexec_dev doctest::doctest)
# The runtime's private headers, such as its work-stealing queue, are tested directly.
target_include_directories(lexec_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
# Defining this with exceptions enabled would switch doctest's exception assertions off.
target_compile_definitions(lexec_tests PRIVATE
    $<$<BOOL:${LEXEC_DISABLE_EXCEPTIONS}>:DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS>)
add_test(NAME lexec_tests COMMAND ${LEXEC_TEST_LAUNCHER} $<TARGET_FILE:lexec_tests>)

file(GLOB_RECURSE lexec_public_headers CONFIGURE_DEPENDS
    RELATIVE "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/lexec/*.hpp")
set(lexec_header_check_sources "")
foreach(header IN LISTS lexec_public_headers)
    string(MAKE_C_IDENTIFIER "${header}" header_id)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_check/${header_id}.cpp")
    file(CONFIGURE OUTPUT "${source}" CONTENT "#include <${header}>\n#include <${header}>\n")
    list(APPEND lexec_header_check_sources "${source}")
endforeach()
add_library(lexec_header_check OBJECT ${lexec_header_check_sources})
target_link_libraries(lexec_header_check PRIVATE lexec::lexec lexec_dev)

# Optimized regardless of build type and free of sanitizer instrumentation, so the
# disassembly reflects what users get.
add_library(lexec_zero_overhead_probe OBJECT asm/zero_overhead_probe.cpp)
target_link_libraries(lexec_zero_overhead_probe PRIVATE lexec::lexec)
target_compile_options(lexec_zero_overhead_probe PRIVATE -O2)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND CMAKE_OBJDUMP)
    add_test(NAME lexec_zero_overhead_codegen
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/asm/compare_functions.py" "${CMAKE_OBJDUMP}"
            $<TARGET_OBJECTS:lexec_zero_overhead_probe> lexec_probe_direct lexec_probe_framework lexec_probe_reference)
endif()
