cmake_minimum_required(VERSION 3.14)
project(DynamicLockBenchmark)
include_directories(Benchmark-v700)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(benchmark main.cpp
        spinlock.cpp
        genetic_lock.hpp
        genetic_partitioner.hpp
        request_workload.hpp)

target_compile_options(benchmark PRIVATE -O3 -pthread)
target_link_libraries(benchmark pthread)


add_executable(benchmark_genetic gen_main.cpp
        spinlock.cpp
        genetic_lock.hpp
        genetic_partitioner.hpp
        request_workload.hpp)

target_compile_options(benchmark_genetic PRIVATE -O3 -pthread)
target_link_libraries(benchmark_genetic pthread)
