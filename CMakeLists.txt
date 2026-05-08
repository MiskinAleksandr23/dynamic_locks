cmake_minimum_required(VERSION 3.14)
project(DynamicLockBenchmark)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_BENCHMARK_V700 "Build vendored Benchmark-v700 framework" OFF)

add_executable(adaptive_lock_benchmark
        benchmarks/adaptive_lock_benchmark.cpp
        common/spinlock.cpp)
target_include_directories(adaptive_lock_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(adaptive_lock_benchmark PRIVATE cxx_std_23)
target_compile_options(adaptive_lock_benchmark PRIVATE -O3 -pthread)
target_link_libraries(adaptive_lock_benchmark PRIVATE pthread)

add_executable(genetic_lock_benchmark
        benchmarks/genetic_lock_benchmark.cpp
        common/spinlock.cpp)
target_include_directories(genetic_lock_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(genetic_lock_benchmark PRIVATE cxx_std_23)
target_compile_options(genetic_lock_benchmark PRIVATE -O3 -pthread)
target_link_libraries(genetic_lock_benchmark PRIVATE pthread)

if (BUILD_BENCHMARK_V700)
        add_subdirectory(Benchmark-v700/cpp Benchmark-v700-build EXCLUDE_FROM_ALL)
endif ()
