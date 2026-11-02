#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ob/types.hpp"

namespace ob {

// One side of the book as a tick-indexed array: levels_[i] holds the level at
// price base_ + i. Finding the level for a price is an array index (O(1)), and
// the best price is cached, so the matching loop never searches a tree.
//
//  - The best price moves only when its level empties; the next best is found by
//    stepping one tick at a time toward worse prices. That walk is as long as the
//    gap to the next occupied level, which is short in a liquid book.
//  - The array grows (doubling, re-centred on the occupied prices) when a price
//    lands outside it. A Level holds only indices into the order slab, so growth
//    copies levels across and nothing that refers to an order has to change.
//  - Empty regions are dropped when the array is rebuilt, but a side's occupied
//    prices must fit within kMaxSpan ticks. OrderBook checks can_hold() before it
//    changes anything, so a price outside that band is rejected cleanly.
//
// Level must be copyable and have `bool empty() const`.
template <class Level, bool IsBid>
class PriceLadder {
    public:
        static constexpr std::size_t kMaxSpan = std::size_t{1} << 22;   // ~4.2M ticks; $41,943 at cent ticks
        static constexpr std::size_t kInitialSize = 1024;

        bool empty() const noexcept { return occupied_ == 0; }
        std::size_t level_count() const noexcept { return occupied_; }

        // Preconditions for the next three: !empty().
        Price best_price() const noexcept { return best_; }
        Level& best_level() noexcept { return slot(best_); }
        const Level& best_level() const noexcept { return slot(best_); }

        // The level at a price that is known to be in range (a resting order's price).
        Level& at(Price price) noexcept { return slot(price); }

        // True if a level at this price can exist without this side's occupied
        // prices spanning more than kMaxSpan ticks.
        bool can_hold(Price price) const noexcept {
            if (empty() || in_range(price)) return true;
            const auto [lo, hi] = occupied_bounds();
            return span(std::min(lo, price), std::max(hi, price)) <= kMaxSpan;
        }

        // The level at this price, growing the array if needed. Precondition: can_hold(price).
        Level& get_or_create(Price price) {
            if (!in_range(price)) rebuild_around(price);
            return slot(price);
        }

        // Call after a level goes from no orders to some.
        void on_level_filled(Price price) noexcept {
            if (occupied_++ == 0 || better(price, best_)) best_ = price;
        }

        // Call after a level loses its last order.
        void on_level_emptied(Price price) noexcept {
            if (--occupied_ == 0 || price != best_) return;
            Price p = best_;
            do { p = worse_by_one(p); } while (slot(p).empty());
            best_ = p;
        }

        // Calls f(price, level) for each non-empty level, best first, until f returns false.
        template <class F>
        void for_each(F&& f) const {
            std::size_t seen = 0;
            for (Price p = best_; seen < occupied_; p = worse_by_one(p)) {
                const Level& level = slot(p);
                if (level.empty()) continue;
                ++seen;
                if (!f(p, level)) return;
            }
        }

    private:
        static constexpr bool better(Price a, Price b) noexcept { return IsBid ? a > b : a < b; }
        static constexpr Price worse_by_one(Price p) noexcept { return IsBid ? p - 1 : p + 1; }

        // Number of ticks in [lo, hi], computed without signed overflow.
        static std::size_t span(Price lo, Price hi) noexcept {
            return static_cast<std::size_t>(static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo)) + 1;
        }

        bool in_range(Price price) const noexcept {
            return !levels_.empty() && price >= base_ &&
                   static_cast<std::uint64_t>(price) - static_cast<std::uint64_t>(base_) < levels_.size();
        }
        Level& slot(Price price) noexcept { return levels_[static_cast<std::size_t>(price - base_)]; }
        const Level& slot(Price price) const noexcept { return levels_[static_cast<std::size_t>(price - base_)]; }

        // Lowest and highest occupied prices. O(array size): only used on the rare growth path.
        std::pair<Price, Price> occupied_bounds() const noexcept {
            std::size_t lo = 0, hi = levels_.size() - 1;
            while (levels_[lo].empty()) ++lo;
            while (levels_[hi].empty()) --hi;
            return {base_ + static_cast<Price>(lo), base_ + static_cast<Price>(hi)};
        }

        // Builds a new array covering the occupied prices and `price`, with slack on
        // both sides, and copies every occupied level across.
        void rebuild_around(Price price) {
            Price lo = price, hi = price;
            if (!empty()) {
                const auto bounds = occupied_bounds();
                lo = std::min(lo, bounds.first);
                hi = std::max(hi, bounds.second);
            }
            const std::size_t need = span(lo, hi);
            const std::size_t size = std::min(kMaxSpan, std::max({need * 2, levels_.size() * 2, kInitialSize}));
            const Price new_base = lo - static_cast<Price>((size - need) / 2);

            std::vector<Level> fresh(size);
            if (!empty()) {
                for (Price p = lo; p <= hi; ++p) {
                    if (!in_range(p)) continue;  // `price` itself may be outside the old array
                    const Level& from = slot(p);
                    if (from.empty()) continue;
                    fresh[static_cast<std::size_t>(p - new_base)] = from;
                }
            }
            levels_.swap(fresh);
            base_ = new_base;
        }

        std::vector<Level> levels_;
        Price base_{0};
        Price best_{0};
        std::size_t occupied_{0};  // number of non-empty levels
};

}  // namespace ob
