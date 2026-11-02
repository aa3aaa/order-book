#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include "ob/price_ladder.hpp"
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

        std::size_t order_count() const noexcept { return live_; }

        // Pre-allocates for up to max_resting_orders resting orders, so the hot
        // path never grows a container: the order slab and the id index's
        // bucket array are sized now (no reallocation or rehash mid-run).
        // Optional; call it before trading.
        void reserve(std::size_t max_resting_orders);

    private:
        // ---- Orders live in one slab and refer to each other by index ----
        //
        // Every resting order is one Node in nodes_. A Node is linked into two
        // singly-owned lists at once, both by 32-bit index rather than pointer:
        //   prev/next  the FIFO queue of its price level (time priority)
        //   hnext      its bucket's chain in the id index
        // Freed nodes go on a free list threaded through `next`, so allocation
        // is a pop and freeing is a push: this slab is the pool allocator,
        // specialised to one type. Indices stay valid when the vector grows.
        using NodeRef = std::uint32_t;
        static constexpr NodeRef kNil = 0xFFFF'FFFFu;

        struct Node {
            OrderId id{};
            Price price{};
            Quantity qty{};
            NodeRef prev{kNil};
            NodeRef next{kNil};
            NodeRef hnext{kNil};
            Side side{Side::Buy};
        };  // 40 bytes: no type (a resting order is always Limit) and no seq (queue position is the priority)

        struct Level {
            Quantity total{0};
            NodeRef head{kNil};  // oldest order
            NodeRef tail{kNil};  // newest order
            std::uint32_t count{0};
            bool empty() const noexcept { return count == 0; }
        };

        using Bids = PriceLadder<Level, true>;   // best = highest price
        using Asks = PriceLadder<Level, false>;  // best = lowest price

        // Executes every trade the taker can make against book (the opposite
        // side), reporting each one and removing filled makers.
        template <class Book>
        void match(Order& taker, Book& book);

        // Puts an order with quantity left at the back of its price level.
        void rest(const Order& order);

        // Throws std::length_error if a Limit order at this price could not rest.
        void check_band(Side side, Price price) const;

        // Slab.
        NodeRef alloc_node();
        void free_node(NodeRef r) noexcept;

        // Level FIFO.
        void push_back(Level& level, NodeRef r) noexcept;
        void unlink(Level& level, NodeRef r) noexcept;

        // Id index: a bucket array of chain heads. The bucket is id & mask:
        // exchange ids count up, so recent orders land in neighbouring buckets
        // and stay in cache, and with at least one bucket per live order the
        // chains are almost always 0 or 1 long.
        NodeRef index_find(OrderId id) const noexcept;
        void index_insert(NodeRef r);
        NodeRef index_erase(OrderId id) noexcept;  // returns the node, or kNil if absent
        void index_rebuild(std::size_t buckets);

        std::vector<Node> nodes_;
        NodeRef free_head_{kNil};
        std::vector<NodeRef> buckets_;
        std::size_t bucket_mask_{0};
        std::size_t live_{0};

        Bids bids_;
        Asks asks_;
        TradeCallback on_trade_;
        Sequence next_seq_{1};
};

}  // namespace ob
