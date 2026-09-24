# Build policy for lexec's own tests and benchmarks. Consumers of lexec::lexec never see it.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR "lexec development builds currently support GCC and Clang only")
endif()

set(LEXEC_CXX_STANDARD 17 CACHE STRING "C++ standard for lexec's own builds")
set_property(CACHE LEXEC_CXX_STANDARD PROPERTY STRINGS 17 20 23)
set(LEXEC_SANITIZERS "" CACHE STRING "Value passed to -fsanitize= for lexec's own builds")
set(LEXEC_TEST_LAUNCHER "" CACHE STRING "Command prefix, as a CMake list, for running lexec's tests")
option(LEXEC_DISABLE_EXCEPTIONS "Build lexec's own targets with -fno-exceptions" OFF)
option(LEXEC_WARNINGS_AS_ERRORS "Treat warnings as errors in lexec's own targets" ON)
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

target_compile_options(lexec_dev INTERFACE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion -Wshadow
    -Wold-style-cast -Wnon-virtual-dtor -Wzero-as-null-pointer-constant
    $<$<BOOL:${LEXEC_WARNINGS_AS_ERRORS}>:-Werror>)

if(LEXEC_DISABLE_EXCEPTIONS)
    target_compile_options(lexec_dev INTERFACE -fno-exceptions)
endif()
target_compile_definitions(lexec_dev INTERFACE
    LEXEC_TEST_EXCEPTIONS_ENABLED=$<IF:$<BOOL:${LEXEC_DISABLE_EXCEPTIONS}>,0,1>)

if(LEXEC_SANITIZERS)
    target_compile_options(lexec_dev INTERFACE
        -fsanitize=${LEXEC_SANITIZERS} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(lexec_dev INTERFACE -fsanitize=${LEXEC_SANITIZERS})
endif()
