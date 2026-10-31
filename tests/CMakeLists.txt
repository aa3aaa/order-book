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

set(OB_TEST_SOURCES order_book_test.cpp differential_test.cpp pool_allocator_test.cpp)

# Same tests against both builds of the book. The pool hides use-after-free from
# AddressSanitizer (freed nodes go back to the pool, not to malloc), so the
# no-pool run is the one that lets ASan see every node.
add_executable(ob_tests ${OB_TEST_SOURCES})
target_link_libraries(ob_tests PRIVATE orderbook GTest::gtest_main)
gtest_discover_tests(ob_tests PROPERTIES ENVIRONMENT "${OB_TEST_ENV}")

add_executable(ob_tests_nopool ${OB_TEST_SOURCES})
target_link_libraries(ob_tests_nopool PRIVATE orderbook_nopool GTest::gtest_main)
gtest_discover_tests(ob_tests_nopool TEST_PREFIX "nopool." PROPERTIES ENVIRONMENT "${OB_TEST_ENV}")
