#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
#include "ob/price_ladder.hpp"
#include "ob/types.hpp"

namespace ob {

// A single-instrument limit order book with price-time priority.
// Single-threaded: all calls must come from one thread.
//
// OnTrade is the type of the trade callback. It is a template parameter so that
// a lambda passed in is called directly and can be inlined into the matching
// loop; with std::function every trade costs an indirect call the compiler
// cannot see through. `OrderBook` (below) is the std::function version, for
// code that wants one concrete type; the benchmark uses the lambda version:
//
//   ob::BasicOrderBook book([&](const ob::Trade& t) { ... });  // OnTrade deduced
template <class OnTrade = std::function<void(const Trade&)>>
class BasicOrderBook {
    public:
        // Every execution is reported through on_trade, in the order it happens.
        explicit BasicOrderBook(OnTrade on_trade = {}) : on_trade_(std::move(on_trade)) { index_rebuild(16); }

        // Matches the order against the opposite side, then rests any remainder
        // if it is a Limit order. Returns the quantity left resting: 0 if fully
        // filled, or if the remainder was cancelled because the order was Market
        // or IOC. Assigns the sequence number; orders with qty <= 0 are ignored.
        // Ids must be unique among resting orders.
        // Throws std::length_error, before changing anything, if a Limit order's
        // price would make its side's resting prices span more than ~4.2M ticks.
        Quantity add(Order order) {
            if (order.qty <= 0) [[unlikely]] return 0;
            if (order.type == OrderType::Limit) check_band(order.side, order.price);  // before anything changes
            order.seq = next_seq_++;  // the book stamps the sequence, never the caller

            if (order.side == Side::Buy) match(order, asks_); else match(order, bids_);

            if (order.qty > 0 && order.type == OrderType::Limit) {
                rest(order);
                return order.qty;
            }
            return 0;  // filled, or a Market / IOC remainder that is cancelled
        }

        // Removes a resting order. False if no order with that id is resting.
        bool cancel(OrderId id) {
            const NodeRef r = index_erase(id);  // one chain walk finds and unlinks it
            if (r == kNil) return false;
            detach(r);
            free_node(r);
            return true;
        }

        // Changes a resting order. Keeps its queue position only if the price is
        // unchanged and the quantity does not go up; otherwise the order is
        // cancelled and re-added as a new Limit order, which may match at once.
        // False if the id is unknown or new_qty <= 0 (the book is unchanged).
        // Throws std::length_error, as add() does, before changing anything.
        bool modify(OrderId id, Price new_price, Quantity new_qty) {
            if (new_qty <= 0) return false;
            const NodeRef r = index_find(id);
            if (r == kNil) return false;
            Node& n = nodes_[r];

            // Same price, quantity not going up: adjust in place and keep queue priority.
            if (new_price == n.price && new_qty <= n.qty) {
                const Quantity reduce_by = n.qty - new_qty;
                auto shrink = [&](auto& book) { book.at(n.price).total -= reduce_by; };
                if (n.side == Side::Buy) shrink(bids_); else shrink(asks_);
                n.qty = new_qty;
                return true;
            }

            // Anything else loses priority. It behaves exactly like cancel + add of
            // a new Limit order, but the order keeps its slab node and index entry:
            // unlink it from its level, match it as a taker, then link whatever is
            // left onto the back of its new level. That saves an index erase and
            // re-insert and a free + allocate on every price change.
            const Side side = n.side;
            check_band(side, new_price);  // throw now, while the order is still resting untouched
            detach(r);

            Order taker;
            taker.id = id;
            taker.side = side;
            taker.type = OrderType::Limit;
            taker.price = new_price;
            taker.qty = new_qty;
            taker.seq = next_seq_++;  // a fresh sequence: it has lost its place
            if (side == Side::Buy) match(taker, asks_); else match(taker, bids_);

            if (taker.qty > 0) {
                Node& moved = nodes_[r];
                moved.price = new_price;
                moved.qty = taker.qty;
                attach(r);
            } else {
                index_erase(id);  // fully filled while moving
                free_node(r);
            }
            return true;
        }

