// The smallest useful program built on the order book: create a book, send it
// orders, react to trades, read the top of book. Build and run:
//
//   cmake --build build/release --target example && ./build/release/example
#include <iostream>
#include <vector>

#include "ob/order_book.hpp"

int main() {
    // 1. The book reports every execution through a callback, in the order it happens.
    std::vector<ob::Trade> trades;
    ob::OrderBook book([&trades](const ob::Trade& t) { trades.push_back(t); });

    // Optional: pre-size for the most orders you expect to rest at once, so no
    // container grows (and stalls) in the middle of trading.
    book.reserve(10'000);

    // 2. Orders are plain structs. Prices are integer ticks (here, cents).
    auto limit = [](ob::OrderId id, ob::Side side, ob::Price price, ob::Quantity qty) {
        ob::Order o;
        o.id = id;
        o.side = side;
        o.type = ob::OrderType::Limit;
        o.price = price;
        o.qty = qty;
        return o;
    };

    book.add(limit(1, ob::Side::Sell, 10'050, 300));  // ask $100.50 x 300
    book.add(limit(2, ob::Side::Sell, 10'060, 200));  // ask $100.60 x 200
    book.add(limit(3, ob::Side::Buy, 10'000, 500));   // bid $100.00 x 500

    // 3. Read the book.
    auto show_top = [&] {
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        std::cout << "best bid " << (bid ? bid->price : 0) << " x " << (bid ? bid->qty : 0)
                  << " | best ask " << (ask ? ask->price : 0) << " x " << (ask ? ask->qty : 0) << "\n";
    };
    show_top();

    // 4. An aggressive buy for 400 at up to $100.60 sweeps two levels; add() returns
    //    whatever is left resting (0 here: fully filled).
    const ob::Quantity rested = book.add(limit(4, ob::Side::Buy, 10'060, 400));
    std::cout << "order 4 left resting: " << rested << "\n";
    for (const auto& t : trades) std::cout << "  " << t << "\n";
    show_top();

    // 5. Cancel and modify by id.
    book.modify(3, 10'000, 250);  // same price, smaller size: keeps its place in the queue
    book.cancel(2);               // false if it is no longer resting
    std::cout << "five best bid levels:\n";
    for (const auto& level : book.depth(ob::Side::Buy, 5)) std::cout << "  " << level << "\n";
}
