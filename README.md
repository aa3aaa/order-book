# order-book

A limit order book and matching engine in C++20: price-time priority, limit / market / IOC orders, O(1) cancel and modify, with a differential test against a naive reference implementation and a latency benchmark.

**Status: in progress** (started 19 Oct 2026)

- [x] Build system (CMake 3.21+, GoogleTest via FetchContent, optional ASan/UBSan build)
- [ ] Core book: rest, cancel, best bid/ask, depth
- [ ] Matching: limit, market, IOC; modify
- [ ] Reference book + differential test (100k random ops)
- [ ] Replay tool (CSV in, trades out)
- [ ] Benchmark harness (p50 / p99 / p99.9)
- [ ] Pool allocator for order nodes, measured against `new`

## Matching rules

1. A limit order matches against the opposite side while prices cross, best price first and oldest order first within a price (price-time priority). Each trade is for `min(taker remaining, maker remaining)` at the **maker's** price.
2. A maker whose quantity reaches zero is removed from its level and from the id index; an empty level is removed from the book.
3. Whatever remains of a limit order rests at its price with a fresh sequence number. Market and IOC remainders are cancelled.
4. A market order ignores its price and matches until filled or the opposite side is empty.
5. `modify` keeps queue priority only when the price is unchanged and the quantity goes down (adjusted in place). A price change or a quantity increase removes the order and re-adds it as a new limit order, which may match immediately.
6. Self-trading is not prevented (see Limitations).

## Design decisions

- Prices are integer ticks (cents), never `double`: comparisons are exact and an integer is a cheap map key.
- `Price` and `Quantity` are signed 64-bit so that a bug which subtracts too much shows up as a negative number an assert can catch, rather than an unsigned value silently wrapping. Ids and sequence numbers are unsigned.
- The book assigns the sequence number on arrival; the caller never sets it.
- Price levels live in one `std::map` per side, sorted best-first (bids descending, asks ascending), so `begin()` is always the best level.
- Orders inside a level sit in a `std::list` (FIFO) whose iterators stay valid when other nodes are erased, so the id index can point straight at an order's node.
- An `std::unordered_map<OrderId, Handle>` gives O(1) cancel and modify: find the handle, erase that node, fix the level total. No scan of the level.
- Trades are reported through a callback in the order they happen; the book itself never prints or logs.

## Build and run

Requires a C++20 compiler (Apple Clang or GCC 13), CMake 3.21+ and internet access for the first configure (GoogleTest is downloaded into `build/`).

```bash
# release build and tests
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure

# AddressSanitizer + UBSan build
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DOB_SANITIZE=ON
cmake --build build/asan -j && ctest --test-dir build/asan --output-on-failure
```

Layout:

```
include/ob/types.hpp       Price, Quantity, Side, OrderType, Order, Trade, LevelView
include/ob/order_book.hpp  OrderBook interface
src/order_book.cpp         implementation
tests/                     GoogleTest unit tests and the differential test
```

## Benchmark

Measured on the release build with one million pre-generated operations after a warm-up; each operation is timed with `std::chrono::steady_clock`, so roughly 20–40 ns of timer overhead is included in every figure.

| Operation | ops/s | p50 (ns) | p99 (ns) | p99.9 (ns) |
| --- | --- | --- | --- | --- |
| add (std::list + new) | TBD | TBD | TBD | TBD |
| add (pool allocator) | TBD | TBD | TBD | TBD |
| cancel | TBD | TBD | TBD | TBD |
| modify | TBD | TBD | TBD | TBD |

Machine: TBD (CPU, compiler version, flags).

## Limitations and next steps

- Single-threaded by design; the next step is an SPSC lock-free ring buffer between a feed-parsing thread and the matcher, pinned to two cores.
- No self-trade prevention.
- Price levels in a `std::map` (O(log L)); a tick-indexed `std::vector` of levels is the planned optimisation once the benchmark shows where time goes.
- The trade callback is a `std::function`; a template parameter would remove the indirect call on the hot path.
