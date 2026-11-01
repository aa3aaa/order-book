#include "ob/order_book.hpp"

#include <algorithm>
#include <iterator>
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
    return LevelView{book.best_price(), level.total, level.orders.size()};
}

template <class Book>
std::vector<LevelView> levels_of(const Book& book, std::size_t n) {
    std::vector<LevelView> out;
    out.reserve(std::min(n, book.level_count()));
    if (n == 0) return out;
    book.for_each([&](Price price, const auto& level) {
        out.push_back(LevelView{price, level.total, level.orders.size()});
        return out.size() < n;
    });
    return out;
}

}  // namespace

OrderBook::OrderBook(TradeCallback on_trade) : on_trade_(std::move(on_trade)) {}

void OrderBook::reserve(std::size_t max_resting_orders) {
    index_.reserve(max_resting_orders);
#if OB_USE_POOL
    // Allocate that many nodes of each kind and free them again: the pools keep
    // the memory, so later adds pop pre-grown blocks instead of growing mid-run.
    // (The temporaries share the pools with the real containers: the allocator is stateless.)
    {
        OrderList orders;
        for (std::size_t i = 0; i < max_resting_orders; ++i) orders.emplace_back();
    }
    {
        Index dummy;
        dummy.reserve(max_resting_orders);
        for (std::size_t i = 0; i < max_resting_orders; ++i) dummy.emplace(i, Handle{});
    }
#endif
}

std::optional<LevelView> OrderBook::best_bid() const { return top_of(bids_); }

std::optional<LevelView> OrderBook::best_ask() const { return top_of(asks_); }

std::vector<LevelView> OrderBook::depth(Side side, std::size_t n) const {
    return side == Side::Buy ? levels_of(bids_, n) : levels_of(asks_, n);
}

void OrderBook::check_band(Side side, Price price) const {
    const bool ok = side == Side::Buy ? bids_.can_hold(price) : asks_.can_hold(price);
    if (!ok) throw std::length_error("order price is too far from this side's resting prices");
}

Quantity OrderBook::add(Order order) {
    if (order.qty <= 0) return 0;
    if (order.type == OrderType::Limit) check_band(order.side, order.price);  // before anything changes
    order.seq = next_seq_++;  // the book stamps the sequence, never the caller

    if (order.side == Side::Buy) match(order, asks_); else match(order, bids_);

    if (order.qty > 0 && order.type == OrderType::Limit) {
        rest(order);
        return order.qty;
    }
    return 0;  // filled, or a Market / IOC remainder that is cancelled
}

// Defined here and only called from add() in this file, so the compiler
// instantiates it for both Bids and Asks without an explicit declaration.
template <class Book>
void OrderBook::match(Order& taker, Book& book) {
    while (taker.qty > 0 && !book.empty()) {
        const Price level_price = book.best_price();  // re-read every iteration: it moves as levels empty
        if (taker.type != OrderType::Market && !crosses(taker.side, taker.price, level_price)) break;

        auto& level = book.best_level();
        Order& maker = level.orders.front();  // oldest order at the best price
        const Quantity fill = std::min(taker.qty, maker.qty);

        if (on_trade_) on_trade_(Trade{taker.id, maker.id, maker.price, fill});

        taker.qty -= fill;
        maker.qty -= fill;
        level.total -= fill;

        if (maker.qty == 0) {
            index_.erase(maker.id);    // before pop_front: the index holds this node's iterator
            level.orders.pop_front();  // `maker` dangles from here on
            if (level.orders.empty()) book.on_level_emptied(level_price);
        }
    }
}

void OrderBook::rest(Order order) {
    auto put = [&](auto& book) {
        auto& level = book.get_or_create(order.price);  // add() already checked the band
        const bool was_empty = level.orders.empty();
        level.total += order.qty;
        level.orders.push_back(order);
        index_[order.id] = Handle{order.side, order.price, std::prev(level.orders.end())};
        if (was_empty) book.on_level_filled(order.price);
    };
    if (order.side == Side::Buy) put(bids_); else put(asks_);
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;
    const Handle handle = found->second;  // copy out before changing any container

    auto remove = [&](auto& book) {
        auto& level = book.at(handle.price);
        level.total -= handle.it->qty;
        level.orders.erase(handle.it);
        if (level.orders.empty()) book.on_level_emptied(handle.price);
    };
    if (handle.side == Side::Buy) remove(bids_); else remove(asks_);

    index_.erase(found);  // last: the handle is no longer needed
    return true;
}

bool OrderBook::modify(OrderId id, Price new_price, Quantity new_qty) {
    if (new_qty <= 0) return false;
    auto found = index_.find(id);
    if (found == index_.end()) return false;
    const Handle handle = found->second;

    // Same price, quantity not going up: adjust in place and keep queue priority.
    if (new_price == handle.price && new_qty <= handle.it->qty) {
        const Quantity reduce_by = handle.it->qty - new_qty;
        auto shrink = [&](auto& book) { book.at(handle.price).total -= reduce_by; };
        if (handle.side == Side::Buy) shrink(bids_); else shrink(asks_);
        handle.it->qty = new_qty;
        return true;
    }

    // Anything else loses priority: cancel, then re-add as a fresh Limit order.
    const Side side = handle.side;  // copied out: cancel() invalidates the handle
    check_band(side, new_price);    // throw now, while the order is still resting untouched
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
