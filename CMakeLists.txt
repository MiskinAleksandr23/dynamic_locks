cmake_minimum_required(VERSION 3.16)
project(DynamicLockBenchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
include(CTest)

option(BUILD_GENETIC_BENCHMARK "Build the separate legacy genetic benchmark" OFF)
option(BUILD_BENCHMARK_V700 "Build vendored Benchmark-v700 framework" OFF)
set(SANITIZER "" CACHE STRING "Optional sanitizer: address,undefined or thread")

add_library(dynamic_locks INTERFACE)
target_include_directories(dynamic_locks INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dynamic_locks INTERFACE Threads::Threads)
if(SANITIZER)
  target_compile_options(dynamic_locks INTERFACE -fsanitize=${SANITIZER} -fno-omit-frame-pointer)
  target_link_options(dynamic_locks INTERFACE -fsanitize=${SANITIZER})
endif()

add_executable(adaptive_lock_benchmark benchmarks/adaptive_lock_benchmark.cpp common/spinlock.cpp)
target_link_libraries(adaptive_lock_benchmark PRIVATE dynamic_locks)

option(BUILD_LOCKTREE_BASELINE "Build optional RocksDB/PerconaFT LockTree baseline" OFF)
if(BUILD_LOCKTREE_BASELINE)
  add_subdirectory(third_party/rocksdb_locktree)
  target_link_libraries(adaptive_lock_benchmark PRIVATE locktree_baseline)
  target_compile_definitions(adaptive_lock_benchmark PRIVATE DYNAMIC_HAS_LOCKTREE=1)
endif()
add_executable(example example/main.cpp)
target_link_libraries(example PRIVATE dynamic_locks)
add_executable(table1_benchmark benchmarks/table1_benchmark.cpp common/spinlock.cpp)
target_link_libraries(table1_benchmark PRIVATE dynamic_locks)

if(BUILD_TESTING)
  add_executable(srl_array_lock_tests tests/srl_array_lock_test.cpp)
  target_link_libraries(srl_array_lock_tests PRIVATE dynamic_locks)
  add_test(NAME srl_array_lock_tests COMMAND srl_array_lock_tests)
  add_test(NAME benchmark_srl_smoke COMMAND adaptive_lock_benchmark
    --implementations srl4,srl64,srl256 --scenario steady_range
    --array-size 65537 --hot-width 65536 --range-length 1025 --write-body range
    --operations 1025 --threads 3 --trace-size 17 --repetitions 1 --read-percent 70)
  set_tests_properties(srl_array_lock_tests benchmark_srl_smoke PROPERTIES TIMEOUT 60)
  add_executable(multigranularity_lock_tests tests/multigranularity_lock_test.cpp)
  target_link_libraries(multigranularity_lock_tests PRIVATE dynamic_locks)
  add_test(NAME multigranularity_lock_tests COMMAND multigranularity_lock_tests)
  set_tests_properties(multigranularity_lock_tests PROPERTIES TIMEOUT 60)
  add_test(NAME benchmark_mgl_smoke COMMAND adaptive_lock_benchmark
    --implementations mgl --array-size 4096 --hot-width 256 --range-length 8
    --operations 8191 --threads 3 --trace-size 17 --repetitions 1 --read-percent 70)
  set_tests_properties(benchmark_mgl_smoke PROPERTIES TIMEOUT 60)
  add_executable(paper_partitioner_tests tests/paper_partitioner_test.cpp)
  target_link_libraries(paper_partitioner_tests PRIVATE dynamic_locks)
  add_test(NAME paper_partitioner_tests COMMAND paper_partitioner_tests)
  add_executable(paper_lock_tests tests/paper_lock_test.cpp)
  target_link_libraries(paper_lock_tests PRIVATE dynamic_locks)
  add_test(NAME paper_lock_tests COMMAND paper_lock_tests)
  set_tests_properties(paper_partitioner_tests paper_lock_tests PROPERTIES TIMEOUT 60)
  add_executable(read_write_lock_tests tests/read_write_lock_test.cpp common/spinlock.cpp)
  target_link_libraries(read_write_lock_tests PRIVATE dynamic_locks)
  add_test(NAME read_write_lock_tests COMMAND read_write_lock_tests)
  set_tests_properties(read_write_lock_tests PROPERTIES TIMEOUT 30)
  add_executable(dynamic_lock_tests tests/dynamic_lock_test.cpp)
  target_link_libraries(dynamic_lock_tests PRIVATE dynamic_locks)
  add_executable(partitioner_tests tests/partitioner_test.cpp)
  target_link_libraries(partitioner_tests PRIVATE dynamic_locks)
  add_executable(genetic_partitioner_tests tests/genetic_partitioner_test.cpp)
  target_link_libraries(genetic_partitioner_tests PRIVATE dynamic_locks)
  add_executable(benchmark_workload_tests tests/benchmark_workload_test.cpp common/spinlock.cpp)
  target_link_libraries(benchmark_workload_tests PRIVATE dynamic_locks)
  add_test(NAME dynamic_lock_tests COMMAND dynamic_lock_tests)
  add_test(NAME partitioner_tests COMMAND partitioner_tests)
  add_test(NAME genetic_partitioner_tests COMMAND genetic_partitioner_tests)
  add_test(NAME benchmark_workload_tests COMMAND benchmark_workload_tests)
  add_test(NAME example COMMAND example)
  add_test(NAME benchmark_smoke COMMAND adaptive_lock_benchmark
    --operations 8192 --threads 4 --repetitions 1)
  add_test(NAME benchmark_cyclic_smoke COMMAND adaptive_lock_benchmark
    --operations 8191 --threads 3 --trace-size 17 --repetitions 1)
  add_test(NAME benchmark_baselines_smoke COMMAND adaptive_lock_benchmark
    --implementations global,per_element --array-size 4096 --hot-width 256
    --range-length 8 --operations 8191 --threads 3 --trace-size 17 --repetitions 1)
  add_test(NAME benchmark_genetic_smoke COMMAND adaptive_lock_benchmark
    --model legacy --write-body single
    --implementations fixed,balanced,dynamic_dp,genetic
    --operations 65537 --threads 3 --trace-size 257 --repetitions 1 --rebuild-ms 1)
  set_tests_properties(dynamic_lock_tests partitioner_tests benchmark_workload_tests example benchmark_smoke benchmark_cyclic_smoke
    benchmark_baselines_smoke genetic_partitioner_tests benchmark_genetic_smoke PROPERTIES TIMEOUT 90)
endif()

if(BUILD_GENETIC_BENCHMARK)
  add_executable(genetic_lock_benchmark benchmarks/genetic_lock_benchmark.cpp common/spinlock.cpp)
  target_link_libraries(genetic_lock_benchmark PRIVATE dynamic_locks)
endif()
if(BUILD_BENCHMARK_V700)
  add_subdirectory(Benchmark-v700/cpp Benchmark-v700-build EXCLUDE_FROM_ALL)
endif()
