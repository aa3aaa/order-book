cmake_minimum_required(VERSION 3.21)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)        # for clangd / VS Code
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OB_SANITIZE "AddressSanitizer + UBSan" OFF)

# The book is built twice: `orderbook` allocates list nodes from the pool
# allocator (the default), `orderbook_nopool` uses plain operator new. Both
# run the full test suite; the benchmark is built against each to compare.
function(ob_add_library name use_pool)
  add_library(${name} src/order_book.cpp)
  target_include_directories(${name} PUBLIC include)
  target_compile_definitions(${name} PUBLIC OB_USE_POOL=${use_pool})
  target_compile_options(${name} PUBLIC -Wall -Wextra -Wpedantic -Wshadow)
  if(OB_SANITIZE)
    target_compile_options(${name} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(${name} PUBLIC -fsanitize=address,undefined)
  endif()
endfunction()

ob_add_library(orderbook 1)
ob_add_library(orderbook_nopool 0)

enable_testing()
add_subdirectory(tests)

# Replay tool: CSV of orders in, trades and final book out.
add_executable(replay src/replay/main.cpp)
target_link_libraries(replay PRIVATE orderbook)
add_test(NAME replay_sample COMMAND replay ${CMAKE_CURRENT_SOURCE_DIR}/data/sample.csv)

# Latency benchmark, with and without the pool. Always run from the Release build.
add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE orderbook)
add_executable(bench_nopool bench/bench.cpp)
target_link_libraries(bench_nopool PRIVATE orderbook_nopool)

# Minimal example of using the book as a library.
add_executable(example examples/basic_usage.cpp)
target_link_libraries(example PRIVATE orderbook)
add_test(NAME example_runs COMMAND example)
