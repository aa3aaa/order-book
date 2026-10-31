#include <gtest/gtest.h>
#include <vector>
#include "ob/order_book.hpp"

using ob::LevelView;
using ob::Side;

namespace {

ob::Order make(ob::OrderId id, Side side, ob::OrderType type, ob::Price price, ob::Quantity qty) {
    ob::Order o;
    o.id = id;
    o.side = side;
    o.type = type;
    o.price = price;
    o.qty = qty;
    return o;
}
ob::Order buy(ob::OrderId id, ob::Price px, ob::Quantity qty) { return make(id, Side::Buy, ob::OrderType::Limit, px, qty); }
ob::Order sell(ob::OrderId id, ob::Price px, ob::Quantity qty) { return make(id, Side::Sell, ob::OrderType::Limit, px, qty); }

}  // namespace

// Every test starts with a fresh book and a vector that collects its trades.
class BookTest : public ::testing::Test {
    protected:
        std::vector<ob::Trade> trades;
        ob::OrderBook book{[this](const ob::Trade& t) { trades.push_back(t); }};
};

// ---------- Step 1: an empty book reports nothing ----------

TEST_F(BookTest, EmptyBookHasNoBestLevels) {
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST_F(BookTest, EmptyBookReportsNothing) {
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_TRUE(book.depth(Side::Buy, 5).empty());
    EXPECT_TRUE(book.depth(Side::Sell, 5).empty());
}

// ---------- Step 2: a limit order that crosses nothing rests ----------

TEST_F(BookTest, RestingBuyBecomesBestBid) {
    EXPECT_EQ(book.add(buy(1, 100, 10)), 10);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_bid(), (LevelView{100, 10, 1}));
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(BookTest, BestLevelsAreTheBestPrices) {
    book.add(buy(1, 99, 1));
    book.add(buy(2, 101, 1));
    book.add(buy(3, 100, 1));
    book.add(sell(4, 103, 1));
    book.add(sell(5, 102, 1));
    EXPECT_EQ(book.best_bid()->price, 101);
    EXPECT_EQ(book.best_ask()->price, 102);
    EXPECT_EQ(book.order_count(), 5u);
}

TEST_F(BookTest, OrdersAtOnePriceShareALevel) {
    book.add(sell(1, 100, 4));
    book.add(sell(2, 100, 6));
    EXPECT_EQ(book.best_ask(), (LevelView{100, 10, 2}));
}

TEST_F(BookTest, DepthListsLevelsBestFirstAndStopsAtN) {
    book.add(buy(1, 99, 1));
    book.add(buy(2, 101, 2));
    book.add(buy(3, 100, 3));
    const std::vector<LevelView> expected{{101, 2, 1}, {100, 3, 1}};
    EXPECT_EQ(book.depth(Side::Buy, 2), expected);
    EXPECT_EQ(book.depth(Side::Buy, 10).size(), 3u);
    EXPECT_TRUE(book.depth(Side::Sell, 10).empty());
}

// ---------- Step 3: cancel in O(1) ----------

TEST_F(BookTest, CancelRemovesTheOrder) {
    book.add(buy(1, 100, 10));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, CancelTwiceReturnsFalse) {
    book.add(buy(1, 100, 10));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.cancel(1));
}

TEST_F(BookTest, CancelUnknownIdReturnsFalse) {
    EXPECT_FALSE(book.cancel(42));
    book.add(buy(1, 100, 10));
    EXPECT_FALSE(book.cancel(42));
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(BookTest, CancelFirstOfTwoLeavesTheSecond) {
    book.add(buy(1, 100, 10));
    book.add(buy(2, 100, 7));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_bid(), (LevelView{100, 7, 1}));
}

TEST_F(BookTest, CancelLastOrderAtALevelRemovesTheLevel) {
    book.add(sell(1, 101, 5));
    book.add(sell(2, 102, 5));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_ask(), (LevelView{102, 5, 1}));
    EXPECT_EQ(book.depth(Side::Sell, 10).size(), 1u);
}
