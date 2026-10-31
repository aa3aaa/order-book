#pragma once
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>
#include "ob/types.hpp"

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
        Quantity add(Order order);

        // Removes a resting order. False if no order with that id is resting.
        bool cancel(OrderId id);

        // Changes a resting order. Keeps its queue position only if the price is
        // unchanged and the quantity does not go up; otherwise the order is
        // cancelled and re-added as a new Limit order, which may match at once.
        // False if the id is unknown or new_qty <= 0 (the book is unchanged).
        bool modify(OrderId id, Price new_price, Quantity new_qty);

        std::optional<LevelView> best_bid() const;
        std::optional<LevelView> best_ask() const;

        // Up to n levels on one side, best first.
        std::vector<LevelView> depth(Side side, std::size_t n) const;

        std::size_t order_count() const noexcept { return index_.size(); }

    private:
        struct Level {
            Quantity total{0};
            std::list<Order> orders;  // FIFO: front is the oldest
        };
        struct Handle {
            Side side;
            Price price;
            std::list<Order>::iterator it;  // points at the order's own node
        };
        using Bids = std::map<Price, Level, std::greater<>>;  // highest price first
        using Asks = std::map<Price, Level>;                  // lowest price first

        // Executes every trade the taker can make against book (the opposite
        // side), reporting each one and removing filled makers.
        template <class Book>
        void match(Order& taker, Book& book);

        // Puts an order with quantity left at the back of its price level.
        void rest(Order order);

        Bids bids_;
        Asks asks_;
        std::unordered_map<OrderId, Handle> index_;
        TradeCallback on_trade_;
        Sequence next_seq_{1};
};

}  // namespace ob
