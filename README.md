# order-book

[![ci](https://github.com/aa3aaa/order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/aa3aaa/order-book/actions/workflows/ci.yml)

A limit order book and matching engine in C++20: price-time priority, limit / market / IOC orders, O(1) cancel and modify through an order-id index, and tick-indexed price levels. It is checked by 35 GoogleTest unit tests, 5 allocator tests and two differential tests against a naive reference book (100,000 operations in a narrow band, 30,000 with wide, drifting prices), and measured by a latency benchmark with and without a custom pool allocator.

**Results** (MacBook Air, Apple Silicon, native macOS, Apple Clang 21, `-O3`): **44.7 M ops/s**; resting add mean 22.5 ns; worst single operation 11–19 µs over 1M operations. That is up from 20.7 M ops/s and 0.6–0.8 ms worst-case before the latency work described [below](#benchmark).

## Using the order book

### Build and test

Requires a C++20 compiler (GCC 11+ or Apple Clang), CMake 3.21+, and internet access on the first configure (GoogleTest is downloaded into `build/`).

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure       # all tests, both builds of the book

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
./build/release/bench_nopool         # same, with plain operator new instead of the pool
OB_BENCH_NO_RESERVE=1 ./build/release/bench   # without reserve(), to see the difference
```

Always run it from the Release build, on mains power, with the machine otherwise idle.

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

**Three data structures, each doing the one job it is fast at:**

| Structure | Type | Why |
| --- | --- | --- |
| Price levels, one per side | `PriceLadder`: a `std::vector<Level>` indexed by `price - base`, with the best price cached | Finding a level is an array index, and the best level is always at hand, so matching never searches a tree. Replaced a `std::map` (O(log L)); see the benchmark history |
| Orders inside a level | `std::list<Order>` (FIFO, oldest at the front) plus a running `total` | List iterators stay valid when other nodes are inserted or erased, so the index can point straight at an order's node and remove it in O(1) |
| Lookup by id | `std::unordered_map<OrderId, Handle>`, `Handle = {side, price, list iterator}` | O(1) cancel and modify: find the handle, jump to the node, fix the level total. No scan of the level |

**The price ladder** (`include/ob/price_ladder.hpp`). The best price only moves when its level empties; the next best is then found by stepping one tick toward worse prices, a walk as long as the gap to the next occupied level, which is short in a liquid book. When a price lands outside the array, the array is rebuilt around the occupied prices plus the new one, with slack on both sides. Each level's orders move across with `list::splice`, which keeps every iterator valid, so the id index never has to be rebuilt. Empty regions are dropped during rebuilds, which lets a side's price range drift over time.

**Trades are reported through a callback** (`std::function<void(const Trade&)>`) in the order they happen; the book never prints or logs. The two sides have different types (bids are best-highest, asks best-lowest), so `match` is a member template that serves both with one body.

**Pool allocator** (`include/ob/pool_allocator.hpp`). `std::list` and `std::unordered_map` call `new` for every node, and `new` is the classic latency spike: it can take a lock or page-fault. `PoolAllocator` hands out fixed-size blocks from large chunks, with the free list threaded through the unused blocks themselves; `allocate` and `deallocate` are an O(1) pop and push with no system call. Both the order lists and the id index use it. What gets allocated is each container's *node* (an `Order` plus two pointers, or a hash-map entry), not the `Order`, which is why the allocator has a rebinding constructor: the container rebinds `PoolAllocator<Order>` to its node type and the pool's block size follows. When a pool runs out it grows by another chunk (doubling, up to 1M blocks) rather than falling back to `new`, and memory is reused, never returned to the OS. The allocator is stateless, so all instances compare equal and lists can splice freely.

**`reserve(n)`** pre-sizes everything that would otherwise grow during trading: the id index gets its buckets up front (no rehash mid-run), and in the pool build both node pools are grown to `n` blocks by allocating and freeing that many nodes. This is what removed the millisecond outliers (see below).

**Testing.** 35 unit tests cover each rule (resting, cancel, fills at the maker's price, level walking, FIFO, market, IOC, modify keeping or losing priority, bad input) plus the ladder (levels millions of ticks apart, cancels after the array grows, skipping gaps, the price-band rejection). Two differential tests drive the real book and `tests/reference_book.hpp`, a deliberately naive book that keeps every order in one `std::vector` and scans it, with the same seeded stream:

- 100,000 operations with prices in a narrow 95–105 band, so the sides cross constantly (about 52,000 trades);
- 30,000 operations where the mid price drifts and occasionally jumps 20,000 ticks, so the ladders keep growing and re-centring.

After every operation each test compares the return value, the trades emitted, best bid and ask, five levels of depth on each side and the order count. Breaking a rule on purpose makes them fail fast: filling at the taker's price fails at operation 3, and not copying a level's total during a ladder rebuild fails at operation 132. Every test runs twice, against the pool build and the plain-`new` build, and CI runs both under AddressSanitizer + UBSan. The plain build is the one that lets ASan see every node, because the pool recycles freed nodes instead of returning them to `malloc`.

Layout:

```
include/ob/types.hpp           Price, Quantity, Side, OrderType, Order, Trade, LevelView
include/ob/order_book.hpp      OrderBook interface
include/ob/price_ladder.hpp    tick-indexed price levels for one side
include/ob/pool_allocator.hpp  fixed-size pool and the std-conforming allocator
src/order_book.cpp             implementation
src/replay/main.cpp            replay / interactive tool
examples/basic_usage.cpp       minimal program using the book
bench/bench.cpp                latency benchmark
tests/                         unit tests, reference book, differential tests, pool tests
data/sample.csv                example replay input
```

## Benchmark

**Method.** One million operations are generated up front from a seeded RNG (70% limit adds with prices ~ Normal(10,000, 30) ticks and quantity 1–100; 20% cancels and 10% modifies of one of the last 1,024 ids added, some of which have already filled), so generation is never timed. 100,000 warm-up operations run first; the book peaks at about 110,000 resting orders. Each timed operation is bracketed by `std::chrono::steady_clock::now()`, and adds are split into those that rested without trading and those that traded. Throughput comes from a second, untimed pass over the same operations on a fresh book. The whole measurement runs three times on fresh books and the median run by throughput is reported.

### Native macOS, current version (median run of 3)

MacBook Air (Apple Silicon), macOS, Apple Clang 21.0 (`clang-2100.3.34.2`), `-O3 -DNDEBUG` (CMake Release).

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

### History of the latency work (Linux VM, pool build, median of 3 runs)

Apple Silicon Mac, inside a 4-vCPU aarch64 Ubuntu 22.04 VM; GCC 11.4, `-O3 -DNDEBUG` (CMake Release), otherwise idle. Every row was measured in one session, building each commit in turn, so the rows compare with each other.

| Version | Throughput | Resting add mean | Resting add p99 | Resting add p99.9 | Worst single op (3 runs) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `std::map` levels, pool for list nodes only | 20.7 M ops/s | 76.8 ns | 125 ns | 541 ns | 0.60–0.79 ms |
| + `reserve()`, pool for index nodes | 28.3 M ops/s | 61.7 ns | 125 ns | 250 ns | 51–514 µs |
| + tick-indexed price levels | **38.4 M ops/s** | **43.6 ns** | 125 ns | **209 ns** | **14–26 µs** |

The same steps with plain `operator new` instead of the pool: 19.6 → 22.7 → 28.4 M ops/s, worst op 1.3–2.7 ms → 13–36 µs → 14–94 µs. At the final version the pool is worth 35% more throughput (38.4 vs 28.4 M ops/s).

### Current version in full (Linux VM, pool build, median run)

| Operation | Count | Mean (ns) | Mean − clock overhead (ns) | p50 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| add, rests (no trade) | 398,018 | 43.6 | 25.8 | 42 | 125 | 209 | 13,917 |
| add, trades | 301,941 | 50.5 | 32.6 | 42 | 125 | 375 | 13,000 |
| cancel | 200,010 | 26.9 | 9.0 | 41 | 83 | 84 | 10,959 |
| modify | 100,031 | 31.1 | 13.2 | 41 | 84 | 167 | 3,958 |

### Reading the numbers

- **The millisecond outliers were the id index rehashing.** On the current version, `OB_BENCH_NO_RESERVE=1` brings them straight back (worst op 0.74–1.6 ms over three runs, 27 M ops/s), and `reserve()` removes them (12–16 µs, 38 M ops/s). The plain-`new` build shows the same thing, so it is the rehash and not the pool. That was the hypothesis in the previous version of this README, now tested.
- **Tick-indexed levels were the biggest single win:** +36% throughput and a 30% lower mean for resting adds, because the best level is cached and finding any level is an array index rather than a tree walk.
- **Clock resolution limits the percentiles.** On Apple Silicon `steady_clock` ticks every 41.7 ns (a 24 MHz counter), so p50 is "one tick" for every operation, and p99 values of 84 / 125 / 167 ns are 2 / 3 / 4 ticks. Percentiles are now reported mainly for the tail.
- **The mean is accurate below one tick.** Each sample is rounded to a whole tick, but where an operation starts relative to the next tick is effectively random, so the rounding averages out over hundreds of thousands of samples. "Mean − clock overhead" subtracts the measured cost of the two `now()` calls (~18 ns here). Throughput needs no clock per operation and is the cleanest number: 38.4 M ops/s is 26 ns per operation.
- **The worst-case column varies run to run** (one Tier 1 run hit 514 µs while the other two stayed near 50–95 µs), which is consistent with the VM being descheduled. That is why it is reported as a range.
- The native macOS numbers above are faster than these VM numbers across the board; the VM table is kept because it is the only place where every step was measured on one setup.

## Limitations and next steps

- **Single-threaded by design.** The next step is an SPSC lock-free ring buffer between a feed-parsing thread and the matcher, each pinned to its own core.
- **No self-trade prevention.** An order can trade against a resting order from the same participant; the book has no notion of participants.
- **Duplicate ids are not detected.** Adding an id that is already resting corrupts the index; the caller must keep ids unique.
- **Price band per side.** A side's resting prices must fit in 2^22 ticks. A sparse book (a few orders very far apart) makes the "next best" walk long; a bitmap of occupied levels would make it a few word scans.
- **The trade callback is a `std::function`**, an indirect call on the hot path. A template parameter would let the compiler inline it, at the cost of making `OrderBook` a header-only class template. Not done yet; measure before and after.
- **The id index is still a node-based hash map.** A flat open-addressing table (or an array indexed by id, when ids are dense) would remove a pointer chase from every cancel and modify.
- **The benchmark's op stream depends on the standard library** (see the native section). Replacing `std::normal_distribution` and `std::uniform_int_distribution` with hand-written ones would make the workload identical on every platform.
- **The pool is not thread-safe** and never returns memory to the OS; both are fine for a single-threaded book but would need revisiting with the SPSC design.
