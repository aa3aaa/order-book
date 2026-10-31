#pragma once
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <vector>
#include "ob/types.hpp"

namespace ob {

// A deliberately naive order book: one vector of every resting order, linear
// scans for everything. Same public interface and the same six matching rules
// as OrderBook, written in a different shape so the two can be compared.
// Never optimise this; its only job is to be obviously correct.
class ReferenceBook {
    public:
        using TradeCallback = std::function<void(const Trade&)>;

        explicit ReferenceBook(TradeCallback on_trade = {}) : on_trade_(std::move(on_trade)) {}

        Quantity add(Order order) {
            if (order.qty <= 0) return 0;
            order.seq = next_seq_++;
            const Side opposite = order.side == Side::Buy ? Side::Sell : Side::Buy;

            while (order.qty > 0) {
                auto maker = best(opposite);
                if (maker == orders_.end()) break;
                if (order.type != OrderType::Market && !crosses(order.side, order.price, maker->price)) break;

                const Quantity fill = std::min(order.qty, maker->qty);
                if (on_trade_) on_trade_(Trade{order.id, maker->id, maker->price, fill});
                order.qty -= fill;
                maker->qty -= fill;
                if (maker->qty == 0) orders_.erase(maker);
            }

            if (order.qty > 0 && order.type == OrderType::Limit) {
                orders_.push_back(order);
                return order.qty;
            }
            return 0;
        }

        bool cancel(OrderId id) {
            auto it = find_it(id);
            if (it == orders_.end()) return false;
            orders_.erase(it);
            return true;
        }

        bool modify(OrderId id, Price new_price, Quantity new_qty) {
            if (new_qty <= 0) return false;
            auto it = find_it(id);
            if (it == orders_.end()) return false;
            if (new_price == it->price && new_qty <= it->qty) {
                it->qty = new_qty;  // keeps its sequence number, so keeps priority
                return true;
            }
            Order replacement;
            replacement.id = id;
            replacement.side = it->side;
            replacement.type = OrderType::Limit;
            replacement.price = new_price;
            replacement.qty = new_qty;
            orders_.erase(it);
            add(replacement);
            return true;
        }

        std::optional<LevelView> best_bid() const { return first(depth(Side::Buy, 1)); }
        std::optional<LevelView> best_ask() const { return first(depth(Side::Sell, 1)); }

        std::vector<LevelView> depth(Side side, std::size_t n) const {
            // Group by price, best first, then take the first n.
            std::map<Price, LevelView> by_price;
            for (const Order& o : orders_) {
                if (o.side != side) continue;
                LevelView& level = by_price[o.price];
                level.price = o.price;
                level.qty += o.qty;
                level.orders += 1;
            }
            std::vector<LevelView> out;
            for (const auto& [price, level] : by_price) out.push_back(level);
            if (side == Side::Buy) std::reverse(out.begin(), out.end());  // bids: highest first
            if (out.size() > n) out.resize(n);
            return out;
        }

        std::size_t order_count() const noexcept { return orders_.size(); }

        // Test helper: the resting order with this id, if any.
        const Order* find(OrderId id) const {
            auto it = std::find_if(orders_.begin(), orders_.end(), [id](const Order& o) { return o.id == id; });
            return it == orders_.end() ? nullptr : &*it;
        }

    private:
        std::vector<Order>::iterator find_it(OrderId id) {
            return std::find_if(orders_.begin(), orders_.end(), [id](const Order& o) { return o.id == id; });
        }

        // Best resting order on one side: best price, ties broken by the lowest sequence.
        std::vector<Order>::iterator best(Side side) {
            auto best_it = orders_.end();
            for (auto it = orders_.begin(); it != orders_.end(); ++it) {
                if (it->side != side) continue;
                if (best_it == orders_.end()) { best_it = it; continue; }
                const bool better_price = side == Side::Buy ? it->price > best_it->price
                                                            : it->price < best_it->price;
                const bool same_price_older = it->price == best_it->price && it->seq < best_it->seq;
                if (better_price || same_price_older) best_it = it;
            }
            return best_it;
        }

        static std::optional<LevelView> first(const std::vector<LevelView>& levels) {
            if (levels.empty()) return std::nullopt;
            return levels.front();
        }

        std::vector<Order> orders_;
        TradeCallback on_trade_;
        Sequence next_seq_{1};
};

}  // namespace ob
