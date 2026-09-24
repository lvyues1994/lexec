cmake_minimum_required(VERSION 3.21...3.31)

project(lexec VERSION 0.1.0 LANGUAGES CXX)

add_library(lexec INTERFACE)
add_library(lexec::lexec ALIAS lexec)
target_include_directories(lexec INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lexec INTERFACE cxx_std_17)

option(LEXEC_BUILD_TESTS "Build lexec's own tests" ${PROJECT_IS_TOP_LEVEL})

if(LEXEC_BUILD_TESTS)
    include(cmake/lexec_dev.cmake)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(bench)
endif()
