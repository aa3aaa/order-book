include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# The sanitised build is slow, so the differential test runs fewer operations there.
if(OB_SANITIZE)
  set(OB_TEST_ENV "OB_DIFF_OPS=20000")
else()
  set(OB_TEST_ENV "")
endif()

add_executable(ob_tests order_book_test.cpp differential_test.cpp)
target_link_libraries(ob_tests PRIVATE orderbook GTest::gtest_main)
gtest_discover_tests(ob_tests PROPERTIES ENVIRONMENT "${OB_TEST_ENV}")
