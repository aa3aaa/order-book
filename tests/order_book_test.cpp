#include <gtest/gtest.h>
#include <stdexcept>
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

// ---------- reserve() ----------

TEST_F(BookTest, ReserveChangesNothingVisible) {
    book.reserve(10'000);
    EXPECT_EQ(book.order_count(), 0u);
    book.add(sell(1, 100, 5));
    book.reserve(20'000);  // calling it again on a live book is safe
    EXPECT_EQ(book.add(buy(2, 100, 3)), 0);
    EXPECT_EQ(book.best_ask(), (LevelView{100, 2, 1}));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.order_count(), 0u);
}

// ---------- tick-indexed levels: growth and the price band ----------

TEST_F(BookTest, LevelsFarApartAreKeptInPriceOrder) {
    book.add(buy(1, 100, 1));
    book.add(buy(2, 1'000'000, 2));  // forces the bid array to grow upward
    book.add(buy(3, 5, 3));          // and downward
    const std::vector<LevelView> expected{{1'000'000, 2, 1}, {100, 1, 1}, {5, 3, 1}};
    EXPECT_EQ(book.depth(Side::Buy, 10), expected);

    EXPECT_EQ(book.add(market(4, Side::Sell, 6)), 0);
    const std::vector<Trade> fills{{4, 2, 1'000'000, 2}, {4, 1, 100, 1}, {4, 3, 5, 3}};
    EXPECT_EQ(trades, fills);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, CancelAndMatchStillWorkAfterTheArrayGrows) {
    book.add(sell(1, 100, 5));
    book.add(sell(2, 100, 5));
    book.add(sell(3, 90'000, 5));  // growth moves level 100's orders to a new array
    EXPECT_TRUE(book.cancel(1));   // the index's iterator for order 1 must still be valid
    book.add(buy(4, 100, 5));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{4, 2, 100, 5}));
    EXPECT_EQ(book.best_ask(), (LevelView{90'000, 5, 1}));
}

TEST_F(BookTest, BestPriceSkipsGapsWhenTheBestLevelEmpties) {
    book.add(sell(1, 100, 1));
    book.add(sell(2, 150, 1));
    book.add(sell(3, 400, 1));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_ask()->price, 150);
    book.add(buy(4, 150, 1));
    EXPECT_EQ(book.best_ask()->price, 400);
}

TEST_F(BookTest, PriceBeyondTheBandIsRejectedBeforeAnythingChanges) {
    constexpr ob::Price far = ob::Price{1} << 23;  // further than ~4.2M ticks from the resting bid
    book.add(buy(1, 0, 5));
    book.add(sell(2, 10, 5));
    // This buy would cross the ask at 10, but could not rest beside the bid at 0.
    EXPECT_THROW(book.add(buy(3, far, 8)), std::length_error);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_ask(), (LevelView{10, 5, 1}));
    EXPECT_EQ(book.order_count(), 2u);

    EXPECT_THROW(book.modify(1, far, 5), std::length_error);
    EXPECT_EQ(book.best_bid(), (LevelView{0, 5, 1}));  // order 1 still resting, unchanged

    // Market and IOC orders never rest, so their price is not checked.
    EXPECT_EQ(book.add(ioc(4, Side::Buy, far, 2)), 0);
    EXPECT_EQ(book.best_ask(), (LevelView{10, 3, 1}));
}

TEST_F(BookTest, AnEmptySideCanRestartAtAnyPrice) {
    constexpr ob::Price far = ob::Price{1} << 23;
    book.add(buy(1, 0, 5));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.add(buy(2, far, 5)), 5);  // nothing resting, so no band to break
    EXPECT_EQ(book.best_bid(), (LevelView{far, 5, 1}));
}

// ---------- slab and id index ----------

// A million orders with consecutive ids, half cancelled, the rest filled: the
// shape that breaks open addressing with an order-preserving hash. Must stay fast.
TEST_F(BookTest, ManyConsecutiveIdsStayFast) {
    constexpr ob::OrderId n = 1'000'000;
    for (ob::OrderId id = 1; id <= n; ++id) book.add(sell(id, 100 + static_cast<ob::Price>(id % 50), 1));
    EXPECT_EQ(book.order_count(), n);
    for (ob::OrderId id = 1; id <= n; id += 2) ASSERT_TRUE(book.cancel(id));
    EXPECT_EQ(book.order_count(), n / 2);
    EXPECT_EQ(book.add(market(n + 1, Side::Buy, static_cast<ob::Quantity>(n))), 0);
    EXPECT_EQ(trades.size(), n / 2);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, FreedNodesAreReusedAndIdsCanComeBack) {
    for (int round = 0; round < 3; ++round) {
        for (ob::OrderId id = 1; id <= 100; ++id) book.add(buy(id, 50 + static_cast<ob::Price>(id), 2));
        for (ob::OrderId id = 1; id <= 100; ++id) ASSERT_TRUE(book.cancel(id));
        EXPECT_EQ(book.order_count(), 0u);
        EXPECT_EQ(book.best_bid(), std::nullopt);
    }
    book.add(buy(7, 100, 3));  // a reused id after its earlier order was cancelled
    EXPECT_EQ(book.best_bid(), (LevelView{100, 3, 1}));
}

TEST_F(BookTest, IdsThatShareABucketAreKeptApart) {
    // With 16 buckets, ids 1, 17, 33... all hash to bucket 1.
    for (ob::OrderId id = 1; id <= 1 + 16 * 9; id += 16) book.add(sell(id, 100, 1));
    EXPECT_TRUE(book.cancel(33));
    EXPECT_FALSE(book.cancel(33));
    EXPECT_TRUE(book.modify(65, 100, 1));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_ask(), (LevelView{100, 8, 8}));
}

