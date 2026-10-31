#include <gtest/gtest.h>
#include <vector>
#include "ob/order_book.hpp"

using ob::LevelView;
using ob::Side;
using ob::Trade;

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
ob::Order market(ob::OrderId id, Side side, ob::Quantity qty) { return make(id, side, ob::OrderType::Market, 0, qty); }
ob::Order ioc(ob::OrderId id, Side side, ob::Price px, ob::Quantity qty) { return make(id, side, ob::OrderType::IOC, px, qty); }

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

// ---------- Step 4: matching ----------

TEST_F(BookTest, FullFillTradesAtTheMakersPrice) {
    book.add(sell(1, 100, 10));
    EXPECT_EQ(book.add(buy(2, 105, 10)), 0);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, 100, 10}));
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, PartialFillRestsTheRemainder) {
    book.add(sell(1, 100, 5));
    EXPECT_EQ(book.add(buy(2, 100, 12)), 7);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, 100, 5}));
    EXPECT_EQ(book.best_bid(), (LevelView{100, 7, 1}));
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST_F(BookTest, PartialFillLeavesTheMaker) {
    book.add(sell(1, 100, 12));
    EXPECT_EQ(book.add(buy(2, 100, 5)), 0);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, 100, 5}));
    EXPECT_EQ(book.best_ask(), (LevelView{100, 7, 1}));
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST_F(BookTest, TakerWalksLevelsUpToItsLimit) {
    book.add(sell(1, 100, 5));
    book.add(sell(2, 101, 5));
    book.add(sell(3, 102, 5));
    EXPECT_EQ(book.add(buy(4, 101, 12)), 2);
    const std::vector<Trade> expected{{4, 1, 100, 5}, {4, 2, 101, 5}};
    EXPECT_EQ(trades, expected);
    EXPECT_EQ(book.best_bid(), (LevelView{101, 2, 1}));
    EXPECT_EQ(book.best_ask(), (LevelView{102, 5, 1}));
}

TEST_F(BookTest, FifoWithinALevel) {
    book.add(sell(1, 100, 5));
    book.add(sell(2, 100, 5));
    book.add(buy(3, 100, 7));
    const std::vector<Trade> expected{{3, 1, 100, 5}, {3, 2, 100, 2}};
    EXPECT_EQ(trades, expected);
    EXPECT_EQ(book.best_ask(), (LevelView{100, 3, 1}));
}

TEST_F(BookTest, NoTradeWhenPricesDoNotCross) {
    book.add(sell(1, 101, 5));
    EXPECT_EQ(book.add(buy(2, 100, 5)), 5);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_bid(), (LevelView{100, 5, 1}));
    EXPECT_EQ(book.best_ask(), (LevelView{101, 5, 1}));
}

TEST_F(BookTest, SellTakerMatchesHighestBidFirst) {
    book.add(buy(1, 99, 5));
    book.add(buy(2, 101, 5));
    book.add(buy(3, 100, 5));
    EXPECT_EQ(book.add(sell(4, 100, 8)), 0);
    const std::vector<Trade> expected{{4, 2, 101, 5}, {4, 3, 100, 3}};
    EXPECT_EQ(trades, expected);
    EXPECT_EQ(book.best_bid(), (LevelView{100, 2, 1}));
}

TEST(BookNoCallback, MatchingWithoutACallbackIsSafe) {
    ob::OrderBook book;  // no callback passed
    book.add(sell(1, 100, 5));
    EXPECT_EQ(book.add(buy(2, 100, 5)), 0);
    EXPECT_EQ(book.order_count(), 0u);
}

// ---------- Step 5: market and IOC ----------

TEST_F(BookTest, MarketOrderSweepsLevelsAndRestsNothing) {
    book.add(sell(1, 100, 5));
    book.add(sell(2, 110, 5));
    EXPECT_EQ(book.add(market(3, Side::Buy, 12)), 0);
    const std::vector<Trade> expected{{3, 1, 100, 5}, {3, 2, 110, 5}};
    EXPECT_EQ(trades, expected);
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST_F(BookTest, MarketOrderOnAnEmptySideDoesNothing) {
    book.add(buy(1, 100, 5));
    EXPECT_EQ(book.add(market(2, Side::Buy, 10)), 0);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(BookTest, IocTradesThenCancelsTheRemainder) {
    book.add(sell(1, 100, 5));
    EXPECT_EQ(book.add(ioc(2, Side::Buy, 100, 8)), 0);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, 100, 5}));
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, IocRespectsItsLimitPrice) {
    book.add(sell(1, 101, 5));
    EXPECT_EQ(book.add(ioc(2, Side::Buy, 100, 5)), 0);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_ask(), (LevelView{101, 5, 1}));
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

// ---------- Step 6: modify ----------

TEST_F(BookTest, ModifyDownAtSamePriceKeepsPriority) {
    book.add(sell(1, 100, 10));
    book.add(sell(2, 100, 10));
    EXPECT_TRUE(book.modify(1, 100, 4));
    EXPECT_EQ(book.best_ask(), (LevelView{100, 14, 2}));
    book.add(buy(3, 100, 4));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{3, 1, 100, 4}));
}

TEST_F(BookTest, ModifyUpLosesPriority) {
    book.add(sell(1, 100, 10));
    book.add(sell(2, 100, 10));
    EXPECT_TRUE(book.modify(1, 100, 12));
    EXPECT_EQ(book.best_ask(), (LevelView{100, 22, 2}));
    book.add(buy(3, 100, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{3, 2, 100, 10}));
}

TEST_F(BookTest, ModifyPriceRequeuesAndCanMatch) {
    book.add(buy(1, 100, 10));
    book.add(sell(2, 105, 5));
    EXPECT_TRUE(book.modify(1, 105, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{1, 2, 105, 5}));
    EXPECT_EQ(book.best_bid(), (LevelView{105, 5, 1}));
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST_F(BookTest, ModifyUnknownIdReturnsFalseAndChangesNothing) {
    book.add(buy(1, 100, 10));
    EXPECT_FALSE(book.modify(99, 100, 5));
    EXPECT_EQ(book.best_bid(), (LevelView{100, 10, 1}));
}

TEST_F(BookTest, ModifyToNonPositiveQuantityReturnsFalse) {
    book.add(buy(1, 100, 10));
    EXPECT_FALSE(book.modify(1, 100, 0));
    EXPECT_FALSE(book.modify(1, 100, -3));
    EXPECT_EQ(book.best_bid(), (LevelView{100, 10, 1}));
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(BookTest, ModifiedOrderCanStillBeCancelled) {
    book.add(buy(1, 100, 10));
    EXPECT_TRUE(book.modify(1, 99, 10));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.order_count(), 0u);
}