        std::optional<LevelView> best_bid() const { return top_of(bids_); }
        std::optional<LevelView> best_ask() const { return top_of(asks_); }

        // Up to n levels on one side, best first.
        std::vector<LevelView> depth(Side side, std::size_t n) const {
            return side == Side::Buy ? levels_of(bids_, n) : levels_of(asks_, n);
        }

        std::size_t order_count() const noexcept { return live_; }

        // Pre-allocates for up to max_resting_orders resting orders, so the hot
        // path never grows a container: the order slab and the id index's
        // bucket array are sized now (no reallocation or rehash mid-run).
        // Optional; call it before trading.
        void reserve(std::size_t max_resting_orders) {
            nodes_.reserve(max_resting_orders);
            std::size_t want = 16;
            while (want < max_resting_orders) want *= 2;
            if (want > buckets_.size()) index_rebuild(want);
        }

    private:
        // ---- Orders live in one slab and refer to each other by index ----
        //
        // Every resting order is one Node in nodes_. A Node is linked into two
        // lists at once, both by 32-bit index rather than pointer:
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

        // ---- callback ----

        void report(const Trade& t) {
            if constexpr (std::is_constructible_v<bool, const OnTrade&>) {
                if (on_trade_) on_trade_(t);  // std::function may be empty
            } else {
                on_trade_(t);
            }
        }

        // ---- matching ----

        // Executes every trade the taker can make against book (the opposite
        // side), reporting each one and removing filled makers.
        template <class Book>
        void match(Order& taker, Book& book) {
            while (taker.qty > 0 && !book.empty()) {
                const Price level_price = book.best_price();  // re-read every iteration: it moves as levels empty
                if (taker.type != OrderType::Market && !crosses(taker.side, taker.price, level_price)) break;

                Level& level = book.best_level();
                const NodeRef r = level.head;  // oldest order at the best price
                Node& maker = nodes_[r];
                const Quantity fill = std::min(taker.qty, maker.qty);

                report(Trade{taker.id, maker.id, level_price, fill});

                taker.qty -= fill;
                if (maker.qty == fill) {
                    index_erase(maker.id);
                    unlink(level, r);  // subtracts the maker's remaining qty (== fill) from the level
                    free_node(r);
                    if (level.empty()) book.on_level_emptied(level_price);
                } else {
                    maker.qty -= fill;
                    level.total -= fill;
                }
            }
        }

        // Puts an order with quantity left at the back of its price level.
        void rest(const Order& order) {
            const NodeRef r = alloc_node();
            Node& n = nodes_[r];
            n.id = order.id;
            n.price = order.price;
            n.qty = order.qty;
            n.side = order.side;
            index_insert(r);
            attach(r);
        }

        // Links a node onto the back of the level at its price (add() or modify()
        // already checked the band), or takes it off its level.
        void attach(NodeRef r) {
            const Node& n = nodes_[r];
            auto put = [&](auto& book) {
                Level& level = book.get_or_create(n.price);
                push_back(level, r);
                if (level.count == 1) book.on_level_filled(n.price);
            };
            if (n.side == Side::Buy) put(bids_); else put(asks_);
        }

        void detach(NodeRef r) noexcept {
            const Node& n = nodes_[r];
            auto take = [&](auto& book) {
                Level& level = book.at(n.price);
                unlink(level, r);
                if (level.empty()) book.on_level_emptied(n.price);
            };
            if (n.side == Side::Buy) take(bids_); else take(asks_);
        }

        // Throws std::length_error if a Limit order at this price could not rest.
        void check_band(Side side, Price price) const {
            const bool ok = side == Side::Buy ? bids_.can_hold(price) : asks_.can_hold(price);
            if (!ok) [[unlikely]] throw std::length_error("order price is too far from this side's resting prices");
        }

        // ---- slab ----

        NodeRef alloc_node() {
            if (free_head_ != kNil) {
                const NodeRef r = free_head_;
                free_head_ = nodes_[r].next;
                return r;
            }
            if (nodes_.size() >= kNil) [[unlikely]] throw std::length_error("order slab is full");
            nodes_.emplace_back();  // may reallocate: only indices are kept, so nothing dangles
            return static_cast<NodeRef>(nodes_.size() - 1);
        }

