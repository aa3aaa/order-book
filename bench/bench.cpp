// Latency benchmark. Run on the Release build, on mains power, machine otherwise idle:
//
//   ./build/release/bench                 # 1,000,000 timed operations, 3 runs
//   ./build/release/bench 5000000 5       # choose the op count and the number of runs
//
// Method:
//   1. Every operation is generated up front from a seeded RNG; generation is never timed.
//   2. 100,000 warm-up operations run first so the book has depth and the caches are warm.
//   3. Each timed operation is bracketed by steady_clock::now(). The clock's tick and the
//      cost of one now() call are measured first and printed, because both limit what the
//      per-op numbers can resolve.
//   4. Percentiles are read from the sorted samples. The mean is the sum of the samples
//      over their count: each sample is rounded to a whole tick, but where an operation
//      starts relative to the next tick is effectively random, so the rounding averages
//      out and the mean is accurate well below one tick. "mean - overhead" subtracts the
//      measured cost of the clock calls themselves.
//   5. Throughput comes from a second, untimed pass over the same operations on a fresh book.
//   6. The whole measurement repeats `runs` times on fresh books; the full table is
//      printed for the median run by throughput.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
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
    std::size_t count{};
    std::int64_t p50{}, p99{}, p999{}, max{};
    double mean{};
};

Summary summarise(std::vector<std::int64_t>& ns) {
    if (ns.empty()) return {};
    std::sort(ns.begin(), ns.end());
    auto at = [&](double q) { return ns[std::min(ns.size() - 1, static_cast<std::size_t>(q * ns.size()))]; };
    long double sum = 0;
    for (auto v : ns) sum += v;
    return {ns.size(), at(0.50), at(0.99), at(0.999), ns.back(), static_cast<double>(sum / ns.size())};
}

// The clock's resolution (smallest non-zero step) and the mean cost of one
// back-to-back now() pair, which is what every timed sample also pays.
struct ClockInfo {
    std::int64_t tick_ns;
    double overhead_ns;
};

ClockInfo calibrate_clock() {
    constexpr int kSamples = 2'000'000;
    std::int64_t tick = std::numeric_limits<std::int64_t>::max();
    long double sum = 0;
    for (int i = 0; i < kSamples; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        const auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        sum += d;
        if (d > 0) tick = std::min(tick, d);
    }
    return {tick, static_cast<double>(sum / kSamples)};
}

// The book peaks at about 110k resting orders on this workload; reserve headroom.
// Set OB_BENCH_NO_RESERVE=1 to measure without it.
const std::size_t kReserveOrders = std::getenv("OB_BENCH_NO_RESERVE") ? 0 : 1u << 18;

struct RunResult {
    double mops{};
    std::uint64_t trades{};
    std::size_t resting_after_warmup{}, resting_at_end{};
    Summary add_rest, add_trade, cancel, modify;
};

RunResult run_once(const std::vector<Op>& warmup, const std::vector<Op>& ops) {
    RunResult r;

    std::uint64_t trades = 0;  // printed at the end so the optimiser cannot drop the work
    ob::OrderBook book([&trades](const ob::Trade&) { ++trades; });
    book.reserve(kReserveOrders);
    for (const Op& op : warmup) run_op(book, op);
    r.resting_after_warmup = book.order_count();

    // Timed pass: one latency sample per operation, split by kind. Adds are split
    // further into those that rested without trading and those that traded.
    std::vector<std::int64_t> add_rest, add_trade, cancel_ns, modify_ns;
    add_rest.reserve(ops.size());
    add_trade.reserve(ops.size());
    cancel_ns.reserve(ops.size() / 4);
    modify_ns.reserve(ops.size() / 8);

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
    r.resting_at_end = book.order_count();

    // Untimed pass for throughput: same operations, fresh book, same warm-up.
    std::uint64_t trades2 = 0;
    ob::OrderBook book2([&trades2](const ob::Trade&) { ++trades2; });
    book2.reserve(kReserveOrders);
    for (const Op& op : warmup) run_op(book2, op);
    const auto w0 = Clock::now();
    for (const Op& op : ops) run_op(book2, op);
    const auto w1 = Clock::now();
    r.mops = ops.size() / std::chrono::duration<double>(w1 - w0).count() / 1e6;

    if (trades != trades2) {  // same ops on the same kind of book must produce the same trades
        std::fprintf(stderr, "trade count mismatch: %llu vs %llu\n",
                     static_cast<unsigned long long>(trades), static_cast<unsigned long long>(trades2));
        std::exit(1);
    }
    r.trades = trades;
    r.add_rest = summarise(add_rest);
    r.add_trade = summarise(add_trade);
    r.cancel = summarise(cancel_ns);
    r.modify = summarise(modify_ns);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t timed_ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    const std::size_t runs = std::max<std::size_t>(1, argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3);
    constexpr std::size_t warmup_ops = 100'000;

    const auto warmup = generate(warmup_ops, 1, 1);
    // OB_BENCH_SEED changes the timed stream; the PGO script trains on a different
    // seed from the one it measures, so the profile is not fitted to the exact ops.
    const std::uint64_t seed = std::getenv("OB_BENCH_SEED") ? std::strtoull(std::getenv("OB_BENCH_SEED"), nullptr, 10) : 2;
    const auto ops = generate(timed_ops, seed, warmup_ops + 1);
    const ClockInfo clock = calibrate_clock();

    std::printf("order book benchmark  (compiler: %s)\n", __VERSION__);
    std::printf("clock: tick %lld ns, one now() pair costs %.1f ns on average\n",
                static_cast<long long>(clock.tick_ns), clock.overhead_ns);

    std::vector<RunResult> results;
    for (std::size_t i = 0; i < runs; ++i) {
        results.push_back(run_once(warmup, ops));
        const RunResult& r = results.back();
        std::printf("run %zu: %.2f M ops/s, add p99 %lld ns, add p99.9 %lld ns, worst op %lld ns\n", i + 1, r.mops,
                    static_cast<long long>(r.add_rest.p99), static_cast<long long>(r.add_rest.p999),
                    static_cast<long long>(std::max({r.add_rest.max, r.add_trade.max, r.cancel.max, r.modify.max})));
    }

    std::vector<std::size_t> order(results.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return results[a].mops < results[b].mops; });
    const std::size_t median = order[order.size() / 2];
    const RunResult& r = results[median];

    std::printf("\nmedian run (run %zu of %zu by throughput)\n", median + 1, runs);
    std::printf("%zu timed ops after %zu warm-up ops; %zu orders resting after warm-up, %zu at end\n",
                timed_ops, warmup_ops, r.resting_after_warmup, r.resting_at_end);
    std::printf("throughput (untimed pass): %.2f M ops/s  (%.1f ns per op)\n\n", r.mops, 1e3 / r.mops);

    std::printf("| operation | count | mean (ns) | mean - overhead (ns) | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |\n");
    std::printf("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
    auto row = [&](const char* name, const Summary& s) {
        std::printf("| %s | %zu | %.1f | %.1f | %lld | %lld | %lld | %lld |\n", name, s.count, s.mean,
                    std::max(0.0, s.mean - clock.overhead_ns), static_cast<long long>(s.p50),
                    static_cast<long long>(s.p99), static_cast<long long>(s.p999), static_cast<long long>(s.max));
    };
    row("add, rests (no trade)", r.add_rest);
    row("add, trades", r.add_trade);
    row("cancel", r.cancel);
    row("modify", r.modify);
    std::printf("\ntrades: %llu\n", static_cast<unsigned long long>(r.trades));
    return 0;
}
