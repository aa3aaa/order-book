cmake_minimum_required(VERSION 3.21)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)        # for clangd / VS Code
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OB_SANITIZE "AddressSanitizer + UBSan" OFF)

# ---- Optimisation switches (all off by default; see README "Compiler settings") ----
option(OB_NATIVE "Tune for this machine's CPU (-march=native or -mcpu=native)" OFF)
option(OB_LTO "Link-time optimisation, so calls into the library can be inlined" OFF)
set(OB_PGO "" CACHE STRING "Profile-guided optimisation phase: empty, GENERATE or USE (scripts/pgo_build.sh drives it)")
set(OB_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Where PGO profiles are written and read")

include(CheckCXXCompilerFlag)
if(OB_NATIVE)
  check_cxx_compiler_flag(-march=native OB_HAS_MARCH_NATIVE)
  check_cxx_compiler_flag(-mcpu=native OB_HAS_MCPU_NATIVE)
  if(OB_HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
  elseif(OB_HAS_MCPU_NATIVE)
    add_compile_options(-mcpu=native)
  else()
    message(WARNING "OB_NATIVE: compiler accepts neither -march=native nor -mcpu=native")
  endif()
endif()

if(OB_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT OB_IPO_OK OUTPUT OB_IPO_MSG)
  if(OB_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(WARNING "OB_LTO: not supported here: ${OB_IPO_MSG}")
  endif()
endif()

if(OB_PGO STREQUAL "GENERATE")
  add_compile_options(-fprofile-generate=${OB_PGO_DIR})
  add_link_options(-fprofile-generate=${OB_PGO_DIR})
elseif(OB_PGO STREQUAL "USE")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang needs the raw profiles merged first (the script does this).
    add_compile_options(-fprofile-use=${OB_PGO_DIR}/merged.profdata
                        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
    add_link_options(-fprofile-use=${OB_PGO_DIR}/merged.profdata)
  else()
    add_compile_options(-fprofile-use=${OB_PGO_DIR} -fprofile-correction -Wno-missing-profile)
    add_link_options(-fprofile-use=${OB_PGO_DIR})
  endif()
elseif(NOT OB_PGO STREQUAL "")
  message(FATAL_ERROR "OB_PGO must be empty, GENERATE or USE (got '${OB_PGO}')")
endif()

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
