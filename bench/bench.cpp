// Latency benchmark. Run on the Release build, on mains power, machine otherwise idle:
//
//   ./build/release/bench            # 1,000,000 timed operations
//   ./build/release/bench 5000000    # or choose the count
//
// Method:
//   1. Every operation is generated up front from a seeded RNG; generation is never timed.
//   2. 100,000 warm-up operations run first so the book has depth and the caches are warm.
//   3. Each timed operation is bracketed by steady_clock::now(); the ~20-40 ns cost of
//      the clock call is inside every figure, so absolute numbers are conservative.
//   4. Throughput comes from a second, untimed pass over the same operations on a fresh book.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "ob/order_book.hpp"

namespace {

using Clock = std::chrono::steady_clock;

enum class OpKind : std::uint8_t { Add, Cancel, Modify };

struct Op {
    OpKind kind{};
    ob::Order order{};      // Add: the order. Cancel / Modify: order.id is the target
    ob::Price new_price{};  // Modify only
    ob::Quantity new_qty{}; // Modify only
};

struct Generated {
    ob::Side side;
    ob::Price price;
    ob::Quantity qty;
    ob::OrderId id;
};

// 70% adds (price ~ Normal(10000, 30) ticks, qty 1-100), 20% cancels, 10% modifies.
// Cancels and modifies target one of the last 1024 ids added; some of those will
// already have been filled, exactly as in a real feed.
std::vector<Op> generate(std::size_t count, std::uint64_t seed, ob::OrderId first_id) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> percent(1, 100);
    std::uniform_int_distribution<int> coin(0, 1);
    std::normal_distribution<double> price_dist(10'000.0, 30.0);
    std::uniform_int_distribution<ob::Quantity> qty_dist(1, 100);

    std::vector<Op> ops;
    ops.reserve(count);
    std::deque<Generated> recent;
    ob::OrderId next_id = first_id;

    for (std::size_t i = 0; i < count; ++i) {
        const int roll = percent(rng);
        Op op;
        if (roll <= 70 || recent.empty()) {
            op.kind = OpKind::Add;
            op.order.id = next_id++;
            op.order.side = coin(rng) ? ob::Side::Buy : ob::Side::Sell;
            op.order.type = ob::OrderType::Limit;
            op.order.price = static_cast<ob::Price>(std::llround(price_dist(rng)));
            op.order.qty = qty_dist(rng);
            recent.push_back({op.order.side, op.order.price, op.order.qty, op.order.id});
            if (recent.size() > 1024) recent.pop_front();
        } else {
            const auto& target = recent[std::uniform_int_distribution<std::size_t>(0, recent.size() - 1)(rng)];
            op.order.id = target.id;
            if (roll <= 90) {
                op.kind = OpKind::Cancel;
            } else {
                op.kind = OpKind::Modify;
                if (coin(rng)) {  // half: same price, smaller size (keeps priority)
                    op.new_price = target.price;
                    op.new_qty = std::max<ob::Quantity>(1, target.qty / 2);
                } else {          // half: new price and size (re-queues, may trade)
                    op.new_price = static_cast<ob::Price>(std::llround(price_dist(rng)));
                    op.new_qty = qty_dist(rng);
                }
            }
        }
        ops.push_back(op);
    }
    return ops;
}

inline void run_op(ob::OrderBook& book, const Op& op) {
    switch (op.kind) {
        case OpKind::Add: book.add(op.order); break;
        case OpKind::Cancel: book.cancel(op.order.id); break;
        case OpKind::Modify: book.modify(op.order.id, op.new_price, op.new_qty); break;
    }
}

struct Summary {
    std::size_t count;
    std::int64_t p50, p99, p999, max;
};

Summary summarise(std::vector<std::int64_t>& ns) {
    if (ns.empty()) return {0, 0, 0, 0, 0};
    std::sort(ns.begin(), ns.end());
    auto at = [&](double q) { return ns[std::min(ns.size() - 1, static_cast<std::size_t>(q * ns.size()))]; };
    return {ns.size(), at(0.50), at(0.99), at(0.999), ns.back()};
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t timed_ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    constexpr std::size_t warmup_ops = 100'000;

    const auto warmup = generate(warmup_ops, 1, 1);
    const auto ops = generate(timed_ops, 2, warmup_ops + 1);

    std::uint64_t trades = 0;  // printed at the end so the optimiser cannot drop the work
    ob::OrderBook book([&trades](const ob::Trade&) { ++trades; });
    for (const Op& op : warmup) run_op(book, op);
    const std::size_t resting_after_warmup = book.order_count();

    // Timed pass: one latency sample per operation, split by kind. Adds are split
    // further into those that rested without trading and those that traded.
    std::vector<std::int64_t> add_rest, add_trade, cancel_ns, modify_ns;
    add_rest.reserve(timed_ops);
    add_trade.reserve(timed_ops);
    cancel_ns.reserve(timed_ops / 4);
    modify_ns.reserve(timed_ops / 8);

    for (const Op& op : ops) {
        const std::uint64_t trades_before = trades;
        const auto t0 = Clock::now();
        run_op(book, op);
        const auto t1 = Clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        switch (op.kind) {
            case OpKind::Add: (trades == trades_before ? add_rest : add_trade).push_back(ns); break;
            case OpKind::Cancel: cancel_ns.push_back(ns); break;
            case OpKind::Modify: modify_ns.push_back(ns); break;
        }
    }
    const std::size_t resting_at_end = book.order_count();

    // Untimed pass for throughput: same operations, fresh book, same warm-up.
    std::uint64_t trades2 = 0;
    ob::OrderBook book2([&trades2](const ob::Trade&) { ++trades2; });
    for (const Op& op : warmup) run_op(book2, op);
    const auto w0 = Clock::now();
    for (const Op& op : ops) run_op(book2, op);
    const auto w1 = Clock::now();
    const double wall_s = std::chrono::duration<double>(w1 - w0).count();

    std::printf("order book benchmark  (pool allocator: %s, compiler: %s)\n",
                OB_USE_POOL ? "on" : "off", __VERSION__);
    std::printf("%zu timed ops after %zu warm-up ops; %zu orders resting after warm-up, %zu at end\n",
                timed_ops, warmup_ops, resting_after_warmup, resting_at_end);
    std::printf("throughput (untimed pass): %.2f M ops/s\n\n", timed_ops / wall_s / 1e6);

    std::printf("| operation | count | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |\n");
    std::printf("| --- | ---: | ---: | ---: | ---: | ---: |\n");
    auto row = [](const char* name, std::vector<std::int64_t>& v) {
        const Summary s = summarise(v);
        std::printf("| %s | %zu | %lld | %lld | %lld | %lld |\n", name, s.count,
                    static_cast<long long>(s.p50), static_cast<long long>(s.p99),
                    static_cast<long long>(s.p999), static_cast<long long>(s.max));
    };
    row("add, rests (no trade)", add_rest);
    row("add, trades", add_trade);
    row("cancel", cancel_ns);
    row("modify", modify_ns);

    std::printf("\ntrades: %llu (timed pass), %llu (untimed pass)\n",
                static_cast<unsigned long long>(trades), static_cast<unsigned long long>(trades2));
    return trades == trades2 ? 0 : 1;  // same ops on the same book must produce the same trades
}
