#pragma once        // guard: safe to include from several files
#include <cstddef>  // std::size_t
#include <cstdint>  // std::int64_t, std::uint64_t
#include <ostream>  // operator<< so GoogleTest can print values on failure

namespace ob {      // the test refers to ob::OrderBook, so everything lives here

using Price = std::int64_t;         // integer ticks (cents), never double
using OrderId = std::uint64_t;
using Quantity = std::int64_t;      // signed: an over-subtraction shows up as a negative
using Sequence = std::uint64_t;     // assigned by the book on arrival, never by the caller

enum class OrderType : std::uint8_t { Limit, Market, IOC };
enum class Side : std::uint8_t { Buy, Sell };

struct Order {
    OrderId id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price{};
    Quantity qty{};     // remaining quantity while the order rests
    Sequence seq{};
};

// The taker is the incoming order, the maker is the resting one,
// and the price is always the maker's.
struct Trade {
    OrderId taker_id{};
    OrderId maker_id{};
    Price price{};
    Quantity qty{};
    bool operator==(const Trade&) const = default;
};

// One price level as reported to the outside.
struct LevelView {
    Price price{};
    Quantity qty{};
    std::size_t orders{};
    bool operator==(const LevelView&) const = default;
};

// A buy crosses a resting price at or below its own; a sell crosses one at or above.
constexpr bool crosses(Side taker_side, Price taker_price, Price resting_price) noexcept {
    return taker_side == Side::Buy ? resting_price <= taker_price
                                   : resting_price >= taker_price;
}

inline std::ostream& operator<<(std::ostream& os, const Trade& t) {
    return os << "Trade{taker=" << t.taker_id << ", maker=" << t.maker_id
              << ", price=" << t.price << ", qty=" << t.qty << "}";
}

inline std::ostream& operator<<(std::ostream& os, const LevelView& l) {
    return os << "LevelView{price=" << l.price << ", qty=" << l.qty
              << ", orders=" << l.orders << "}";
}

}  // namespace ob
