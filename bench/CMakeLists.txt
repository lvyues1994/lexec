# Built with every configuration so the compile-time probes keep compiling;
# compile_time/measure.py times them separately.
add_library(lexec_compile_time_probes OBJECT
    compile_time/include_only.cpp
    compile_time/deep_pipeline.cpp
    compile_time/let_pipeline.cpp)
target_link_libraries(lexec_compile_time_probes PRIVATE lexec::lexec lexec_dev)

if(LEXEC_BUILD_BENCHMARKS)
    include(FetchContent)
    # Header-only use: stdexec's own CMake project is not configured.
    FetchContent_Declare(stdexec
        GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
        GIT_TAG ead186b1d8db3ebe37a946ff84a6ce08bf795153
        SOURCE_SUBDIR no-cmake-project)
    FetchContent_MakeAvailable(stdexec)

    add_executable(lexec_pool_bench pool/lexec_pool_bench.cpp)
    target_link_libraries(lexec_pool_bench PRIVATE lexec::runtime lexec_dev)

    add_executable(stdexec_pool_bench pool/stdexec_pool_bench.cpp)
    set_target_properties(stdexec_pool_bench PROPERTIES CXX_STANDARD 20)
    target_include_directories(stdexec_pool_bench SYSTEM PRIVATE "${stdexec_SOURCE_DIR}/include")
    target_link_libraries(stdexec_pool_bench PRIVATE Threads::Threads)
endif()
