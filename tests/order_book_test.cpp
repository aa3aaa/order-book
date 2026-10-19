#include <gtest/gtest.h>
#include "ob/order_book.hpp"

TEST(BookTest, EmptyBookHasNoBestLevels) {
    ob::OrderBook book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}