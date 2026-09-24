include(FetchContent)

FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
    URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
# lexec compiles doctest's main itself so it shares the exception mode of the tests.
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
set(DOCTEST_NO_INSTALL ON)
FetchContent_MakeAvailable(doctest)

file(GLOB_RECURSE lexec_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(lexec_tests ${lexec_test_sources})
target_link_libraries(lexec_tests PRIVATE lexec::lexec lexec_dev doctest::doctest)
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
