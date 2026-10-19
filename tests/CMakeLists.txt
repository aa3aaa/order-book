include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(ob_tests order_book_test.cpp)
target_link_libraries(ob_tests PRIVATE orderbook GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(ob_tests)