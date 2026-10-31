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

        // Rests the order (matching comes in step 4). Returns the quantity left
        // resting. Assigns the sequence number. Ids must be unique among resting orders.
        Quantity add(Order order);

        // Removes a resting order. False if no order with that id is resting.
        bool cancel(OrderId id);

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

        void rest(Order order);

        Bids bids_;
        Asks asks_;
        std::unordered_map<OrderId, Handle> index_;
        TradeCallback on_trade_;
        Sequence next_seq_{1};
};

}  // namespace ob
