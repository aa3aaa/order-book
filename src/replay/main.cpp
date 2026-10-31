// replay: feed a CSV of orders through the book, print every trade and the final book.
//
//   ./build/release/replay data/sample.csv
//
// Line format (blank lines and lines starting with # are skipped):
//   A,id,side,price,qty   limit order (side is B or S)
//   M,id,side,qty         market order
//   I,id,side,price,qty   immediate-or-cancel
//   C,id                  cancel
//   X,id,price,qty        modify
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ob/order_book.hpp"

namespace {

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) fields.push_back(field);
    return fields;
}

ob::Side parse_side(const std::string& s) {
    if (s == "B") return ob::Side::Buy;
    if (s == "S") return ob::Side::Sell;
    throw std::invalid_argument("side must be B or S, got '" + s + "'");
}

void expect_fields(const std::vector<std::string>& f, std::size_t n) {
    if (f.size() != n)
        throw std::invalid_argument("expected " + std::to_string(n) + " fields, got " + std::to_string(f.size()));
}

// Applies one CSV line to the book. Throws std::exception on malformed input.
void apply_line(ob::OrderBook& book, const std::string& line) {
    const auto f = split(line);
    if (f.empty()) throw std::invalid_argument("empty line");
    const std::string& op = f[0];

    if (op == "A" || op == "I") {
        expect_fields(f, 5);
        ob::Order o;
        o.id = std::stoull(f[1]);
        o.side = parse_side(f[2]);
        o.type = op == "A" ? ob::OrderType::Limit : ob::OrderType::IOC;
        o.price = std::stoll(f[3]);
        o.qty = std::stoll(f[4]);
        book.add(o);
    } else if (op == "M") {
        expect_fields(f, 4);
        ob::Order o;
        o.id = std::stoull(f[1]);
        o.side = parse_side(f[2]);
        o.type = ob::OrderType::Market;
        o.qty = std::stoll(f[3]);
        book.add(o);
    } else if (op == "C") {
        expect_fields(f, 2);
        const auto id = std::stoull(f[1]);
        if (!book.cancel(id)) std::cout << "  cancel " << id << ": no such resting order\n";
    } else if (op == "X") {
        expect_fields(f, 4);
        const auto id = std::stoull(f[1]);
        if (!book.modify(id, std::stoll(f[2]), std::stoll(f[3])))
            std::cout << "  modify " << id << ": rejected (unknown id or qty <= 0)\n";
    } else {
        throw std::invalid_argument("unknown operation '" + op + "'");
    }
}

// Prints the book as a ladder: asks highest to lowest, then bids highest to lowest.
void dump(const ob::OrderBook& book, std::ostream& out) {
    constexpr std::size_t all = static_cast<std::size_t>(-1);
    auto asks = book.depth(ob::Side::Sell, all);
    std::reverse(asks.begin(), asks.end());
    out << "--- book (" << book.order_count() << " resting orders) ---\n";
    for (const auto& l : asks) out << "  ASK " << l.price << "  qty " << l.qty << "  orders " << l.orders << "\n";
    out << "  -----\n";
    for (const auto& l : book.depth(ob::Side::Buy, all))
        out << "  BID " << l.price << "  qty " << l.qty << "  orders " << l.orders << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <orders.csv>\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }

    ob::OrderBook book([](const ob::Trade& t) {
        std::cout << "TRADE taker=" << t.taker_id << " maker=" << t.maker_id
                  << " price=" << t.price << " qty=" << t.qty << "\n";
    });

    std::string line;
    std::size_t line_no = 0, errors = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate Windows line endings
        if (line.empty() || line[0] == '#') continue;
        try {
            apply_line(book, line);
        } catch (const std::exception& e) {
            std::cerr << argv[1] << ":" << line_no << ": " << e.what() << ": " << line << "\n";
            ++errors;
        }
    }

    dump(book, std::cout);
    return errors == 0 ? 0 : 1;
}
