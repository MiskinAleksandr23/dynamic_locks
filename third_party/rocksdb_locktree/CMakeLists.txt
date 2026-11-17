cmake_minimum_required(VERSION 3.16)

# This directory can be added to an opt-in parent target or configured alone.
# No download, test execution, or benchmark is performed by configuration.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(LockTreeStandalone LANGUAGES CXX)
endif()

if(NOT UNIX)
  message(FATAL_ERROR "The prepared LockTree standalone port currently targets POSIX macOS/Linux")
endif()
find_package(Threads REQUIRED)

add_library(locktree_standalone STATIC
  upstream/locktree/concurrent_tree.cc
  upstream/locktree/keyrange.cc
  upstream/locktree/lock_request.cc
  upstream/locktree/locktree.cc
  upstream/locktree/manager.cc
  upstream/locktree/range_buffer.cc
  upstream/locktree/treenode.cc
  upstream/locktree/txnid_set.cc
  upstream/locktree/wfg.cc
  upstream/util/dbt.cc
  upstream/util/memarena.cc
  upstream/standalone_port.cc
  compat/locktree_port.cpp
)
target_compile_features(locktree_standalone PUBLIC cxx_std_20)
target_include_directories(locktree_standalone PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/compat/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
)
target_compile_definitions(locktree_standalone PUBLIC ROCKSDB_NAMESPACE=locktree_rocksdb_compat)
target_link_libraries(locktree_standalone PUBLIC Threads::Threads)

# Do not patch vendored headers to replace transitive includes from full RocksDB.
# These requirements reach the private adapter TU, whose native types never
# appear in its public header.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(locktree_standalone PUBLIC
    "-include" "${CMAKE_CURRENT_SOURCE_DIR}/compat/include/locktree_port_prefix.hpp")
else()
  message(FATAL_ERROR "Prepared standalone build requires a GCC/Clang-compatible compiler")
endif()

# These are the same ASan/UBSan flags used by the parent repository, when opted
# in. Sanitizers are never enabled or run implicitly.
if(SANITIZER)
  target_compile_options(locktree_standalone PUBLIC -fsanitize=${SANITIZER} -fno-omit-frame-pointer)
  target_link_options(locktree_standalone PUBLIC -fsanitize=${SANITIZER})
endif()

get_filename_component(LOCKTREE_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_library(locktree_baseline STATIC "${LOCKTREE_PROJECT_ROOT}/common/locktree_lock.cpp")
target_compile_features(locktree_baseline PUBLIC cxx_std_20)
target_include_directories(locktree_baseline PUBLIC "${LOCKTREE_PROJECT_ROOT}")
target_link_libraries(locktree_baseline PRIVATE locktree_standalone)
if(SANITIZER)
  target_compile_options(locktree_baseline PUBLIC -fsanitize=${SANITIZER} -fno-omit-frame-pointer)
  target_link_options(locktree_baseline PUBLIC -fsanitize=${SANITIZER})
endif()

option(LOCKTREE_BUILD_TESTS "Build the standalone LockTree adapter correctness tests" OFF)
if(LOCKTREE_BUILD_TESTS)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
  endif()
  add_executable(locktree_lock_tests "${LOCKTREE_PROJECT_ROOT}/tests/locktree_lock_test.cpp")
  target_link_libraries(locktree_lock_tests PRIVATE locktree_baseline)
  add_test(NAME locktree_lock_tests COMMAND locktree_lock_tests)
  set_tests_properties(locktree_lock_tests PROPERTIES TIMEOUT 30)
endif()
