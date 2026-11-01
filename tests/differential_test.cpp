// Differential test: drive the real book and the naive reference book with the
// same seeded random stream and fail the moment they disagree on anything.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "ob/order_book.hpp"
#include "reference_book.hpp"

namespace {

std::uint64_t env_or(const char* name, std::uint64_t fallback) {
    const char* value = std::getenv(name);
    return value ? std::strtoull(value, nullptr, 10) : fallback;
}

const char* side_name(ob::Side s) { return s == ob::Side::Buy ? "B" : "S"; }

const char* type_name(ob::OrderType t) {
    switch (t) {
        case ob::OrderType::Limit: return "limit";
        case ob::OrderType::Market: return "market";
        case ob::OrderType::IOC: return "ioc";
    }
    return "?";
}

}  // namespace

TEST(DifferentialTest, MatchesReferenceBookOnRandomStream) {
    // OB_DIFF_OPS is lowered for the sanitiser build (see tests/CMakeLists.txt);
    // OB_DIFF_SEED lets you explore other streams. A failure prints both.
    const std::uint64_t ops = env_or("OB_DIFF_OPS", 100'000);
    const std::uint64_t seed = env_or("OB_DIFF_SEED", 20261019);
    std::mt19937_64 rng(seed);

    std::vector<ob::Trade> real_trades, ref_trades;
    ob::OrderBook real([&](const ob::Trade& t) { real_trades.push_back(t); });
    ob::ReferenceBook ref([&](const ob::Trade& t) { ref_trades.push_back(t); });

    auto uniform = [&](std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };
    auto percent = [&] { return uniform(1, 100); };

    std::deque<ob::OrderId> recent;  // the last 64 ids added; some will be filled already
    ob::OrderId next_id = 1;
    std::uint64_t trades_seen = 0, in_place_modifies = 0;

    for (std::uint64_t i = 0; i < ops; ++i) {
        std::ostringstream what;
        const auto roll = percent();

        if (roll <= 65 || recent.empty()) {
            // add: narrow price range so the sides cross constantly
            ob::Order o;
            o.id = next_id++;
            o.side = uniform(0, 1) ? ob::Side::Buy : ob::Side::Sell;
            const auto kind = percent();
            o.type = kind <= 80 ? ob::OrderType::Limit : kind <= 90 ? ob::OrderType::Market : ob::OrderType::IOC;
            o.price = uniform(95, 105);
            o.qty = uniform(1, 20);
            what << "add id=" << o.id << " " << side_name(o.side) << " " << type_name(o.type)
                 << " px=" << o.price << " qty=" << o.qty;

            ASSERT_EQ(real.add(o), ref.add(o)) << "op " << i << ": " << what.str();
            recent.push_back(o.id);
            if (recent.size() > 64) recent.pop_front();
        } else if (roll <= 85) {
            const ob::OrderId id = recent[uniform(0, static_cast<std::int64_t>(recent.size()) - 1)];
            what << "cancel id=" << id;
            ASSERT_EQ(real.cancel(id), ref.cancel(id)) << "op " << i << ": " << what.str();
        } else {
            const ob::OrderId id = recent[uniform(0, static_cast<std::int64_t>(recent.size()) - 1)];
            ob::Price px = uniform(95, 105);
            ob::Quantity qty = uniform(-1, 20);  // occasionally <= 0 to hit the reject path
            // Bias toward the keep-priority path (same price, smaller qty) so it is well covered.
            if (const ob::Order* resting = ref.find(id); resting && percent() <= 40) {
                px = resting->price;
                qty = uniform(1, resting->qty);
                ++in_place_modifies;
            }
            what << "modify id=" << id << " px=" << px << " qty=" << qty;
            ASSERT_EQ(real.modify(id, px, qty), ref.modify(id, px, qty)) << "op " << i << ": " << what.str();
        }

        const std::string where = "op " + std::to_string(i) + ": " + what.str() + " (seed " + std::to_string(seed) + ")";
        ASSERT_EQ(real_trades, ref_trades) << where;
        ASSERT_EQ(real.best_bid(), ref.best_bid()) << where;
        ASSERT_EQ(real.best_ask(), ref.best_ask()) << where;
        ASSERT_EQ(real.depth(ob::Side::Buy, 5), ref.depth(ob::Side::Buy, 5)) << where;
        ASSERT_EQ(real.depth(ob::Side::Sell, 5), ref.depth(ob::Side::Sell, 5)) << where;
        ASSERT_EQ(real.order_count(), ref.order_count()) << where;

        trades_seen += real_trades.size();
        real_trades.clear();
        ref_trades.clear();
    }

    // Guard against a stream that silently stopped exercising the matcher.
    EXPECT_GT(trades_seen, ops / 10);
    EXPECT_GT(in_place_modifies, ops / 500);
    std::cout << "[ differential ] " << ops << " ops, " << trades_seen << " trades, "
              << in_place_modifies << " in-place modifies, seed " << seed << "\n";
}

// Same comparison, but with prices that drift and occasionally jump far away, so
// the tick-indexed ladders keep growing, re-centring and skipping wide gaps.
TEST(DifferentialTest, MatchesReferenceBookOnWideDriftingPrices) {
    const std::uint64_t ops = std::min<std::uint64_t>(env_or("OB_DIFF_OPS", 100'000), 30'000);
    const std::uint64_t seed = env_or("OB_DIFF_SEED", 20261101);
    std::mt19937_64 rng(seed);

    std::vector<ob::Trade> real_trades, ref_trades;
    ob::OrderBook real([&](const ob::Trade& t) { real_trades.push_back(t); });
    ob::ReferenceBook ref([&](const ob::Trade& t) { ref_trades.push_back(t); });

    auto uniform = [&](std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };
    std::deque<ob::OrderId> recent;
    ob::OrderId next_id = 1;
    ob::Price mid = 50'000;

    for (std::uint64_t i = 0; i < ops; ++i) {
        mid += uniform(-20, 20);
        if (uniform(1, 1000) == 1) mid += uniform(-20'000, 20'000);  // rare jump: forces regrowth
        const auto roll = uniform(1, 100);
        std::ostringstream what;

        if (roll <= 60 || recent.empty()) {
            ob::Order o;
            o.id = next_id++;
            o.side = uniform(0, 1) ? ob::Side::Buy : ob::Side::Sell;
            o.type = uniform(1, 10) <= 8 ? ob::OrderType::Limit : ob::OrderType::IOC;
            o.price = mid + (uniform(1, 20) == 1 ? uniform(-3'000, 3'000) : uniform(-40, 40));
            o.qty = uniform(1, 20);
            what << "add id=" << o.id << " " << side_name(o.side) << " " << type_name(o.type)
                 << " px=" << o.price << " qty=" << o.qty;
            ASSERT_EQ(real.add(o), ref.add(o)) << "op " << i << ": " << what.str();
            recent.push_back(o.id);
            if (recent.size() > 256) recent.pop_front();
        } else if (roll <= 85) {
            const ob::OrderId id = recent[uniform(0, static_cast<std::int64_t>(recent.size()) - 1)];
            what << "cancel id=" << id;
            ASSERT_EQ(real.cancel(id), ref.cancel(id)) << "op " << i << ": " << what.str();
        } else {
            const ob::OrderId id = recent[uniform(0, static_cast<std::int64_t>(recent.size()) - 1)];
            const ob::Price px = mid + uniform(-40, 40);
            const ob::Quantity qty = uniform(1, 20);
            what << "modify id=" << id << " px=" << px << " qty=" << qty;
            ASSERT_EQ(real.modify(id, px, qty), ref.modify(id, px, qty)) << "op " << i << ": " << what.str();
        }

        const std::string where = "op " + std::to_string(i) + ": " + what.str() + " (seed " + std::to_string(seed) + ")";
        ASSERT_EQ(real_trades, ref_trades) << where;
        ASSERT_EQ(real.best_bid(), ref.best_bid()) << where;
        ASSERT_EQ(real.best_ask(), ref.best_ask()) << where;
        ASSERT_EQ(real.depth(ob::Side::Buy, 5), ref.depth(ob::Side::Buy, 5)) << where;
        ASSERT_EQ(real.depth(ob::Side::Sell, 5), ref.depth(ob::Side::Sell, 5)) << where;
        ASSERT_EQ(real.order_count(), ref.order_count()) << where;
        real_trades.clear();
        ref_trades.clear();
    }
}