        void free_node(NodeRef r) noexcept {
            nodes_[r].next = free_head_;
            free_head_ = r;
        }

        // ---- level FIFO ----

        void push_back(Level& level, NodeRef r) noexcept {
            Node& n = nodes_[r];
            n.prev = level.tail;
            n.next = kNil;
            if (level.tail != kNil) nodes_[level.tail].next = r; else level.head = r;
            level.tail = r;
            ++level.count;
            level.total += n.qty;
        }

        void unlink(Level& level, NodeRef r) noexcept {
            const Node& n = nodes_[r];
            if (n.prev != kNil) nodes_[n.prev].next = n.next; else level.head = n.next;
            if (n.next != kNil) nodes_[n.next].prev = n.prev; else level.tail = n.prev;
            --level.count;
            level.total -= n.qty;
        }

        // ---- id index ----
        //
        // A bucket array of chain heads; the bucket is id & mask. Exchange ids
        // count up, so recent orders land in neighbouring buckets and stay in
        // cache, and with at least one bucket per live order the chains are
        // almost always 0 or 1 long. (Open addressing was tried first: with a
        // scrambling hash it lost the locality and was ~2x slower; with this
        // order-preserving one, consecutive ids form one long run and
        // backward-shift deletion degrades to a scan of the whole run.)

        NodeRef index_find(OrderId id) const noexcept {
            for (NodeRef r = buckets_[id & bucket_mask_]; r != kNil; r = nodes_[r].hnext)
                if (nodes_[r].id == id) return r;
            return kNil;
        }

        void index_insert(NodeRef r) {
            if (live_ + 1 > buckets_.size()) [[unlikely]] index_rebuild(buckets_.size() * 2);
            NodeRef& head = buckets_[nodes_[r].id & bucket_mask_];
            nodes_[r].hnext = head;
            head = r;
            ++live_;
        }

        // Unlinks and returns the node for id, or kNil if it is not resting.
        NodeRef index_erase(OrderId id) noexcept {
            NodeRef* link = &buckets_[id & bucket_mask_];
            while (*link != kNil) {
                const NodeRef r = *link;
                if (nodes_[r].id == id) {
                    *link = nodes_[r].hnext;
                    --live_;
                    return r;
                }
                link = &nodes_[r].hnext;
            }
            return kNil;
        }

        void index_rebuild(std::size_t buckets) {
            std::vector<NodeRef> old(buckets, kNil);
            old.swap(buckets_);
            bucket_mask_ = buckets - 1;
            for (NodeRef head : old) {
                for (NodeRef r = head; r != kNil;) {
                    const NodeRef next = nodes_[r].hnext;
                    NodeRef& b = buckets_[nodes_[r].id & bucket_mask_];
                    nodes_[r].hnext = b;
                    b = r;
                    r = next;
                }
            }
        }

        // ---- read-only helpers (one body for both ladder types) ----

        template <class Book>
        static std::optional<LevelView> top_of(const Book& book) {
            if (book.empty()) return std::nullopt;
            const auto& level = book.best_level();
            return LevelView{book.best_price(), level.total, level.count};
        }

        template <class Book>
        static std::vector<LevelView> levels_of(const Book& book, std::size_t n) {
            std::vector<LevelView> out;
            out.reserve(std::min(n, book.level_count()));
            if (n == 0) return out;
            book.for_each([&](Price price, const auto& level) {
                out.push_back(LevelView{price, level.total, level.count});
                return out.size() < n;
            });
            return out;
        }

        std::vector<Node> nodes_;
        NodeRef free_head_{kNil};
        std::vector<NodeRef> buckets_;
        std::size_t bucket_mask_{0};
        std::size_t live_{0};

        Bids bids_;
        Asks asks_;
        OnTrade on_trade_;
        Sequence next_seq_{1};
};

// The std::function version: one concrete type, compiled once in src/order_book.cpp.
using OrderBook = BasicOrderBook<>;
extern template class BasicOrderBook<>;

}  // namespace ob
