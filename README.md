# order-book

[![ci](https://github.com/aa3aaa/order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/aa3aaa/order-book/actions/workflows/ci.yml)

A limit order book and matching engine in C++20: price-time priority, limit / market / IOC orders, and O(1) cancel and modify through an order-id index. It is checked by 29 GoogleTest unit tests, 5 allocator tests and a 100,000-operation differential test against a naive reference book, and measured by a latency benchmark with and without a custom pool allocator.

**Results** (Apple Silicon Mac, Linux VM, GCC 11.4, `-O3`): **20.6 M ops/s**; resting add p50 83 ns, p99 209 ns, p99.9 0.67 µs; the pool allocator cut resting-add p99 by 37% versus `operator new`. Full table and method [below](#benchmark).

## Matching rules

1. A limit order matches against the opposite side while prices cross, best price first and oldest order first within a price (price-time priority). Each trade is for `min(taker remaining, maker remaining)` at the **maker's** price.
2. A maker whose quantity reaches zero is removed from its level and from the id index; an empty level is removed from the book.
3. Whatever remains of a limit order rests at its price with a fresh sequence number. Market and IOC remainders are cancelled.
4. A market order ignores its price and matches until filled or the opposite side is empty.
5. `modify` keeps queue priority only when the price is unchanged and the quantity does not go up (adjusted in place). A price change or a quantity increase removes the order and re-adds it as a new limit order, which may match immediately.
6. Self-trading is not prevented (see [Limitations](#limitations-and-next-steps)).

Input rules: `add` ignores orders with `qty <= 0`; `modify` returns false for an unknown id or `new_qty <= 0` and leaves the book unchanged; `cancel` returns false for an id that is not resting. Order ids must be unique among resting orders.

## Design decisions

**Prices are integer ticks** (cents), never `double`: comparisons are exact and an integer is a cheap map key. `Price` and `Quantity` are signed 64-bit so that a bug which subtracts too much shows up as a negative number an assert can catch, rather than an unsigned value silently wrapping to ~1.8 × 10^19. Ids and sequence numbers are unsigned. The book stamps the sequence number on arrival; the caller never sets it.

**Three data structures, each doing the one job it is fast at:**

| Structure | Type | Why |
| --- | --- | --- |
| Price levels, one map per side | `std::map<Price, Level, std::greater<>>` for bids, `std::map<Price, Level>` for asks | Sorted best-first, so `begin()` is always the best level and matching walks forward. O(log L) in the number of levels |
| Orders inside a level | `std::list<Order>` (FIFO, oldest at the front) plus a running `total` | List iterators stay valid when other nodes are inserted or erased, so the index can point straight at an order's node and remove it in O(1) |
| Lookup by id | `std::unordered_map<OrderId, Handle>`, `Handle = {side, price, list iterator}` | O(1) cancel and modify: find the handle, jump to the node, fix the level total. No scan of the level |

**Trades are reported through a callback** (`std::function<void(const Trade&)>`) in the order they happen; the book never prints or logs. The two maps have different types, so `match` is a member template that serves both sides with one body.

**Pool allocator** (`include/ob/pool_allocator.hpp`). `std::list` calls `new` for every node, and `new` is the classic latency spike: it can take a lock or page-fault. `PoolAllocator` hands out fixed-size blocks from large chunks, with the free list threaded through the unused blocks themselves; `allocate` and `deallocate` are an O(1) pop and push with no system call. What gets allocated is the list's *node* (an `Order` plus two pointers), not the `Order`, which is why the allocator has a rebinding constructor: the list rebinds `PoolAllocator<Order>` to its node type and the pool's block size follows. When the pool runs out it grows by another chunk (doubling, up to 1M blocks) rather than falling back to `new`, and memory is reused, never returned to the OS. The allocator is stateless, so all instances compare equal and lists can splice freely.

**Testing.** 29 unit tests cover each rule (resting, cancel, fills at the maker's price, level walking, FIFO, market, IOC, modify keeping or losing priority, bad input). The differential test (`tests/differential_test.cpp`) drives the real book and `tests/reference_book.hpp`, a deliberately naive book that keeps every order in one `std::vector` and scans it, with the same seeded stream of 100,000 operations (65% add, 20% cancel, 15% modify; prices in a narrow 95–105 band so the sides cross constantly; about 52,000 trades). After every operation it compares the return value, the trades emitted, best bid and ask, five levels of depth on each side and the order count. Breaking one rule on purpose (filling at the taker's price) makes it fail at operation 3. Every test runs twice, against the pool build and the plain-`new` build, and CI runs both under AddressSanitizer + UBSan. The plain build is the one that lets ASan see every node, because the pool recycles freed nodes instead of returning them to `malloc`.

## Build and run

Requires a C++20 compiler (GCC 11+ or Apple Clang), CMake 3.21+, and internet access on the first configure (GoogleTest is downloaded into `build/`).

```bash
# release build and tests
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure

# one test while working on it
./build/release/tests/ob_tests --gtest_filter='BookTest.Cancel*'

# AddressSanitizer + UBSan build (the differential test runs 20,000 ops here)
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DOB_SANITIZE=ON
cmake --build build/asan -j && ctest --test-dir build/asan --output-on-failure

# replay a CSV of orders: prints every trade and the final book
./build/release/replay data/sample.csv

# benchmark, with and without the pool allocator
./build/release/bench
./build/release/bench_nopool
```

The differential test reads `OB_DIFF_OPS` and `OB_DIFF_SEED` from the environment, so other streams are one command away: `OB_DIFF_SEED=7 ./build/release/tests/ob_tests --gtest_filter='Diff*'`.

Replay input format (see `data/sample.csv`):

```
A,id,side,price,qty   limit order (side is B or S)
M,id,side,qty         market order
I,id,side,price,qty   immediate-or-cancel
C,id                  cancel
X,id,price,qty        modify
```

Layout:

```
include/ob/types.hpp           Price, Quantity, Side, OrderType, Order, Trade, LevelView
include/ob/order_book.hpp      OrderBook interface
include/ob/pool_allocator.hpp  fixed-size pool and the std-conforming allocator
src/order_book.cpp             implementation
src/replay/main.cpp            CSV replay tool
bench/bench.cpp                latency benchmark
tests/                         unit tests, reference book, differential test, pool tests
data/sample.csv                example replay input
```

## Benchmark

**Method.** One million operations are generated up front from a seeded RNG (70% limit adds with prices ~ Normal(10,000, 30) ticks and quantity 1–100; 20% cancels and 10% modifies of one of the last 1,024 ids added, some of which have already filled), so generation is never timed. 100,000 warm-up operations run first. Each timed operation is bracketed by `std::chrono::steady_clock::now()`, and adds are split into those that rested without trading and those that traded. Throughput comes from a second, untimed pass over the same operations on a fresh book. Each binary was run three times; the table shows the middle run by throughput.

**Machine.** Apple Silicon Mac, inside a 4-vCPU aarch64 Ubuntu 22.04 VM; GCC 11.4, `-O3 -DNDEBUG` (CMake Release), on mains power, otherwise idle.

| Build | Operation | p50 (ns) | p99 (ns) | p99.9 (ns) |
| --- | --- | ---: | ---: | ---: |
| `std::list` + `new` | add, rests | 83 | 333 | 875 |
| | add, trades | 83 | 250 | 417 |
| | cancel | 42 | 125 | 250 |
| | modify | 42 | 208 | 292 |
| pool allocator | add, rests | 83 | **209** | **667** |
| | add, trades | 42 | 209 | 541 |
| | cancel | 42 | 125 | 375 |
| | modify | 42 | 208 | 417 |

| Build | Throughput (untimed pass) |
| --- | ---: |
| `std::list` + `new` | 17.0 M ops/s |
| pool allocator | **20.6 M ops/s** |

**Reading the numbers.**

- The pool cut resting-add p99 from 333 to 209 ns (−37%) and lifted throughput by 21%; p50 did not move. That is the expected shape: `new` is usually fast and occasionally slow, so a pool fixes the tail, not the median. The p99 drop held in all three runs (333/333/292 ns without the pool vs 208/209/167 ns with it).
- **Clock resolution matters here.** On this machine `steady_clock` ticks every 41.7 ns (a 24 MHz counter), so 42 and 83 ns mean "one tick" and "two ticks". The p50 rows are at the limit of what this method can resolve. The p99 and p99.9 rows, and the throughput, are the meaningful comparison.
- Every figure includes the cost of the `now()` calls themselves, so the absolute latencies are conservative.
- Single operations occasionally take ~1 ms (the `max` column printed by the benchmark). These outliers are not yet explained; candidates are an `unordered_map` rehash of the id index, or the VM being descheduled.
- Numbers from a native macOS build (Apple Clang) or a bare-metal Linux box will differ.

## Limitations and next steps

- **Single-threaded by design.** The next step is an SPSC lock-free ring buffer between a feed-parsing thread and the matcher, each pinned to its own core.
- **No self-trade prevention.** An order can trade against a resting order from the same participant; the book has no notion of participants.
- **Duplicate ids are not detected.** Adding an id that is already resting corrupts the index; the caller must keep ids unique.
- **Price levels live in a `std::map`** (O(log L)). A tick-indexed `std::vector` of levels around the mid would make best-level access O(1).
- **The trade callback is a `std::function`**, an indirect call on the hot path. A template parameter would let the compiler inline it.
- **The pool is not thread-safe** and never returns memory to the OS; both are fine for a single-threaded book but would need revisiting with the SPSC design.
- **Unexplained ~1 ms outliers.** Next: `reserve()` the id index up front and re-measure to test the rehash hypothesis.
