#include "ob/order_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ob {

namespace {

// Bids and Asks are different ladder types, so the read-only helpers are
// templates: one body serves both sides.
template <class Book>
std::optional<LevelView> top_of(const Book& book) {
    if (book.empty()) return std::nullopt;
    const auto& level = book.best_level();
    return LevelView{book.best_price(), level.total, level.count};
}

template <class Book>
std::vector<LevelView> levels_of(const Book& book, std::size_t n) {
    std::vector<LevelView> out;
    out.reserve(std::min(n, book.level_count()));
    if (n == 0) return out;
    book.for_each([&](Price price, const auto& level) {
        out.push_back(LevelView{price, level.total, level.count});
        return out.size() < n;
    });
    return out;
}

std::size_t pow2_at_least(std::size_t n) {
    std::size_t c = 16;
    while (c < n) c *= 2;
    return c;
}

}  // namespace

OrderBook::OrderBook(TradeCallback on_trade) : on_trade_(std::move(on_trade)) { index_rebuild(16); }

void OrderBook::reserve(std::size_t max_resting_orders) {
    nodes_.reserve(max_resting_orders);
    const std::size_t want = pow2_at_least(max_resting_orders);
    if (want > buckets_.size()) index_rebuild(want);
}

// ---------------------------------------------------------------- slab

OrderBook::NodeRef OrderBook::alloc_node() {
    if (free_head_ != kNil) {
        const NodeRef r = free_head_;
        free_head_ = nodes_[r].next;
        return r;
    }
    if (nodes_.size() >= kNil) throw std::length_error("order slab is full");
    nodes_.emplace_back();  // may reallocate: only indices are kept, so nothing dangles
    return static_cast<NodeRef>(nodes_.size() - 1);
}

void OrderBook::free_node(NodeRef r) noexcept {
    nodes_[r].next = free_head_;
    free_head_ = r;
}

// ---------------------------------------------------------------- level FIFO

void OrderBook::push_back(Level& level, NodeRef r) noexcept {
    Node& n = nodes_[r];
    n.prev = level.tail;
    n.next = kNil;
    if (level.tail != kNil) nodes_[level.tail].next = r; else level.head = r;
    level.tail = r;
    ++level.count;
    level.total += n.qty;
}

void OrderBook::unlink(Level& level, NodeRef r) noexcept {
    const Node& n = nodes_[r];
    if (n.prev != kNil) nodes_[n.prev].next = n.next; else level.head = n.next;
    if (n.next != kNil) nodes_[n.next].prev = n.prev; else level.tail = n.prev;
    --level.count;
    level.total -= n.qty;
}

// ---------------------------------------------------------------- id index

OrderBook::NodeRef OrderBook::index_find(OrderId id) const noexcept {
    for (NodeRef r = buckets_[id & bucket_mask_]; r != kNil; r = nodes_[r].hnext)
        if (nodes_[r].id == id) return r;
    return kNil;
}

void OrderBook::index_insert(NodeRef r) {
    if (live_ + 1 > buckets_.size()) [[unlikely]] index_rebuild(buckets_.size() * 2);
    NodeRef& head = buckets_[nodes_[r].id & bucket_mask_];
    nodes_[r].hnext = head;
    head = r;
    ++live_;
}

OrderBook::NodeRef OrderBook::index_erase(OrderId id) noexcept {
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

void OrderBook::index_rebuild(std::size_t buckets) {
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

// ---------------------------------------------------------------- queries

std::optional<LevelView> OrderBook::best_bid() const { return top_of(bids_); }

std::optional<LevelView> OrderBook::best_ask() const { return top_of(asks_); }

std::vector<LevelView> OrderBook::depth(Side side, std::size_t n) const {
    return side == Side::Buy ? levels_of(bids_, n) : levels_of(asks_, n);
}

// ---------------------------------------------------------------- operations

void OrderBook::check_band(Side side, Price price) const {
    const bool ok = side == Side::Buy ? bids_.can_hold(price) : asks_.can_hold(price);
    if (!ok) [[unlikely]] throw std::length_error("order price is too far from this side's resting prices");
}

Quantity OrderBook::add(Order order) {
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

// Defined here and only called from this file, so the compiler instantiates it
// for both Bids and Asks without an explicit declaration.
template <class Book>
void OrderBook::match(Order& taker, Book& book) {
    while (taker.qty > 0 && !book.empty()) {
        const Price level_price = book.best_price();  // re-read every iteration: it moves as levels empty
        if (taker.type != OrderType::Market && !crosses(taker.side, taker.price, level_price)) break;

        Level& level = book.best_level();
        const NodeRef r = level.head;  // oldest order at the best price
        Node& maker = nodes_[r];
        const Quantity fill = std::min(taker.qty, maker.qty);

        if (on_trade_) on_trade_(Trade{taker.id, maker.id, level_price, fill});

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

void OrderBook::rest(const Order& order) {
    const NodeRef r = alloc_node();
    Node& n = nodes_[r];
    n.id = order.id;
    n.price = order.price;
    n.qty = order.qty;
    n.side = order.side;
    index_insert(r);

    auto put = [&](auto& book) {
        Level& level = book.get_or_create(order.price);  // add() already checked the band
        push_back(level, r);
        if (level.count == 1) book.on_level_filled(order.price);
    };
    if (order.side == Side::Buy) put(bids_); else put(asks_);
}

bool OrderBook::cancel(OrderId id) {
    const NodeRef r = index_erase(id);  // one chain walk finds and unlinks it
    if (r == kNil) return false;
    const Node& n = nodes_[r];

    auto remove = [&](auto& book) {
        Level& level = book.at(n.price);
        unlink(level, r);
        if (level.empty()) book.on_level_emptied(n.price);
    };
    if (n.side == Side::Buy) remove(bids_); else remove(asks_);

    free_node(r);
    return true;
}

bool OrderBook::modify(OrderId id, Price new_price, Quantity new_qty) {
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

    // Anything else loses priority: cancel, then re-add as a fresh Limit order.
    const Side side = n.side;    // copied out: cancel() frees the node
    check_band(side, new_price);  // throw now, while the order is still resting untouched
    cancel(id);
    Order replacement;
    replacement.id = id;
    replacement.side = side;
    replacement.type = OrderType::Limit;
    replacement.price = new_price;
    replacement.qty = new_qty;
    add(replacement);
    return true;
}

}  // namespace ob
