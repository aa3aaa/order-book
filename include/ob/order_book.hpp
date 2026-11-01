#pragma once
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include "ob/pool_allocator.hpp"
#include "ob/price_ladder.hpp"
#include "ob/types.hpp"

// 1: order-list nodes come from a fixed-size pool (default). 0: plain operator new.
// Set by CMake per library target; both builds pass the same tests.
#ifndef OB_USE_POOL
#define OB_USE_POOL 1
#endif

namespace ob {

// A single-instrument limit order book with price-time priority.
// Single-threaded: all calls must come from one thread.
class OrderBook {
    public:
        using TradeCallback = std::function<void(const Trade&)>;

        // Every execution is reported through on_trade, in the order it happens.
        explicit OrderBook(TradeCallback on_trade = {});

        // Matches the order against the opposite side, then rests any remainder
        // if it is a Limit order. Returns the quantity left resting: 0 if fully
        // filled, or if the remainder was cancelled because the order was Market
        // or IOC. Assigns the sequence number; orders with qty <= 0 are ignored.
        // Ids must be unique among resting orders.
        // Throws std::length_error, before changing anything, if a Limit order's
        // price would make its side's resting prices span more than ~4.2M ticks.
        Quantity add(Order order);

        // Removes a resting order. False if no order with that id is resting.
        bool cancel(OrderId id);

        // Changes a resting order. Keeps its queue position only if the price is
        // unchanged and the quantity does not go up; otherwise the order is
        // cancelled and re-added as a new Limit order, which may match at once.
        // False if the id is unknown or new_qty <= 0 (the book is unchanged).
        // Throws std::length_error, as add() does, before changing anything.
        bool modify(OrderId id, Price new_price, Quantity new_qty);

        std::optional<LevelView> best_bid() const;
        std::optional<LevelView> best_ask() const;

        // Up to n levels on one side, best first.
        std::vector<LevelView> depth(Side side, std::size_t n) const;

        std::size_t order_count() const noexcept { return index_.size(); }

        // Pre-allocates for up to max_resting_orders resting orders, so the hot
        // path never grows a container: the id index gets its buckets now (no
        // rehash later), and in the pool build both node pools are grown to size
        // up front instead of in chunks mid-run. Optional; call it before trading.
        void reserve(std::size_t max_resting_orders);

    private:
#if OB_USE_POOL
        template <class T> using Alloc = PoolAllocator<T>;
#else
        template <class T> using Alloc = std::allocator<T>;
#endif
        using OrderList = std::list<Order, Alloc<Order>>;
        struct Level {
            Quantity total{0};
            OrderList orders;  // FIFO: front is the oldest
        };
        struct Handle {
            Side side{};
            Price price{};
            OrderList::iterator it{};  // points at the order's own node
        };
        // Index nodes come from the pool too: otherwise every add still calls new once.
        using Index = std::unordered_map<OrderId, Handle, std::hash<OrderId>, std::equal_to<>,
                                         Alloc<std::pair<const OrderId, Handle>>>;
        using Bids = PriceLadder<Level, true>;   // best = highest price
        using Asks = PriceLadder<Level, false>;  // best = lowest price

        // Executes every trade the taker can make against book (the opposite
        // side), reporting each one and removing filled makers.
        template <class Book>
        void match(Order& taker, Book& book);

        // Puts an order with quantity left at the back of its price level.
        void rest(Order order);

        // Throws std::length_error if a Limit order at this price could not rest.
        void check_band(Side side, Price price) const;

        Bids bids_;
        Asks asks_;
        Index index_;
        TradeCallback on_trade_;
        Sequence next_seq_{1};
};

}  // namespace ob
