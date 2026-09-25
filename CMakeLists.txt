cmake_minimum_required(VERSION 3.21...3.31)

project(lexec VERSION 0.1.0 LANGUAGES CXX)

add_library(lexec INTERFACE)
add_library(lexec::lexec ALIAS lexec)
target_include_directories(lexec INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lexec INTERFACE cxx_std_17)

# lexec takes the execution policies from <execution>. When TBB's headers are installed,
# libstdc++ implements that header on TBB, and programs including it then need TBB at
# link time, unoptimized ones at least.
function(lexec_check_execution_uses_tbb)
    if(NOT CMAKE_CXX_STANDARD)
        set(CMAKE_CXX_STANDARD 17)
    endif()
    # Compiled only: linking the probe is what fails without TBB.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles([[
        #include <execution>
        #ifndef _PSTL_PAR_BACKEND_TBB
        #error "the standard library does not implement <execution> on TBB"
        #endif
        int main() {}
    ]] LEXEC_EXECUTION_USES_TBB)
endfunction()
lexec_check_execution_uses_tbb()
if(LEXEC_EXECUTION_USES_TBB)
    find_package(TBB CONFIG)
    if(TBB_FOUND)
        target_link_libraries(lexec INTERFACE TBB::tbb)
    else()
        message(WARNING "The standard library implements <execution> on TBB, but CMake found no TBB package; "
                        "programs using lexec must link TBB themselves")
    endif()
endif()

option(LEXEC_BUILD_TESTS "Build lexec's own tests" ${PROJECT_IS_TOP_LEVEL})

if(LEXEC_BUILD_TESTS)
    include(cmake/lexec_dev.cmake)
endif()

# The compiled part of lexec: execution resources such as static_thread_pool.
find_package(Threads REQUIRED)
add_library(lexec_runtime STATIC src/static_thread_pool.cpp)
add_library(lexec::runtime ALIAS lexec_runtime)
target_link_libraries(lexec_runtime PUBLIC lexec::lexec Threads::Threads)
target_include_directories(lexec_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(TARGET lexec_dev)
    target_link_libraries(lexec_runtime PRIVATE lexec_dev)
endif()

if(LEXEC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(bench)
endif()
