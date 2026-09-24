# Built with every configuration so the compile-time probes keep compiling;
# compile_time/measure.py times them separately.
add_library(lexec_compile_time_probes OBJECT
    compile_time/include_only.cpp
    compile_time/deep_pipeline.cpp
    compile_time/let_pipeline.cpp)
target_link_libraries(lexec_compile_time_probes PRIVATE lexec::lexec lexec_dev)
