# Build policy for lexec's own tests and benchmarks. Consumers of lexec::lexec never see it.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|MSVC)$")
    message(FATAL_ERROR "lexec development builds support GCC, Clang, and MSVC only")
endif()

set(LEXEC_CXX_STANDARD 17 CACHE STRING "C++ standard for lexec's own builds")
set_property(CACHE LEXEC_CXX_STANDARD PROPERTY STRINGS 17 20 23)
set(LEXEC_SANITIZERS "" CACHE STRING "Value passed to -fsanitize= for lexec's own builds")
set(LEXEC_TEST_LAUNCHER "" CACHE STRING "Command prefix, as a CMake list, for running lexec's tests")
option(LEXEC_DISABLE_EXCEPTIONS "Build lexec's own targets with -fno-exceptions" OFF)
option(LEXEC_WARNINGS_AS_ERRORS "Treat warnings as errors in lexec's own targets" ON)
option(LEXEC_BUILD_BENCHMARKS "Build the benchmarks comparing lexec with stdexec, which fetches stdexec" OFF)
option(LEXEC_WITH_CO2 "Build the tests of the co2 coroutine bridge, which fetches co2" ON)
set(LEXEC_COMPILE_MEMORY_LIMIT 4294967296 CACHE STRING
    "Address-space limit in bytes for each compiler process of lexec's own builds; 0 disables it")

# A template change whose instantiations regress to super-linear growth then fails the
# compilation instead of exhausting the machine's memory.
if(LEXEC_COMPILE_MEMORY_LIMIT)
    find_program(LEXEC_PRLIMIT prlimit)
    if(LEXEC_PRLIMIT)
        list(PREPEND CMAKE_CXX_COMPILER_LAUNCHER "${LEXEC_PRLIMIT}" "--as=${LEXEC_COMPILE_MEMORY_LIMIT}")
    endif()
endif()

set(CMAKE_CXX_STANDARD ${LEXEC_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lexec_dev INTERFACE)

if(MSVC)
    # C4324: padding added for alignas, which the concurrent structures ask for on purpose.
    # C4702: code the optimizer finds unreachable after inlining a throwing or terminating
    # call into one instantiation of a template.
    target_compile_options(lexec_dev INTERFACE
        /W4 /permissive- /Zc:__cplusplus /utf-8 /wd4324 /wd4702
        $<$<BOOL:${LEXEC_WARNINGS_AS_ERRORS}>:/WX>)
    if(LEXEC_DISABLE_EXCEPTIONS)
        string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        target_compile_options(lexec_dev INTERFACE /EHs-c-)
        target_compile_definitions(lexec_dev INTERFACE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(lexec_dev INTERFACE /EHsc)
    endif()
    if(LEXEC_SANITIZERS)
        target_compile_options(lexec_dev INTERFACE /fsanitize=${LEXEC_SANITIZERS})
    endif()
else()
    target_compile_options(lexec_dev INTERFACE
        -Wall -Wextra -Wpedantic
        -Wconversion -Wsign-conversion -Wshadow
        -Wold-style-cast -Wnon-virtual-dtor -Wzero-as-null-pointer-constant
        $<$<BOOL:${LEXEC_WARNINGS_AS_ERRORS}>:-Werror>)
    if(LEXEC_DISABLE_EXCEPTIONS)
        target_compile_options(lexec_dev INTERFACE -fno-exceptions)
    endif()
    if(LEXEC_SANITIZERS)
        target_compile_options(lexec_dev INTERFACE
            -fsanitize=${LEXEC_SANITIZERS} -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(lexec_dev INTERFACE -fsanitize=${LEXEC_SANITIZERS})
    endif()
endif()

target_compile_definitions(lexec_dev INTERFACE
    LEXEC_TEST_EXCEPTIONS_ENABLED=$<IF:$<BOOL:${LEXEC_DISABLE_EXCEPTIONS}>,0,1>)