// ---------- template callback ----------

// The lambda-typed book (callback inlined) must behave exactly like the
// std::function one on the same orders.
TEST(BasicOrderBookTest, LambdaCallbackBookMatchesStdFunctionBook) {
    std::vector<Trade> a, b;
    ob::OrderBook fn_book([&](const Trade& t) { a.push_back(t); });
    ob::BasicOrderBook lambda_book([&](const Trade& t) { b.push_back(t); });
    for (ob::OrderId id = 1; id <= 2000; ++id) {
        const auto px = static_cast<ob::Price>(95 + id * 7 % 11);
        const auto qty = static_cast<ob::Quantity>(1 + id % 13);
        const auto o = id % 2 ? sell(id, px, qty) : buy(id, px, qty);
        ASSERT_EQ(fn_book.add(o), lambda_book.add(o));
        if (id % 5 == 0) { ASSERT_EQ(fn_book.cancel(id - 3), lambda_book.cancel(id - 3)); }
        if (id % 7 == 0) { ASSERT_EQ(fn_book.modify(id - 2, px, qty), lambda_book.modify(id - 2, px, qty)); }
    }
    EXPECT_EQ(a, b);
    EXPECT_GT(a.size(), 100u);
    EXPECT_EQ(fn_book.depth(Side::Buy, 20), lambda_book.depth(Side::Buy, 20));
    EXPECT_EQ(fn_book.depth(Side::Sell, 20), lambda_book.depth(Side::Sell, 20));
}

TEST(BasicOrderBookTest, AnyCallableWorksAsTheCallback) {
    struct Counter {
        int* fills;
        void operator()(const Trade&) const { ++*fills; }
    };
    int fills = 0;
    ob::BasicOrderBook<Counter> book(Counter{&fills});
    book.add(sell(1, 100, 5));
    book.add(buy(2, 100, 5));
    EXPECT_EQ(fills, 1);
}

// ---------- modify that moves the node ----------

TEST_F(BookTest, ModifyThatFullyFillsRemovesTheOrder) {
    book.add(buy(1, 100, 5));
    book.add(sell(2, 102, 10));
    EXPECT_TRUE(book.modify(1, 102, 5));  // moves up and is filled completely
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{1, 2, 102, 5}));
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_FALSE(book.cancel(1));  // gone from the index too
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), (LevelView{102, 5, 1}));
}

TEST_F(BookTest, MovedOrderGoesToTheBackOfItsNewLevel) {
    book.add(sell(1, 105, 5));
    book.add(sell(2, 100, 5));
    EXPECT_TRUE(book.modify(1, 100, 5));  // joins level 100 behind order 2
    book.add(buy(3, 100, 6));
    const std::vector<Trade> expected{{3, 2, 100, 5}, {3, 1, 100, 1}};
    EXPECT_EQ(trades, expected);
    EXPECT_EQ(book.best_ask(), (LevelView{100, 4, 1}));
    EXPECT_EQ(book.depth(Side::Sell, 10).size(), 1u);  // level 105 was emptied and removed
}
