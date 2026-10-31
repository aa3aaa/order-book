cmake_minimum_required(VERSION 3.21)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)        # for clangd / VS Code
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OB_SANITIZE "AddressSanitizer + UBSan" OFF)

add_library(orderbook src/order_book.cpp)
target_include_directories(orderbook PUBLIC include)
target_compile_options(orderbook PUBLIC -Wall -Wextra -Wpedantic -Wshadow)
if(OB_SANITIZE)
  target_compile_options(orderbook PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(orderbook PUBLIC -fsanitize=address,undefined)
endif()

enable_testing()
add_subdirectory(tests)

# Replay tool: CSV of orders in, trades and final book out.
add_executable(replay src/replay/main.cpp)
target_link_libraries(replay PRIVATE orderbook)
add_test(NAME replay_sample COMMAND replay ${CMAKE_CURRENT_SOURCE_DIR}/data/sample.csv)
