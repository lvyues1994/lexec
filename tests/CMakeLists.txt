include(FetchContent)

FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
    URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
# lexec compiles doctest's main itself so it shares the exception mode of the tests.
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
set(DOCTEST_NO_INSTALL ON)
FetchContent_MakeAvailable(doctest)

file(GLOB_RECURSE lexec_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER lexec_test_sources EXCLUDE REGEX "/(asm|coro|replacement)/")
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

# A program of its own, since it replaces a function that a program defines once.
add_executable(lexec_replacement_tests replacement/parallel_scheduler_replacement_test.cpp test_main.cpp)
target_link_libraries(lexec_replacement_tests PRIVATE lexec::lexec lexec::runtime lexec_dev doctest::doctest)
target_compile_definitions(lexec_replacement_tests PRIVATE
    $<$<BOOL:${LEXEC_DISABLE_EXCEPTIONS}>:DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS>)
add_test(NAME lexec_replacement_tests COMMAND ${LEXEC_TEST_LAUNCHER} $<TARGET_FILE:lexec_replacement_tests>)

function(lexec_add_header_check target)
    set(sources "")
    foreach(header IN LISTS ARGN)
        string(MAKE_C_IDENTIFIER "${header}" header_id)
        set(source "${CMAKE_CURRENT_BINARY_DIR}/header_check/${header_id}.cpp")
        file(CONFIGURE OUTPUT "${source}" CONTENT "#include <${header}>\n#include <${header}>\n")
        list(APPEND sources "${source}")
    endforeach()
    add_library(${target} OBJECT ${sources})
endfunction()

file(GLOB_RECURSE lexec_public_headers CONFIGURE_DEPENDS
    RELATIVE "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/lexec/*.hpp")
# The bridges need the libraries they bridge to, and are checked with them below.
set(lexec_bridge_headers ${lexec_public_headers})
list(FILTER lexec_public_headers EXCLUDE REGEX "^lexec/coro/")
list(FILTER lexec_bridge_headers INCLUDE REGEX "^lexec/coro/")
lexec_add_header_check(lexec_header_check ${lexec_public_headers})
target_link_libraries(lexec_header_check PRIVATE lexec::lexec lexec_dev)

# The co2 bridge, whose tests fetch co2; co2 needs exceptions, and its headers are
# included as system headers so that lexec's warnings do not apply to them.
if(LEXEC_WITH_CO2 AND NOT LEXEC_DISABLE_EXCEPTIONS)
    if(CMAKE_VERSION VERSION_LESS 3.25)
        message(WARNING "The co2 bridge's tests need CMake 3.25 or newer; skipping them")
    else()
        FetchContent_Declare(co2
            GIT_REPOSITORY https://github.com/lvyues1994/coro.git
            GIT_TAG a265e5771b0e3ca68d6d473b9bb410fadddf4c23
            SYSTEM)
        FetchContent_MakeAvailable(co2)

        file(GLOB lexec_coro_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/coro/*.cpp")
        if(LEXEC_SANITIZERS)
            list(FILTER lexec_coro_test_sources EXCLUDE REGEX "/allocation_test\\.cpp$")
        else()
            list(APPEND lexec_coro_test_sources support/allocation_counter.cpp)
        endif()
        add_executable(lexec_coro_tests ${lexec_coro_test_sources} test_main.cpp)
        target_link_libraries(lexec_coro_tests PRIVATE lexec::lexec lexec::runtime lexec_dev co2::co2 doctest::doctest)
        # co2 turns on MSVC's conforming preprocessor, under which older Windows SDK
        # headers warn (C5105). co2's ThreadPool parks workers with a fence-based protocol,
        # which GCC warns ThreadSanitizer cannot follow.
        target_compile_options(lexec_coro_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/wd5105>)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND LEXEC_SANITIZERS MATCHES "thread")
            target_compile_options(lexec_coro_tests PRIVATE -Wno-tsan)
        endif()
        add_test(NAME lexec_coro_tests COMMAND ${LEXEC_TEST_LAUNCHER} $<TARGET_FILE:lexec_coro_tests>)

        lexec_add_header_check(lexec_bridge_header_check ${lexec_bridge_headers})
        target_link_libraries(lexec_bridge_header_check PRIVATE lexec::lexec lexec_dev co2::co2)
        target_compile_options(lexec_bridge_header_check PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/wd5105>)
    endif()
endif()

# Optimized regardless of build type and free of sanitizer instrumentation, so the
# disassembly reflects what users get. The comparison reads objdump output, and MSVC's
# debug runtime checks reject /O2, so MSVC skips it.
if(NOT MSVC)
    add_library(lexec_zero_overhead_probe OBJECT asm/zero_overhead_probe.cpp)
    target_link_libraries(lexec_zero_overhead_probe PRIVATE lexec::lexec)
    target_compile_options(lexec_zero_overhead_probe PRIVATE -O2)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND AND CMAKE_OBJDUMP)
        add_test(NAME lexec_zero_overhead_codegen
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/asm/compare_functions.py" "${CMAKE_OBJDUMP}"
                $<TARGET_OBJECTS:lexec_zero_overhead_probe> lexec_probe_direct lexec_probe_framework lexec_probe_reference)
    endif()
endif()
