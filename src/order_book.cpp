#include "ob/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace ob {

namespace {

// Bids and Asks are different map types, so the read-only helpers are
// templates: one body serves both sides.
template <class Book>
std::optional<LevelView> top_of(const Book& book) {
    if (book.empty()) return std::nullopt;
    const auto& [price, level] = *book.begin();  // begin() is the best level
    return LevelView{price, level.total, level.orders.size()};
}

template <class Book>
std::vector<LevelView> levels_of(const Book& book, std::size_t n) {
    std::vector<LevelView> out;
    out.reserve(std::min(n, book.size()));
    for (const auto& [price, level] : book) {
        if (out.size() == n) break;
        out.push_back(LevelView{price, level.total, level.orders.size()});
    }
    return out;
}

}  // namespace

OrderBook::OrderBook(TradeCallback on_trade) : on_trade_(std::move(on_trade)) {}

std::optional<LevelView> OrderBook::best_bid() const { return top_of(bids_); }

std::optional<LevelView> OrderBook::best_ask() const { return top_of(asks_); }

std::vector<LevelView> OrderBook::depth(Side side, std::size_t n) const {
    return side == Side::Buy ? levels_of(bids_, n) : levels_of(asks_, n);
}

Quantity OrderBook::add(Order order) {
    if (order.qty <= 0) return 0;
    order.seq = next_seq_++;
    rest(order);
    return order.qty;
}

void OrderBook::rest(Order order) {
    auto put = [&](auto& book) {
        auto& level = book[order.price];  // operator[] creates the level if new: intended here
        level.total += order.qty;
        level.orders.push_back(order);
        index_[order.id] = Handle{order.side, order.price, std::prev(level.orders.end())};
    };
    if (order.side == Side::Buy) put(bids_); else put(asks_);
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;
    const Handle handle = found->second;  // copy out before changing any container

    auto remove = [&](auto& book) {
        auto level_it = book.find(handle.price);
        auto& level = level_it->second;
        level.total -= handle.it->qty;
        level.orders.erase(handle.it);
        if (level.orders.empty()) book.erase(level_it);
    };
    if (handle.side == Side::Buy) remove(bids_); else remove(asks_);

    index_.erase(found);  // last: the handle is no longer needed
    return true;
}

}  // namespace ob
