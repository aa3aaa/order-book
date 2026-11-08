# order-book

[![ci](https://github.com/aa3aaa/order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/aa3aaa/order-book/actions/workflows/ci.yml)

A limit order book and matching engine in C++20: price-time priority, limit / market / IOC orders, O(1) cancel and modify, tick-indexed price levels with an occupancy bitmap, and every resting order in one slab linked by 32-bit indices. It is checked by 43 GoogleTest unit tests and two differential tests against a naive reference book (100,000 operations in a narrow band, 30,000 with wide, drifting prices), and measured by a latency benchmark.

**Results** (MacBook Air, Apple Silicon, native macOS, Apple Clang 21): **54.9 M ops/s** with the default `-O3` build and **61.9 M ops/s** with profile-guided optimisation, 16–18 ns per operation. Worst single operation over a million: 2–27 µs. That is up from 44.7 M ops/s natively at the end of the first round of latency work, and 20.7 M ops/s (in a Linux VM) at its start. The full history, step by step, is [below](#benchmark).

## Using the order book

### Build and test

Requires a C++20 compiler (GCC 11+ or Apple Clang), CMake 3.21+, and internet access on the first configure (GoogleTest is downloaded into `build/`).

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure       # all tests

./build/release/tests/ob_tests --gtest_filter='BookTest.Cancel*'   # just some tests
OB_DIFF_SEED=7 ./build/release/tests/ob_tests --gtest_filter='Diff*'  # differential test, another seed

# AddressSanitizer + UBSan build: run before every commit
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DOB_SANITIZE=ON
cmake --build build/asan -j && ctest --test-dir build/asan --output-on-failure
```

### Try it interactively

Run `replay` with no arguments and type orders one per line; after each one it prints the top of book, and `P` prints the whole ladder.

```text
$ ./build/release/replay
> A,1,S,100,10
  best bid: -   best ask: 100 x 10   resting orders: 1
> A,2,B,99,5
  best bid: 99 x 5   best ask: 100 x 10   resting orders: 2
> M,3,B,4
TRADE taker=3 maker=1 price=100 qty=4
  best bid: 99 x 5   best ask: 100 x 6   resting orders: 2
> X,2,100,8
TRADE taker=2 maker=1 price=100 qty=6
  best bid: 100 x 2   best ask: -   resting orders: 1
```

What happened: a sell of 10 at 100 rests; a buy of 5 at 99 does not cross, so it rests too; a market buy of 4 trades at the maker's price (100); modifying the bid to 8 at 100 changes its price, so it is re-queued as a new order, crosses the remaining 6 on the ask, and rests the last 2.

| Line | Meaning |
| --- | --- |
| `A,id,side,price,qty` | limit order (side `B` or `S`) |
| `M,id,side,qty` | market order |
| `I,id,side,price,qty` | immediate-or-cancel |
| `C,id` | cancel |
| `X,id,price,qty` | modify |
| `P` | print the whole book |
| `?` | help (interactive mode); Ctrl-D quits |

The same format works from a file or a pipe: `./build/release/replay data/sample.csv` or `cat orders.csv | ./build/release/replay -`. Lines starting with `#` are comments. A bad line is reported with its line number and skipped. That makes the tool handy for debugging too: write a failing sequence from the differential test as a CSV and step through it.

### Use it in your own code

`examples/basic_usage.cpp` is a complete program (`cmake --build build/release --target example && ./build/release/example`). The core of it:

```cpp
#include "ob/order_book.hpp"

std::vector<ob::Trade> trades;
ob::OrderBook book([&](const ob::Trade& t) { trades.push_back(t); });  // every execution, in order
// or, for speed: ob::BasicOrderBook book([&](const ob::Trade& t) {...});  callback type deduced, call inlined
book.reserve(10'000);                       // optional: pre-size so nothing grows mid-trading

ob::Order o;                                // plain struct; prices are integer ticks
o.id = 1; o.side = ob::Side::Sell; o.type = ob::OrderType::Limit; o.price = 10'050; o.qty = 300;
ob::Quantity resting = book.add(o);         // matches, then rests the remainder; returns what rested

book.modify(1, 10'050, 200);                // same price, smaller: keeps queue priority
book.cancel(1);                             // false if not resting
auto bid = book.best_bid();                 // std::optional<LevelView>{price, qty, orders}
auto ladder = book.depth(ob::Side::Sell, 5);  // up to 5 levels, best first
```

To use it from another CMake project, add this repo as a subdirectory and link the `orderbook` target: `add_subdirectory(order-book)` then `target_link_libraries(your_app PRIVATE orderbook)`.

### Benchmark

```bash
./build/release/bench                # 1M timed ops, 3 runs, prints the median run
./build/release/bench 5000000 5      # choose the op count and number of runs
OB_BENCH_NO_RESERVE=1 ./build/release/bench   # without reserve(), to see the difference
```

Always run it from the Release build, on mains power, with the machine otherwise idle.

### Compiler settings

Three opt-in switches make the compiler do more. They are off by default, and CI builds and tests each one (native + LTO together, and the full PGO pipeline).

```bash
# tune for this CPU and optimise across files (lets bench inline calls into the library)
cmake -S . -B build/fast -DCMAKE_BUILD_TYPE=Release -DOB_NATIVE=ON -DOB_LTO=ON
cmake --build build/fast -j && ./build/fast/bench

# profile-guided optimisation: instrumented build, training run, optimised rebuild
scripts/pgo_build.sh && ./build/pgo/bench
```

| Option | What it does |
| --- | --- |
| `OB_NATIVE=ON` | `-march=native` (or `-mcpu=native` on Apple Clang): use every instruction this CPU has. The binary may not run on an older CPU |
| `OB_LTO=ON` | Link-time optimisation: the compiler sees the library and the program together, so it can inline across files |
| `OB_PGO=GENERATE` / `USE` | Profile-guided optimisation, driven by `scripts/pgo_build.sh`. The compiler lays out code and decides inlining from what a real run actually did |

The PGO script trains on the benchmark with a different seed from the one it measures (`OB_BENCH_SEED=3`), so the profile learns the shape of the work rather than the exact operations. It is still the same kind of synthetic workload, so treat the PGO number as an upper bound for this workload, not a general speed-up.

## Matching rules

1. A limit order matches against the opposite side while prices cross, best price first and oldest order first within a price (price-time priority). Each trade is for `min(taker remaining, maker remaining)` at the **maker's** price.
2. A maker whose quantity reaches zero is removed from its level and from the id index; an empty level is removed from the book.
3. Whatever remains of a limit order rests at its price with a fresh sequence number. Market and IOC remainders are cancelled.
4. A market order ignores its price and matches until filled or the opposite side is empty.
5. `modify` keeps queue priority only when the price is unchanged and the quantity does not go up (adjusted in place). A price change or a quantity increase removes the order and re-adds it as a new limit order, which may match immediately.
6. Self-trading is not prevented (see [Limitations](#limitations-and-next-steps)).

Input rules: `add` ignores orders with `qty <= 0`; `modify` returns false for an unknown id or `new_qty <= 0` and leaves the book unchanged; `cancel` returns false for an id that is not resting. Order ids must be unique among resting orders. A limit order whose price would make its side's resting prices span more than 2^22 ticks (~4.2M; $41,943 at cent ticks) is rejected with `std::length_error` before anything changes.

## Design decisions

**Prices are integer ticks** (cents), never `double`: comparisons are exact and an integer indexes an array. `Price` and `Quantity` are signed 64-bit so that a bug which subtracts too much shows up as a negative number an assert can catch, rather than an unsigned value silently wrapping to ~1.8 × 10^19. Ids and sequence numbers are unsigned. The book stamps the sequence number on arrival; the caller never sets it.

**Three structures, and every order lives in exactly one place:**

| Structure | Type | Why |
| --- | --- | --- |
| Price levels, one per side | `PriceLadder`: a `std::vector<Level>` indexed by `price - base`, a cached best price, and a bitmap of occupied prices | Finding a level is an array index and the best level is always at hand, so matching never searches a tree |
| Orders | One slab, `std::vector<Node>`; each 40-byte node holds the order and three 32-bit links | No per-order heap allocation, no pointers: a level is `{head, tail, count, total}` indices into the slab |
| Lookup by id | A bucket array of `uint32` chain heads, chained through the nodes themselves | O(1) cancel and modify; the index costs 4 bytes per bucket and no separate entries |

**The order slab** (`include/ob/order_book.hpp`). Each resting order is one `Node {id, price, qty, prev, next, hnext, side}`. `prev`/`next` link it into its price level's FIFO queue (time priority), and `hnext` links it into its id-index bucket, so one node is in two lists at once ("intrusive" lists). Links are 32-bit indices into the slab rather than pointers: they are half the size, and they stay valid when the vector grows. Freed nodes go on a free list threaded through `next`, so allocating an order is a pop and freeing it is a push. That is the pool allocator from the earlier version, specialised to one type. The node has no order type (a resting order is always a limit order) and no sequence number (its position in the queue *is* its time priority), which keeps it at 40 bytes. The previous design spent roughly 110 bytes per order across two heap nodes.

**The id index.** The bucket for an id is `id & mask`. Exchange order ids count up, so recent orders, the ones most likely to be cancelled or filled next, land in neighbouring buckets and stay in cache. There is at least one bucket per live order, so chains are almost always 0 or 1 long. Two designs were tried and rejected on the way, which is worth knowing:

- **Open addressing with a scrambling hash** (MurmurHash3's finaliser): no clustering, but it spread consecutive orders across the whole table. That lost the locality `std::unordered_map` gets by accident (libstdc++ hashes an integer to itself), and it was about 2× slower in the book benchmark.
- **Open addressing with the order-preserving hash**: consecutive ids then form one long unbroken run of slots, and backward-shift deletion has to scan to the end of the run on every erase. A test with a million consecutive ids hung. That test now runs against the book (`ManyConsecutiveIdsStayFast`).

Chaining keeps the locality without the runs: deleting only touches one short chain.

**The price ladder** (`include/ob/price_ladder.hpp`). The best price only moves when its level empties. The next best is then found in the occupancy bitmap, 64 prices per word with `std::countr_zero` / `std::countl_zero`, so a wide gap costs a few word reads instead of a tick-by-tick walk. When a price lands outside the array, the array is rebuilt around the occupied prices plus the new one, with slack on both sides. Levels hold only slab indices, so they are simply copied across. Empty regions are dropped during rebuilds, which lets a side's price range drift over time.

**The trade callback is a template parameter.** `BasicOrderBook<OnTrade>` stores the callback by its own type, so a lambda is called directly and can be inlined into the matching loop. `std::function` would cost an indirect call per trade that the compiler cannot see through. `ob::OrderBook` is `BasicOrderBook<std::function<void(const Trade&)>>`, compiled once in `src/order_book.cpp`, for code that wants one concrete type; the tests use it and the benchmark uses the lambda version. The two sides have different types (bids are best-highest, asks best-lowest), so `match` is a member template that serves both with one body.

**`modify` moves the order instead of cancelling it.** When a modify changes the price or raises the quantity, the order loses its queue priority. It behaves exactly like cancel + add, but the order keeps its slab node and index entry. It is unlinked from its level, matched as a fresh taker, and whatever is left is linked onto the back of its new level. That saves an index erase and re-insert and a free + allocate per price change.

**`reserve(n)`** pre-sizes the slab and the bucket array, so nothing grows (no reallocation, no rehash) during trading. In the first version, growing the id index mid-run caused the millisecond outliers (see below).

**Branch hints.** The rare paths (`qty <= 0`, the price-band rejection, the slab or index needing to grow) are marked `[[unlikely]]`, so the compiler keeps them out of the straight-line hot path. Profile-guided optimisation makes these mostly redundant.

**Testing.** 43 unit tests cover each rule (resting, cancel, fills at the maker's price, level walking, FIFO, market, IOC, modify keeping or losing priority, bad input), the ladder (levels millions of ticks apart, cancels after the array grows, gaps across bitmap word boundaries, the price-band rejection), the slab and index (a million consecutive ids, node reuse, ids sharing a bucket), a moved modify that fills completely, and the lambda-typed book against the `std::function` one. Two differential tests drive the real book and `tests/reference_book.hpp`, a deliberately naive book that keeps every order in one `std::vector` and scans it, with the same seeded stream:

- 100,000 operations with prices in a narrow 95–105 band, so the sides cross constantly (about 52,000 trades);
- 30,000 operations where the mid price drifts and occasionally jumps 20,000 ticks, so the ladders keep growing and re-centring.

After every operation each test compares the return value, the trades emitted, best bid and ask, five levels of depth on each side and the order count. Breaking a rule on purpose makes them fail fast:

- filling at the taker's price fails at operation 3;
- not copying a level's total during a ladder rebuild fails at operation 132;
- not updating a moved order's price fails at operation 12;
- leaving a level's tail stale after an unlink, or never clearing a level's bitmap bit, crashes the tests.

CI runs everything under AddressSanitizer + UBSan, with `_GLIBCXX_ASSERTIONS` so that an out-of-range slab index fails loudly instead of silently reading another order. It also runs a build with `OB_NATIVE` and `OB_LTO`, and a PGO build (`scripts/pgo_build.sh`, then every test on the profile-optimised binaries) so the profile-driven pipeline cannot rot unnoticed.

Layout:

```
include/ob/types.hpp           Price, Quantity, Side, OrderType, Order, Trade, LevelView
include/ob/order_book.hpp      BasicOrderBook / OrderBook: the slab, the id index, matching
include/ob/price_ladder.hpp    tick-indexed price levels and occupancy bitmap for one side
src/order_book.cpp             compiles the std::function OrderBook once
src/replay/main.cpp            replay / interactive tool
examples/basic_usage.cpp       minimal program using the book
bench/bench.cpp                latency benchmark
scripts/pgo_build.sh           profile-guided build of the benchmark
tests/                         unit tests, reference book, differential tests
data/sample.csv                example replay input
```

## Benchmark

**Method.** One million operations are generated up front from a seeded RNG (70% limit adds with prices ~ Normal(10,000, 30) ticks and quantity 1–100; 20% cancels and 10% modifies of one of the last 1,024 ids added, some of which have already filled), so generation is never timed. 100,000 warm-up operations run first; the book peaks at about 110,000 resting orders. Each timed operation is bracketed by `std::chrono::steady_clock::now()`, and adds are split into those that rested without trading and those that traded. Throughput comes from a second, untimed pass over the same operations on a fresh book. The whole measurement runs three times on fresh books and the median run by throughput is reported.

### Native macOS, end of round 2 (median run of 3)

MacBook Air (Apple Silicon), macOS, Apple Clang 21.0 (`clang-2100.3.34.2`).

| Build | Throughput | ns per op | Resting add mean | Worst single op (3 runs) |
| --- | ---: | ---: | ---: | ---: |
| End of round 1 (`std::list` + pool) | 44.7 M ops/s | 22.4 | 22.5 ns | 11–19 µs |
| End of round 2, `-O3` (CMake Release) | **54.9 M ops/s** | 18.2 | 22.9 ns | 2.3–27 µs |
| End of round 2, PGO + `OB_NATIVE` + `OB_LTO` (`scripts/pgo_build.sh`) | **61.9 M ops/s** | 16.1 | 18.6 ns | 1.2–46 µs |

- **Round 2 is worth +23% natively**, about the same as in the VM. PGO adds a further +13% here, against +23% in the VM. Apple Clang's default output seems to leave less on the table.
- **The resting-add mean barely moved while throughput rose.** The mean includes the two `now()` calls around each operation (13.6–15.7 ns here), which now cost almost as much as the operation itself. Throughput is the figure to compare.
- **Run 1 of both builds had a p99.9 of ~1.5 µs in every operation type**, while runs 2 and 3 measured 42–84 ns for resting adds. As before, a tail that hits everything at once points at the machine (macOS scheduling, performance vs efficiency cores) rather than the book.

### Round 2: slab, index, callback, modify, bitmap, compiler (Linux VM, median of 3 runs)

MacBook Air (Apple Silicon) inside a 4-vCPU aarch64 Ubuntu 22.04 VM; GCC 11.4, CMake Release. Every row was measured in one session, building each commit in turn.

| Version | Throughput | ns per op | Worst single op (3 runs) |
| --- | ---: | ---: | ---: |
| Start of round 2 (`std::list` + pool, `unordered_map`, tick ladder) | 36.7 M ops/s | 27.2 | 15–224 µs |
| + intrusive slab and chained id index | 44.2 M ops/s | 22.7 | 53–170 µs |
| + callback as a template parameter | 51.2 M ops/s | 19.5 | 14–81 µs |
| + modify moves the node | 54.0 M ops/s | 18.5 | 8–81 µs |
| + occupancy bitmap | 52.4 M ops/s | 19.1 | 21–60 µs |
| + `OB_NATIVE` + `OB_LTO` | 54.8 M ops/s | 18.2 | 7–11 µs |
| + profile-guided optimisation | **64.5 M ops/s** | **15.5** | 11–35 µs |

- **The slab was the biggest structural win (+20%).** Each order went from two heap nodes in two containers to one 40-byte node in one array, and the index from a node per entry to a 4-byte bucket.
- **The template callback was worth +16%.** The benchmark's callback just counts trades, so once it is inlined it costs almost nothing. With `std::function` it was an indirect call on every fill.
- **The bitmap made no difference on this workload**, within run-to-run noise. Prices here are Normal(10,000, 30), so the next occupied level is almost always one or two ticks away, and the old tick-by-tick walk was already short. The bitmap is there for sparse books, where the walk could be thousands of ticks. That is a correctness-of-performance fix, not a benchmark win, and the table says so.
- **PGO was worth +23% over the default build.** That is the largest single number in this round, but it was trained on the same kind of synthetic workload it is measured on, so read it as "what PGO can do for this workload".
- **Per-operation means are no longer reported for this round.** At 15–19 ns per operation the work is now smaller than the cost of the two `now()` calls around it (18–30 ns in the VM, and it varies between sessions), so subtracting the overhead gives zero or noise. Throughput needs no clock per operation, so it is the honest figure now. Resolving individual operations would need a cycle counter (`rdtsc` on x86), which Apple Silicon does not expose to programs.

### Native macOS, end of round 1 (median run of 3)

Measured before round 2, so these rows describe the `std::list` + pool version. MacBook Air (Apple Silicon), macOS, Apple Clang 21.0 (`clang-2100.3.34.2`), `-O3 -DNDEBUG` (CMake Release).

| Build | Throughput | Resting add mean | Mean − overhead | Worst single op (3 runs) |
| --- | ---: | ---: | ---: | ---: |
| pool allocator + `reserve()` | **44.7 M ops/s** | **22.5 ns** | 9.0 ns | **11–19 µs** |
| plain `operator new` + `reserve()` | 29.4 M ops/s | 37.3 ns | 23.8 ns | 13–18 µs |
| pool allocator, no `reserve()` | 29.8 M ops/s | 33.1 ns | 19.5 ns | 0.71–0.79 ms |

| Operation (pool build) | Count | Mean (ns) | Mean − clock overhead (ns) | p99 (ns) | p99.9 (ns) | Max (ns) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| add, rests (no trade) | 399,509 | 22.5 | 9.0 | 42 | 792 | 11,916 |
| add, trades | 300,468 | 28.3 | 14.9 | 84 | 791 | 12,916 |
| cancel | 200,129 | 17.6 | 4.2 | 42 | 750 | 18,750 |
| modify | 99,894 | 19.1 | 5.7 | 83 | 708 | 3,458 |

- Natively, the pool is worth **52%** more throughput (44.7 vs 29.4 M ops/s), against 35% in the Linux VM, which suggests each `malloc` call costs more on macOS than under glibc.
- `reserve()` matters here too: without it the worst op is back to ~0.75 ms and throughput falls by a third.
- p50 is omitted: most operations finish inside one 41.7 ns clock tick, so their start and end timestamps often fall in the same tick and p50 reads as 0. The mean is the meaningful per-operation figure.
- The ~750–790 ns p99.9 appears in **every** operation type in the reported run, while the other two runs measured 167–208 ns for resting adds. A tail that hits everything at once points at the machine (macOS scheduling, or moving the process between performance and efficiency cores, which macOS does not let a program pin) rather than at the book's code.
- The op stream is not bit-identical to the Linux runs below (606,442 trades here vs 607,762): `std::normal_distribution` and `std::uniform_int_distribution` are implemented differently by libc++ and libstdc++, so the same seed gives a statistically identical but not identical workload.

### Round 1: the first latency work (Linux VM, pool build, median of 3 runs)

Apple Silicon Mac, inside a 4-vCPU aarch64 Ubuntu 22.04 VM; GCC 11.4, `-O3 -DNDEBUG` (CMake Release), otherwise idle. Every row was measured in one session, building each commit in turn, so the rows compare with each other.

| Version | Throughput | Resting add mean | Resting add p99 | Resting add p99.9 | Worst single op (3 runs) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `std::map` levels, pool for list nodes only | 20.7 M ops/s | 76.8 ns | 125 ns | 541 ns | 0.60–0.79 ms |
| + `reserve()`, pool for index nodes | 28.3 M ops/s | 61.7 ns | 125 ns | 250 ns | 51–514 µs |
| + tick-indexed price levels | **38.4 M ops/s** | **43.6 ns** | 125 ns | **209 ns** | **14–26 µs** |

The same steps with plain `operator new` instead of the pool: 19.6 → 22.7 → 28.4 M ops/s, worst op 1.3–2.7 ms → 13–36 µs → 14–94 µs. At the final version the pool is worth 35% more throughput (38.4 vs 28.4 M ops/s).

### End of round 1 in full (Linux VM, pool build, median run)

| Operation | Count | Mean (ns) | Mean − clock overhead (ns) | p50 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| add, rests (no trade) | 398,018 | 43.6 | 25.8 | 42 | 125 | 209 | 13,917 |
| add, trades | 301,941 | 50.5 | 32.6 | 42 | 125 | 375 | 13,000 |
| cancel | 200,010 | 26.9 | 9.0 | 41 | 83 | 84 | 10,959 |
| modify | 100,031 | 31.1 | 13.2 | 41 | 84 | 167 | 3,958 |

### Reading the round 1 numbers

- **The millisecond outliers were the id index rehashing.** At the end of round 1, `OB_BENCH_NO_RESERVE=1` brings them straight back (worst op 0.74–1.6 ms over three runs, 27 M ops/s), and `reserve()` removes them (12–16 µs, 38 M ops/s). The plain-`new` build shows the same thing, so it is the rehash and not the pool. That was the hypothesis in the previous version of this README, now tested.
- **Tick-indexed levels were the biggest single win:** +36% throughput and a 30% lower mean for resting adds, because the best level is cached and finding any level is an array index rather than a tree walk.
- **Clock resolution limits the percentiles.** On Apple Silicon `steady_clock` ticks every 41.7 ns (a 24 MHz counter), so p50 is "one tick" for every operation, and p99 values of 84 / 125 / 167 ns are 2 / 3 / 4 ticks. Percentiles are now reported mainly for the tail.
- **The mean is accurate below one tick.** Each sample is rounded to a whole tick, but where an operation starts relative to the next tick is effectively random, so the rounding averages out over hundreds of thousands of samples. "Mean − clock overhead" subtracts the measured cost of the two `now()` calls (~18 ns here). Throughput needs no clock per operation and is the cleanest number: 38.4 M ops/s is 26 ns per operation.
- **The worst-case column varies run to run** (one Tier 1 run hit 514 µs while the other two stayed near 50–95 µs), which is consistent with the VM being descheduled. That is why it is reported as a range.
- The native macOS numbers were faster than the VM numbers across the board at the end of round 1. The VM tables are kept because each one measures every step on one setup.

## Limitations and next steps

- **Single-threaded by design.** The next step is an SPSC lock-free ring buffer between a feed-parsing thread and the matcher, each pinned to its own core.
- **No self-trade prevention.** An order can trade against a resting order from the same participant; the book has no notion of participants.
- **Duplicate ids are not detected.** Adding an id that is already resting corrupts the index; the caller must keep ids unique.
- **The id index assumes ids roughly count up**, as exchange order ids do. Ids that were all multiples of a large power of two would share a few buckets and make chains long. If ids can be arbitrary, mix them in the bucket function (at the cost of locality).
- **Price band per side.** A side's resting prices must fit in 2^22 ticks. The bitmap is one level; for books that are very sparse across that whole band, a second summary level (one bit per 64-bit word) would bound every search to a few reads.
- **Memory is never returned.** The slab and the ladders only grow; freed nodes are reused, not released. That is fine for one trading session.
- **The benchmark's op stream depends on the standard library** (see the native section). Replacing `std::normal_distribution` and `std::uniform_int_distribution` with hand-written ones would make the workload identical on every platform.
- **The workload is synthetic.** Replaying real exchange data (for example a NASDAQ TotalView-ITCH sample file) would make the numbers more representative than any further tuning on the synthetic stream.
